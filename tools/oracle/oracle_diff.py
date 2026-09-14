#!/usr/bin/env python3
"""Differential clean-room runner for Sony SceShaccCg vs open-shacccg.

Sony GXP bytes live only in memory by default. The report contains observable
metadata, hashes and USSE instruction words/families, never the proprietary
module itself.
"""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile
from typing import Any

HERE = pathlib.Path(__file__).resolve().parent
TOOLS = HERE.parent
ROOT = TOOLS.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(TOOLS))

import gxp_info  # noqa: E402
import usse_info  # noqa: E402
from unicorn_oracle import Oracle  # noqa: E402


METADATA_KEYS = (
    "stage", "logical_size", "program_flags", "buffer_flags", "texunit_flags",
    "registers", "buffers", "compiler_version_raw", "literals_count",
    "uniform_buffer_count", "dependent_sampler_count",
    "texture_buffer_dependent_sampler_count", "container_count",
)
PARAMETER_KEYS = (
    "name", "category", "type", "component_count", "container_index",
    "semantic", "semantic_index", "array_size", "resource_index",
)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def stage_for_profile(profile: str) -> str:
    if profile == "sce_fp_psp2":
        return "fragment"
    if profile == "sce_vp_psp2":
        return "vertex"
    raise ValueError(f"unsupported corpus profile: {profile}")


def normalized_parameters(parsed: dict[str, Any]) -> list[dict[str, Any]]:
    return [{key: row[key] for key in PARAMETER_KEYS} for row in parsed["parameters"]]


def phase_words(parsed: dict[str, Any], phase: str) -> list[str]:
    return [row["qword"] for row in parsed[phase]["instructions"]]


def phase_families(parsed: dict[str, Any], phase: str) -> list[str]:
    return [usse_info.classify(row)[1] for row in parsed[phase]["instructions"]]


def observable_summary(data: bytes) -> dict[str, Any]:
    parsed = gxp_info.parse(data)
    return {
        "sha256": sha256(data),
        "size": len(data),
        "guids": parsed["guids"],
        "metadata": {key: parsed[key] for key in METADATA_KEYS},
        "parameters": normalized_parameters(parsed),
        "varyings_hex": parsed["varyings"]["hex"],
        "secondary": {
            "qwords": phase_words(parsed, "secondary_program"),
            "families": phase_families(parsed, "secondary_program"),
        },
        "primary": {
            "qwords": phase_words(parsed, "primary_program"),
            "families": phase_families(parsed, "primary_program"),
        },
    }


def equal_except_guids(a: bytes, b: bytes) -> bool:
    if len(a) != len(b):
        return False
    return a[:0x0C] == b[:0x0C] and a[0x14:] == b[0x14:]


def compare_observable(sony_data: bytes, open_data: bytes) -> dict[str, Any]:
    sony = observable_summary(sony_data)
    opened = observable_summary(open_data)
    metadata_diff = []
    for key in METADATA_KEYS:
        a = sony["metadata"][key]
        b = opened["metadata"][key]
        if a != b:
            metadata_diff.append({"field": key, "sony": a, "open": b})
    if sony["parameters"] != opened["parameters"]:
        metadata_diff.append({"field": "parameters", "sony": sony["parameters"], "open": opened["parameters"]})
    if sony["varyings_hex"] != opened["varyings_hex"]:
        metadata_diff.append({"field": "varyings_hex", "sony": sony["varyings_hex"], "open": opened["varyings_hex"]})

    instruction_diff = []
    for phase in ("secondary", "primary"):
        aq = sony[phase]["qwords"]
        bq = opened[phase]["qwords"]
        af = sony[phase]["families"]
        bf = opened[phase]["families"]
        for index in range(max(len(aq), len(bq))):
            aw = aq[index] if index < len(aq) else None
            bw = bq[index] if index < len(bq) else None
            if aw == bw:
                continue
            instruction_diff.append({
                "phase": phase,
                "index": index,
                "sony": aw,
                "open": bw,
                "sony_family": af[index] if index < len(af) else None,
                "open_family": bf[index] if index < len(bf) else None,
            })

    return {
        "byte_equal_except_guids": equal_except_guids(sony_data, open_data),
        "metadata_equal": not metadata_diff,
        "family_equal": sony["secondary"]["families"] == opened["secondary"]["families"] and
                        sony["primary"]["families"] == opened["primary"]["families"],
        "qword_equal": not instruction_diff,
        "metadata_diff": metadata_diff,
        "instruction_diff": instruction_diff,
    }


