#!/usr/bin/env python3
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "oracle"))

import oracle_diff
import gxp_info


def main():
    sample = (ROOT / "research" / "public_samples" / "libvita2d" / "clear_f.gxp").read_bytes()

    same = oracle_diff.compare_observable(sample, sample)
    assert same["byte_equal_except_guids"]
    assert same["metadata_equal"]
    assert same["family_equal"]
    assert same["qword_equal"]

    guid_only = bytearray(sample)
    guid_only[0x0C] ^= 0x55
    guid_only[0x10] ^= 0xAA
    guid = oracle_diff.compare_observable(sample, bytes(guid_only))
    assert guid["byte_equal_except_guids"]
    assert guid["metadata_equal"]
    assert guid["qword_equal"]

    changed = bytearray(sample)
    primary = gxp_info.parse(sample)["primary_program"]
    changed[primary["offset"]] ^= 1
    code = oracle_diff.compare_observable(sample, bytes(changed))
    assert not code["byte_equal_except_guids"]
    assert not code["qword_equal"]
    assert len(code["instruction_diff"]) == 1
    assert code["instruction_diff"][0]["phase"] == "primary"

    cases = [
        {"name": "fp-if", "feature": "control"},
        {"name": "fp-loop", "feature": "control"},
        {"name": "fp-add", "feature": "alu"},
    ]
    selected = oracle_diff.select_cases(cases, ["fp-*"], ["control"], 1)
    assert [case["name"] for case in selected] == ["fp-if"]

    summary = oracle_diff.summarize([
        {"sony": {"ok": True}, "open": {"ok": True}, "comparison": same},
        {"sony": {"ok": True}, "open": {"ok": True}, "comparison": code},
    ], True)
    assert summary["sony_ok"] == 2 and summary["open_ok"] == 2
    assert summary["byte_equal_except_guids"] == 1 and summary["qword_equal"] == 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
