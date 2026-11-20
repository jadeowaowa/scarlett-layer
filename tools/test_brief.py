#!/usr/bin/env python3
"""Build + run CTest and emit a ~10-line token-efficient summary (or exact failure block)."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def run_cmd(cmd: list[str]) -> tuple[int, str]:
    p = subprocess.run(
        cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace"
    )
    return p.returncode, p.stdout


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--focus", default="", help="Substring to select which passing test's telemetry to show (default: last)")
    args = ap.parse_args()

    if not args.no_build:
        rc, out = run_cmd(["cmake", "--build", "build"])
        if rc != 0:
            print("BUILD: FAIL")
            for line in out.splitlines()[-35:]:
                print(line)
            return rc

    rc, out = run_cmd(["ctest", "--test-dir", "build", "--output-on-failure", "-V"])
    lines = [re.sub(r"^\d+:\s?", "", ln) for ln in out.splitlines()]

    blocks: list[tuple[str, list[str], bool]] = []
    cur_name, cur_lines = "", []
    results_line, time_line = "", ""
    leak_lines: list[str] = []

    for ln in lines:
        if "=== Results:" in ln:
            results_line = ln.strip()
        elif "Total Test time" in ln:
            time_line = ln.strip()
        elif "DllMain[Unload]" in ln and ("[kernelx]" in ln or "[xmem]" in ln):
            leak_lines.append(ln.strip())

        m = re.match(r"[TEST]\s+(\S+)\s+\.\.\.(.*)", ln)
        if m:
            name, tail = m.group(1), m.group(2)
            if cur_name and cur_name != name:
                failed = any("[FAIL]" in x or x.strip().endswith("FAIL") for x in cur_lines)
                blocks.append((cur_name, cur_lines, failed))
                cur_lines = []
            cur_name = name
            cur_lines.append(ln)
            if tail.strip().endswith(("PASS", "FAIL")):
                failed = tail.strip().endswith("FAIL") or any("[FAIL]" in x for x in cur_lines)
                blocks.append((cur_name, cur_lines, failed))
                cur_name, cur_lines = "", []
        elif cur_name:
            cur_lines.append(ln)
            if ln.strip() in ("PASS", "FAIL"):
                failed = (ln.strip() == "FAIL") or any("[FAIL]" in x for x in cur_lines)
                blocks.append((cur_name, cur_lines, failed))
                cur_name, cur_lines = "", []

    if cur_name:
        failed = any("[FAIL]" in x or x.strip().endswith("FAIL") for x in cur_lines)
        blocks.append((cur_name, cur_lines, failed))

    failed_blocks = [b for b in blocks if b[2]]
    if rc != 0 or failed_blocks:
        print(f"BUILD: OK | CTEST: FAIL | {results_line or 'see output'}")
        for name, blines, _ in failed_blocks:
            print(f"--- FAIL: {name} ---")
            for x in blines[-25:]:
                print(x)
        if not failed_blocks:
            for x in lines[-25:]:
                print(x)
        return rc or 1

    print(f"BUILD: OK | {results_line} | {time_line}")
    target = None
    if args.focus:
        for b in blocks:
            if args.focus.lower() in b[0].lower():
                target = b
    if target is None and blocks:
        target = blocks[-1]
    if target:
        print(f"FOCUS [{target[0]}]:")
        for x in target[1]:
            if any(k in x for k in ("PSO CACHE", "SavePipelineLibrary", "Worker[Submit]", "teardown", "CreateCommandQueueX", "WARN")):
                print("  " + x.strip())
    for lk in leak_lines[-2:]:
        print("LEAK: " + lk)
    return 0


if __name__ == "__main__":
    sys.exit(main())