def compile_sony(case: dict[str, Any], source: str, elf: pathlib.Path,
                 imports: pathlib.Path, traps: pathlib.Path, trace: bool) -> tuple[dict[str, Any], bytes]:
    oracle = Oracle(elf, imports, traps, trace=trace)
    output = 0
    try:
        output, gxp, diagnostics = oracle.compile_source(source, stage_for_profile(case["profile"]))
        result = {
            "ok": bool(gxp) and not any(d.get("level", 0) >= 2 for d in diagnostics),
            "diagnostics": diagnostics,
        }
        if gxp:
            result.update(observable_summary(gxp))
        return result, gxp
    finally:
        try:
            if output:
                oracle.call_export(0xAA82EF0C, output)
            oracle.call_export(0x95F57A23)
        except Exception:
            # Cleanup must not hide the actual probe/compiler failure.
            pass


def compile_open(case: dict[str, Any], source_path: pathlib.Path,
                 compiler: pathlib.Path, temp_root: pathlib.Path) -> tuple[dict[str, Any], bytes]:
    output = temp_root / f"{case['name']}.open.gxp"
    proc = subprocess.run(
        [str(compiler), "--stage", stage_for_profile(case["profile"]), str(source_path), str(output)],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    data = output.read_bytes() if output.exists() else b""
    result: dict[str, Any] = {
        "ok": proc.returncode == 0 and bool(data),
        "returncode": proc.returncode,
        "stdout": proc.stdout,
        "stderr": proc.stderr,
    }
    if data:
        result.update(observable_summary(data))
    return result, data


def select_cases(cases: list[dict[str, Any]], names: list[str], features: list[str], limit: int | None) -> list[dict[str, Any]]:
    selected = []
    for case in cases:
        if names and not any(fnmatch.fnmatchcase(case["name"], pattern) for pattern in names):
            continue
        if features and case.get("feature") not in features:
            continue
        selected.append(case)
        if limit is not None and len(selected) >= limit:
            break
    return selected


def default_open_compiler() -> pathlib.Path | None:
    for relative in ("build-full/openshacccg_compile", "build-cross/openshacccg_compile", "build/openshacccg_compile"):
        candidate = ROOT / relative
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def print_human(rows: list[dict[str, Any]], have_open: bool) -> None:
    for index, row in enumerate(rows, 1):
        sony = row["sony"]
        line = f"[{index}/{len(rows)}] {row['name']}: sony={'ok' if sony['ok'] else 'FAIL'}"
        if sony.get("size") is not None:
            line += f" {sony['size']}B"
        if have_open:
            opened = row["open"]
            line += f" open={'ok' if opened['ok'] else 'FAIL'}"
            comparison = row.get("comparison")
            if comparison:
                line += " exact" if comparison["byte_equal_except_guids"] else (
                    f" delta(meta={len(comparison['metadata_diff'])},usse={len(comparison['instruction_diff'])})"
                )
        print(line)
        comparison = row.get("comparison")
        if comparison and comparison["instruction_diff"]:
            for diff in comparison["instruction_diff"][:3]:
                print(f"    {diff['phase']}[{diff['index']}]: {diff['sony']} {diff['sony_family']} -> "
                      f"{diff['open']} {diff['open_family']}")
            if len(comparison["instruction_diff"]) > 3:
                print(f"    ... {len(comparison['instruction_diff']) - 3} more instruction differences")


def summarize(rows: list[dict[str, Any]], have_open: bool) -> dict[str, int]:
    summary = {
        "cases": len(rows),
        "sony_ok": sum(1 for row in rows if row["sony"]["ok"]),
    }
    if have_open:
        comparisons = [row.get("comparison") for row in rows]
        summary.update({
            "open_ok": sum(1 for row in rows if row["open"]["ok"]),
            "byte_equal_except_guids": sum(1 for comparison in comparisons if comparison and comparison["byte_equal_except_guids"]),
            "metadata_equal": sum(1 for comparison in comparisons if comparison and comparison["metadata_equal"]),
            "family_equal": sum(1 for comparison in comparisons if comparison and comparison["family_equal"]),
            "qword_equal": sum(1 for comparison in comparisons if comparison and comparison["qword_equal"]),
        })
    return summary


def main() -> int:
    ap = argparse.ArgumentParser(description="Run clean-room Sony/OpenShaccCg differential probes")
    ap.add_argument("corpus", type=pathlib.Path)
    ap.add_argument("--sony-elf", type=pathlib.Path,
                    default=os.environ.get("OPENSHACCG_ORACLE_ELF"))
    ap.add_argument("--imports", type=pathlib.Path, default=ROOT / "oracle_imports.json")
    ap.add_argument("--traps", type=pathlib.Path, default=ROOT / "oracle_trap_map.json")
    ap.add_argument("--open-compiler", type=pathlib.Path)
    ap.add_argument("--name", action="append", default=[], help="shell-style case-name pattern; repeatable")
    ap.add_argument("--feature", action="append", default=[], help="manifest feature filter; repeatable")
    ap.add_argument("--limit", type=int)
    ap.add_argument("--report", type=pathlib.Path, help="write complete JSON report")
    ap.add_argument("--json", action="store_true", help="print JSON report instead of human summary")
    ap.add_argument("--trace", action="store_true", help="trace Sony import calls")
    ap.add_argument("--strict-open", action="store_true", help="fail if OpenShaccCg fails or differs")
    args = ap.parse_args()

    if not args.sony_elf:
        ap.error("--sony-elf or OPENSHACCG_ORACLE_ELF is required")
    args.sony_elf = pathlib.Path(args.sony_elf)
    for path, label in ((args.sony_elf, "Sony ELF"), (args.imports, "imports metadata"), (args.traps, "trap map")):
        if not pathlib.Path(path).is_file():
            ap.error(f"{label} not found: {path}")

    manifest_path = args.corpus / "manifest.json"
    if not manifest_path.is_file():
        ap.error(f"manifest not found: {manifest_path}")
    cases = json.loads(manifest_path.read_text())
    selected = select_cases(cases, args.name, args.feature, args.limit)
    if not selected:
        ap.error("no corpus cases matched the requested filters")

    open_compiler = args.open_compiler or default_open_compiler()
    if open_compiler is not None and not pathlib.Path(open_compiler).is_file():
        ap.error(f"OpenShaccCg compiler not found: {open_compiler}")

    rows = []
    sony_failed = False
    open_failed_or_diff = False
    with tempfile.TemporaryDirectory(prefix="open-shacccg-oracle-") as temp:
        temp_root = pathlib.Path(temp)
        for case in selected:
            source_path = args.corpus / case["file"]
            source = source_path.read_text()
            row: dict[str, Any] = {"name": case["name"], "profile": case["profile"], "feature": case.get("feature")}
            try:
                sony, sony_data = compile_sony(case, source, args.sony_elf, args.imports, args.traps, args.trace)
            except Exception as exc:  # keep a multi-case run useful after one failure
                sony, sony_data = {"ok": False, "error": str(exc)}, b""
            row["sony"] = sony
            sony_failed |= not sony["ok"]

            if open_compiler is not None:
                opened, open_data = compile_open(case, source_path, pathlib.Path(open_compiler), temp_root)
                row["open"] = opened
                if sony_data and open_data:
                    row["comparison"] = compare_observable(sony_data, open_data)
                comparison = row.get("comparison")
                open_failed_or_diff |= (not opened["ok"] or comparison is None or not comparison["byte_equal_except_guids"])
            rows.append(row)

    report = {
        "schema": 1,
        "corpus": str(args.corpus),
        "open_compiler": str(open_compiler) if open_compiler is not None else None,
        "cases": rows,
    }
    report["summary"] = summarize(rows, open_compiler is not None)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2) + "\n")
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print_human(rows, open_compiler is not None)
        summary = report["summary"]
        line = f"summary: sony={summary['sony_ok']}/{summary['cases']}"
        if open_compiler is not None:
            line += (f" open={summary['open_ok']}/{summary['cases']}"
                     f" exact={summary['byte_equal_except_guids']}"
                     f" metadata={summary['metadata_equal']}"
                     f" families={summary['family_equal']}"
                     f" qwords={summary['qword_equal']}")
        print(line)

    if sony_failed:
        return 1
    if args.strict_open and open_failed_or_diff:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
