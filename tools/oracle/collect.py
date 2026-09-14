#!/usr/bin/env python3
"""Run an external clean-room oracle adapter over a shader corpus.

The adapter is intentionally external. It may be a QEMU-based runner, a real
Vita bridge, or another local executable. open-shacccg never links the Sony
module or emulator code.

Command placeholders: {input}, {output}, {profile}, {name}
"""
import argparse
import hashlib
import json
import pathlib
import shlex
import subprocess
import time


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus")
    ap.add_argument("out")
    ap.add_argument("--command", required=True,
                    help="adapter command template; placeholders: {input} {output} {profile} {name}")
    ap.add_argument("--limit", type=int)
    args = ap.parse_args()

    corpus = pathlib.Path(args.corpus)
    out_root = pathlib.Path(args.out)
    out_root.mkdir(parents=True, exist_ok=True)
    cases = json.loads((corpus / "manifest.json").read_text())
    if args.limit:
        cases = cases[:args.limit]

    results = []
    for i, case in enumerate(cases, 1):
        src = (corpus / case["file"]).resolve()
        dst = (out_root / f"{case['name']}.gxp").resolve()
        cmd_text = args.command.format(input=shlex.quote(str(src)),
                                       output=shlex.quote(str(dst)),
                                       profile=shlex.quote(case["profile"]),
                                       name=shlex.quote(case["name"]))
        started = time.monotonic()
        proc = subprocess.run(cmd_text, shell=True, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        elapsed_ms = round((time.monotonic() - started) * 1000, 3)
        row = dict(case)
        row.update({
            "returncode": proc.returncode,
            "elapsed_ms": elapsed_ms,
            "stdout": proc.stdout,
            "stderr": proc.stderr,
            "gxp": dst.name if dst.exists() else None,
            "gxp_size": dst.stat().st_size if dst.exists() else None,
            "gxp_sha256": sha256(dst) if dst.exists() else None,
        })
        results.append(row)
        print(f"[{i}/{len(cases)}] {case['name']}: rc={proc.returncode} "
              f"size={row['gxp_size']} {elapsed_ms:.1f}ms")
        (out_root / "results.json").write_text(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
