#!/usr/bin/env python3
"""Measure OpenShaccCg against pinned real-world Vita shader corpora.

External repositories are cloned only into an ignored local cache. Their source
is used as test input and is never copied into the runtime implementation.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import pathlib
import re
import shutil
import subprocess
import sys
from collections import Counter
from typing import Iterable, Optional


ROOT = pathlib.Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "research" / "real_world_corpus.json"
DEFAULT_CACHE = ROOT / ".real-corpus-cache"
DEFAULT_RESULTS = ROOT / ".real-corpus-results"


@dataclasses.dataclass(frozen=True)
class ShaderCase:
    project: str
    name: str
    stage: str
    origin: str
    source: Optional[str]
    notes: str = ""

    @property
    def case_id(self) -> str:
        return f"{self.project}:{self.name}"


def run_command(args: list[str], cwd: Optional[pathlib.Path] = None,
                check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, cwd=cwd, check=check, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def load_manifest(path: pathlib.Path) -> dict:
    data = json.loads(path.read_text())
    if data.get("version") != 1 or not isinstance(data.get("repositories"), list):
        raise RuntimeError(f"unsupported real-world corpus manifest: {path}")
    return data


def ensure_checkout(repo: dict, cache_root: pathlib.Path, fetch: bool) -> pathlib.Path:
    target = cache_root / repo["name"]
    revision = repo["revision"]
    if not (target / ".git").is_dir():
        if not fetch:
            raise RuntimeError(f"missing checkout {target}; rerun with --fetch")
        cache_root.mkdir(parents=True, exist_ok=True)
        run_command(["git", "clone", "--filter=blob:none", "--no-checkout",
                     repo["url"], str(target)])

    dirty = run_command(["git", "status", "--porcelain"], cwd=target).stdout.strip()
    if dirty:
        raise RuntimeError(f"external corpus checkout is dirty: {target}")
    head = run_command(["git", "rev-parse", "HEAD"], cwd=target).stdout.strip()
    if head != revision:
        if not fetch:
            raise RuntimeError(
                f"{repo['name']} is at {head}, expected {revision}; rerun with --fetch")
        run_command(["git", "fetch", "--depth", "1", "origin", revision], cwd=target)
        run_command(["git", "checkout", "--detach", "FETCH_HEAD"], cwd=target)
        head = run_command(["git", "rev-parse", "HEAD"], cwd=target).stdout.strip()
        if head != revision:
            raise RuntimeError(f"failed to select {repo['name']} revision {revision}")
    return target


def contains_main(source: str) -> bool:
    return re.search(r"\bmain\s*\(", source) is not None


def dsvita_cases(checkout: pathlib.Path) -> list[ShaderCase]:
    graphics = checkout / "src" / "core" / "graphics"
    common_path = graphics / "gpu_2d" / "shaders" / "cg" / "bg_frag_common.cg"
    common = common_path.read_text()
    variants = {
        "gpu_2d/shaders/cg/obj_frag.cg": [
            ("base", ""), ("BPP8", "#define BPP8\n"), ("BITMAP", "#define BITMAP\n")],
        "gpu_2d/shaders/cg/blend_frag.cg": [
            ("base", ""), ("BLEND_3D", "#define BLEND_3D\n")],
        "gpu_3d/shaders/cg/render_frag.cg": [
            ("base", ""), ("W_DEPTH_BUFFER", "#define W_DEPTH_BUFFER\n")],
    }
    cases: list[ShaderCase] = []
    for path in sorted(graphics.rglob("*.cg")):
        if path == common_path:
            continue
        source = path.read_text()
        if not contains_main(source):
            continue
        rel = path.relative_to(graphics).as_posix()
        stage = "vertex" if "_vert" in path.stem else "fragment"
        prefix = common if rel.startswith("gpu_2d/shaders/cg/bg_frag_") else ""
        for variant, define in variants.get(rel, [("base", "")]):
            name = rel[:-3].replace("/", "__")
            if variant != "base":
                name += "__" + variant
            cases.append(ShaderCase(
                "dsvita", name, stage, rel, define + prefix + source,
                "runtime preprocessor variant" if variant != "base" else ""))
    return cases


def extract_raw_string(path: pathlib.Path, variable: str) -> str:
    text = path.read_text()
    match = re.search(
        rf"const\s+char\s*\*\s*{re.escape(variable)}\s*=\s*R\"\((.*?)\)\"\s*;",
        text, re.DOTALL)
    if not match:
        raise RuntimeError(f"cannot extract {variable} from {path}")
    return match.group(1)


def extract_precompiled_comment(path: pathlib.Path) -> str:
    text = path.read_text()
    for match in re.finditer(r"/\*(.*?)\*/", text, re.DOTALL):
        body = match.group(1).strip()
        if contains_main(body):
            lines = [line.strip() for line in body.splitlines()]
            return "\n".join(lines).strip() + "\n"
    raise RuntimeError(f"cannot find source comment in {path}")


def vitagl_cases(checkout: pathlib.Path) -> list[ShaderCase]:
    shaders = checkout / "source" / "shaders"
    cases: list[ShaderCase] = []
    for stem, stage in (("clear_v", "vertex"), ("clear_f", "fragment"),
                        ("blit_v", "vertex"), ("blit_f", "fragment")):
        path = shaders / f"precompiled_{stem}.h"
        cases.append(ShaderCase(
            "vitagl", f"precompiled_{stem}", stage, path.relative_to(checkout).as_posix(),
            extract_precompiled_comment(path), "source retained beside the precompiled GXP"))

    vertex_template = extract_raw_string(shaders / "ffp_v.h", "ffp_vert_src")
    vertex_scenarios = {
        # clip, textures, colors, lights, lighting, shading, normalize,
        # fixed attr mask, fixed position mask, WVP-on-GPU, half interpolation
        "ffp_v_baseline": (0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        "ffp_v_color": (0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0),
        "ffp_v_texture1": (0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        "ffp_v_texture2_color": (0, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0),
        "ffp_v_lighting_smooth": (0, 1, 1, 1, 1, 0, 1, 0, 0, 1, 0),
        "ffp_v_lighting_phong": (0, 1, 1, 1, 1, 1, 1, 0, 0, 1, 0),
        "ffp_v_clip_wvp": (1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 0),
        "ffp_v_fixed_half": (0, 1, 1, 0, 0, 0, 0, 2, 3, 0, 1),
    }
    for name, values in vertex_scenarios.items():
        cases.append(ShaderCase("vitagl", name, "vertex", "source/shaders/ffp_v.h",
                                vertex_template % values, "concrete default-FFP state"))

    helpers = {
        "modulate": extract_raw_string(shaders / "tex_env.h", "modulate_src"),
        "replace": extract_raw_string(shaders / "tex_env.h", "replace_src"),
        "add": extract_raw_string(shaders / "tex_env.h", "add_src"),
    }
    fragment_template = extract_raw_string(shaders / "ffp_f.h", "ffp_frag_src")

    def frag(helper_names: Iterable[str], alpha: int, textures: int, colors: int,
             fog: int, pass0: int, pass1: int, lights: int, lighting: int,
             shading: int, point: int, interp: int, srgb: int) -> str:
        helper_source = "\n".join(helpers[name] for name in helper_names)
        return fragment_template % (
            helper_source, alpha, textures, colors, fog, pass0, pass1,
            lights, lighting, shading, point, interp, srgb)

    fragment_scenarios = {
        "ffp_f_baseline": ((), 7, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0),
        "ffp_f_color": ((), 7, 0, 1, 3, 0, 0, 0, 0, 0, 0, 0, 0),
        "ffp_f_texture1": (("modulate",), 7, 1, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0),
        "ffp_f_texture2": (("modulate", "replace"), 7, 2, 1, 3, 0, 4, 0, 0, 0, 0, 0, 0),
        "ffp_f_alpha": (("modulate",), 0, 1, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0),
        "ffp_f_fog_linear": ((), 7, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        "ffp_f_fog_exp2": ((), 7, 0, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0),
        "ffp_f_lighting_phong": ((), 7, 0, 1, 3, 0, 0, 1, 1, 1, 0, 0, 0),
        "ffp_f_point_sprite": (("modulate",), 7, 1, 0, 3, 0, 0, 0, 0, 0, 1, 0, 0),
        "ffp_f_srgb": ((), 7, 0, 1, 3, 0, 0, 0, 0, 0, 0, 0, 1),
    }
    for name, values in fragment_scenarios.items():
        cases.append(ShaderCase("vitagl", name, "fragment", "source/shaders/ffp_f.h",
                                frag(*values), "concrete default-FFP state"))

    ext_vertex_template = extract_raw_string(shaders / "ffp_ext_v.h", "ffp_vert_src")
    cases.append(ShaderCase(
        "vitagl", "ffp_ext_v_texture3", "vertex", "source/shaders/ffp_ext_v.h",
        ext_vertex_template % (0, 3, 1, 0, 0, 0, 0, 0, 0, 0, 0),
        "high-FFP-texture-units representative state"))

    ext_fragment_template = extract_raw_string(shaders / "ffp_ext_f.h", "ffp_frag_src")
    helper_source = "\n".join((helpers["modulate"], helpers["replace"], helpers["add"]))
    cases.append(ShaderCase(
        "vitagl", "ffp_ext_f_texture3", "fragment", "source/shaders/ffp_ext_f.h",
        ext_fragment_template % (helper_source, 7, 3, 1, 3, 0, 4, 3, 0, 0, 0, 0, 0, 0),
        "high-FFP-texture-units representative state"))
    return cases


GEOMETRIZER_SHADERS = (
    ("POLY_VS", "vertex", "src/video/gl/model1_3d_backend_gl_es.c", "oracle_corpus_v2/vp-geometrizer-poly.cg"),
    ("POLY_FS", "fragment", "src/video/gl/model1_3d_backend_gl_es.c", None),
    ("POLY3D_VS", "vertex", "src/video/gl/model1_3d_backend_gl_es.c", "oracle_corpus_v2/vp-geometrizer-poly3d.cg"),
    ("POLY3D_OBJ_LUT_VS", "vertex", "src/video/gl/model1_3d_backend_gl_es.c", None),
    ("POLY3D_LUT_MOIRE_FS", "fragment", "src/video/gl/model1_3d_backend_gl_es.c", None),
    ("POLY3D_LUT_FS", "fragment", "src/video/gl/model1_3d_backend_gl_es.c", None),
    ("POLY3D_STATIC_VS", "vertex", "src/video/gl/model1_3d_backend_gl_es.c", None),
    ("TM2_VS", "vertex", "src/video/gl/model1_tilemap_gl_es.c", None),
    ("TM2_FS", "fragment", "src/video/gl/model1_tilemap_gl_es.c", None),
    ("TM2_FAST_VS", "vertex", "src/video/gl/model1_tilemap_gl_es.c", None),
    ("TM2_FAST_FS", "fragment", "src/video/gl/model1_tilemap_gl_es.c", "oracle_corpus_v2/fp-geometrizer-tm2-fast.cg"),
    ("TM2_ROWS_VS", "vertex", "src/video/gl/model1_tilemap_gl_es.c", None),
    ("CMP_VS", "vertex", "src/video/gl/model1_tilemap_gl_es.c", None),
    ("CMP_FS", "fragment", "src/video/gl/model1_tilemap_gl_es.c", "oracle_corpus_v2/fp-geometrizer-cmp.cg"),
)


def geometrizer_cases(checkout: pathlib.Path) -> list[ShaderCase]:
    cases: list[ShaderCase] = []
    for name, stage, origin, fixture in GEOMETRIZER_SHADERS:
        external_text = (checkout / origin).read_text()
        if not re.search(rf"\b{re.escape(name)}\s*\[\]\s*=", external_text):
            raise RuntimeError(f"Geometrizer shader {name} is missing at pinned revision")
        source = (ROOT / fixture).read_text() if fixture else None
        notes = ("clean Cg fixture derived from the real Vita shader" if fixture else
                 "capture gap: vitaGL GLSL-to-Cg translation not recorded yet")
        cases.append(ShaderCase("geometrizer", name, stage, f"{origin}:{name}", source, notes))
    return cases


ADAPTERS = {
    "dsvita": dsvita_cases,
    "vitagl": vitagl_cases,
    "geometrizer": geometrizer_cases,
}


def safe_name(case: ShaderCase) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", case.name)


def diagnostic_category(stderr: str) -> tuple[str, Optional[int], str]:
    first = next((line.strip() for line in stderr.splitlines() if line.strip()), "")
    match = re.search(r"0x([0-9A-Fa-f]+)", first)
    code = int(match.group(1), 16) if match else None
    if code is not None and 0x1000 <= code <= 0x10FF:
        category = "frontend"
    elif code is not None and 0x2100 <= code <= 0x21FF:
        category = "spirv"
    elif code is not None and 0x2200 <= code <= 0x22FF:
        category = "backend"
    else:
        category = "unknown"
    message = first.split(": ", 1)[1] if ": " in first else first
    return category, code, message[:500]


def run_cases(cases: list[ShaderCase], compiler: pathlib.Path,
              results_root: pathlib.Path) -> list[dict]:
    sources_root = results_root / "sources"
    gxp_root = results_root / "gxp"
    logs_root = results_root / "logs"
    for path in (sources_root, gxp_root, logs_root):
        path.mkdir(parents=True, exist_ok=True)

    results: list[dict] = []
    for case in cases:
        row = {
            "id": case.case_id,
            "project": case.project,
            "name": case.name,
            "stage": case.stage,
            "origin": case.origin,
            "notes": case.notes,
        }
        if case.source is None:
            row.update(status="capture-gap", category="capture", diagnostic_code=None,
                       message="vitaGL GLSL-to-Cg translation has not been captured")
            results.append(row)
            continue
        project_sources = sources_root / case.project
        project_gxp = gxp_root / case.project
        project_logs = logs_root / case.project
        for path in (project_sources, project_gxp, project_logs):
            path.mkdir(parents=True, exist_ok=True)
        filename = safe_name(case)
        source_path = project_sources / f"{filename}.cg"
        gxp_path = project_gxp / f"{filename}.gxp"
        log_path = project_logs / f"{filename}.log"
        source_path.write_text(case.source)
        proc = run_command([str(compiler), "--stage", case.stage,
                            str(source_path), str(gxp_path)], check=False)
        log_path.write_text(proc.stderr)
        if proc.returncode == 0 and gxp_path.is_file() and gxp_path.stat().st_size:
            row.update(status="pass", category="pass", diagnostic_code=None,
                       message="", gxp_size=gxp_path.stat().st_size)
        else:
            category, code, message = diagnostic_category(proc.stderr)
            row.update(status="fail", category=category, diagnostic_code=code,
                       message=message)
        results.append(row)
    return results


def summarize(results: list[dict]) -> dict:
    summary: dict[str, dict] = {}
    for project in sorted({row["project"] for row in results}):
        rows = [row for row in results if row["project"] == project]
        captured = [row for row in rows if row["status"] != "capture-gap"]
        passed = [row for row in rows if row["status"] == "pass"]
        categories = Counter(row["category"] for row in rows if row["status"] != "pass")
        summary[project] = {
            "target_total": len(rows),
            "captured": len(captured),
            "passed": len(passed),
            "compile_rate": (len(passed) / len(captured)) if captured else 0.0,
            "target_rate": (len(passed) / len(rows)) if rows else 0.0,
            "gaps": dict(sorted(categories.items())),
        }
    rows = results
    captured = [row for row in rows if row["status"] != "capture-gap"]
    passed = [row for row in rows if row["status"] == "pass"]
    summary["overall"] = {
        "target_total": len(rows),
        "captured": len(captured),
        "passed": len(passed),
        "compile_rate": (len(passed) / len(captured)) if captured else 0.0,
        "target_rate": (len(passed) / len(rows)) if rows else 0.0,
        "gaps": dict(sorted(Counter(
            row["category"] for row in rows if row["status"] != "pass").items())),
    }
    return summary


def markdown_report(manifest: dict, summary: dict, results: list[dict]) -> str:
    lines = [
        "# Real-world shader coverage",
        "",
        "`compile rate` counts captured Cg inputs only. `target rate` also counts",
        "known shader inputs whose vitaGL GLSL-to-Cg translation has not been captured yet.",
        "",
        "| Project | Revision | Passed | Captured | Target | Compile rate | Target rate |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: |",
    ]
    revisions = {repo["name"]: repo["revision"] for repo in manifest["repositories"]}
    for project in [repo["name"] for repo in manifest["repositories"]] + ["overall"]:
        s = summary[project]
        revision = revisions.get(project, "-")
        lines.append(
            f"| {project} | `{revision[:12]}` | {s['passed']} | {s['captured']} | "
            f"{s['target_total']} | {s['compile_rate']:.1%} | {s['target_rate']:.1%} |")

    lines += ["", "## Gap categories", ""]
    for project in [repo["name"] for repo in manifest["repositories"]] + ["overall"]:
        gaps = summary[project]["gaps"]
        rendered = ", ".join(f"{key}={value}" for key, value in gaps.items()) or "none"
        lines.append(f"- **{project}:** {rendered}")

    lines += ["", "## Failing / uncaptured cases", "",
              "| Case | Category | Diagnostic |", "| --- | --- | --- |"]
    for row in results:
        if row["status"] == "pass":
            continue
        message = row["message"].replace("|", "\\|").replace("\n", " ")
        lines.append(f"| `{row['id']}` | {row['category']} | {message} |")
    lines.append("")
    return "\n".join(lines)


def write_baseline(path: pathlib.Path, manifest: dict, results: list[dict]) -> None:
    payload = {
        "version": 1,
        "revisions": {repo["name"]: repo["revision"] for repo in manifest["repositories"]},
        "passing_cases": sorted(row["id"] for row in results if row["status"] == "pass"),
    }
    path.write_text(json.dumps(payload, indent=2) + "\n")


def check_baseline(path: pathlib.Path, manifest: dict, results: list[dict]) -> list[str]:
    baseline = json.loads(path.read_text())
    expected_revisions = {repo["name"]: repo["revision"] for repo in manifest["repositories"]}
    errors: list[str] = []
    if baseline.get("revisions") != expected_revisions:
        errors.append("baseline revisions do not match real_world_corpus.json")
    passing = {row["id"] for row in results if row["status"] == "pass"}
    for case_id in baseline.get("passing_cases", []):
        if case_id not in passing:
            errors.append(f"regressed passing case: {case_id}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=pathlib.Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--cache-root", type=pathlib.Path, default=DEFAULT_CACHE)
    parser.add_argument("--results-root", type=pathlib.Path, default=DEFAULT_RESULTS)
    parser.add_argument("--compiler", type=pathlib.Path,
                        default=ROOT / "build-real" / "openshacccg_compile")
    parser.add_argument("--fetch", action="store_true",
                        help="clone/fetch the exact pinned external revisions")
    parser.add_argument("--project", action="append", choices=tuple(ADAPTERS),
                        help="limit the run to one or more projects")
    parser.add_argument("--write-baseline", type=pathlib.Path)
    parser.add_argument("--check-baseline", type=pathlib.Path)
    args = parser.parse_args()

    manifest = load_manifest(args.manifest)
    selected = set(args.project or ADAPTERS)
    repos = [repo for repo in manifest["repositories"] if repo["name"] in selected]
    if not repos:
        raise RuntimeError("no selected corpus repositories")
    if not args.compiler.is_file():
        raise RuntimeError(
            f"compiler not found: {args.compiler}; build the full host pipeline first")

    cases: list[ShaderCase] = []
    for repo in repos:
        checkout = ensure_checkout(repo, args.cache_root, args.fetch)
        adapter = ADAPTERS.get(repo["adapter"])
        if not adapter:
            raise RuntimeError(f"unknown corpus adapter {repo['adapter']}")
        cases.extend(adapter(checkout))

    if args.results_root.exists():
        shutil.rmtree(args.results_root)
    args.results_root.mkdir(parents=True)
    results = run_cases(cases, args.compiler.resolve(), args.results_root)
    summary = summarize(results)
    report = {
        "version": 1,
        "manifest": str(args.manifest),
        "repositories": repos,
        "summary": summary,
        "results": results,
    }
    (args.results_root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    (args.results_root / "REPORT.md").write_text(markdown_report(
        {"repositories": repos}, summary, results))

    for project in [repo["name"] for repo in repos] + ["overall"]:
        s = summary[project]
        print(f"{project:12} pass={s['passed']:3}/{s['captured']:3} captured "
              f"({s['compile_rate']:.1%}), target={s['passed']:3}/{s['target_total']:3} "
              f"({s['target_rate']:.1%})")

    if args.write_baseline:
        write_baseline(args.write_baseline, {"repositories": repos}, results)
    if args.check_baseline:
        errors = check_baseline(args.check_baseline, {"repositories": repos}, results)
        for error in errors:
            print(f"baseline: {error}", file=sys.stderr)
        if errors:
            return 2
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as exc:
        print(f"real_corpus.py: {exc}", file=sys.stderr)
        raise SystemExit(2)
