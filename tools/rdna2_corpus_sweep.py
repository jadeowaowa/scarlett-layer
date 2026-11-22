#!/usr/bin/env python3
"""Reproducible offline RDNA2 (gfx1030) corpus sweep driver for scarlett-layer."""

import argparse
import collections
import concurrent.futures
import json
import os
import pathlib
import re
import subprocess
import sys
import time

STAGE_RE = re.compile(r"_(cs|ps|vs|gs|hs|ds)_", re.IGNORECASE)


def detect_stage(path: pathlib.Path) -> str:
    m = STAGE_RE.search(path.name)
    if m:
        return m.group(1).lower()
    return "cs"


def normalize_error(err: str) -> str:
    err = (err or "").strip()
    if not err:
        return ""
    # Collapse specific FXC line/col numbers to group identical compiler errors
    err = re.sub(r"sweep\.hlsl\(\d+,\d+(?:-\d+)?\):", "sweep.hlsl(L,C):", err)
    err = re.sub(r"\s+", " ", err)
    return err[:140]


def sweep_one(sweep_exe: pathlib.Path, elf_path: pathlib.Path) -> dict:
    stage = detect_stage(elf_path)
    try:
        proc = subprocess.run(
            [str(sweep_exe), "--stage", stage, str(elf_path)],
            capture_output=True,
            text=True,
            timeout=30,
        )
    except subprocess.TimeoutExpired:
        return {
            "file": elf_path.name,
            "stage": stage,
            "decode_ok": False,
            "insts": 0,
            "strict_lift": False,
            "strict_compile": False,
            "compat_lift": False,
            "compat_compile": False,
            "error": "timeout (30s)",
            "strict_err": "",
            "compat_err": "",
        }

    line = (proc.stdout or "").strip().splitlines()
    raw_json = line[-1] if line else ""
    try:
        data = json.loads(raw_json)
    except json.JSONDecodeError:
        data = {
            "decode_ok": False,
            "insts": 0,
            "strict_lift": False,
            "strict_compile": False,
            "compat_lift": False,
            "compat_compile": False,
            "error": f"non-json output (rc={proc.returncode}): {raw_json[:100]}",
            "strict_err": "",
            "compat_err": "",
        }

    data["file"] = elf_path.name
    data["stage"] = stage
    return data


def pct(num: int, den: int) -> str:
    if den <= 0:
        return "0.0%"
    return f"{(100.0 * num / den):.1f}%"


def summarize(results: list[dict]) -> dict:
    by_stage = collections.defaultdict(lambda: {
        "total": 0,
        "decode_ok": 0,
        "strict_lift": 0,
        "strict_compile": 0,
        "compat_lift": 0,
        "compat_compile": 0,
        "insts": 0,
    })
    decode_errs = collections.Counter()
    strict_errs = collections.Counter()
    compat_errs = collections.Counter()

    for r in results:
        st = r["stage"]
        for key in (st, "ALL"):
            b = by_stage[key]
            b["total"] += 1
            if r.get("decode_ok"):
                b["decode_ok"] += 1
                b["insts"] += int(r.get("insts", 0))
            if r.get("strict_lift"):
                b["strict_lift"] += 1
            if r.get("strict_compile"):
                b["strict_compile"] += 1
            if r.get("compat_lift"):
                b["compat_lift"] += 1
            if r.get("compat_compile"):
                b["compat_compile"] += 1

        if not r.get("decode_ok"):
            decode_errs[normalize_error(r.get("error", "decode failed"))] += 1
        else:
            if not r.get("strict_compile"):
                strict_errs[normalize_error(r.get("strict_err", "unknown strict error"))] += 1
            if not r.get("compat_compile"):
                compat_errs[normalize_error(r.get("compat_err", "unknown compat error"))] += 1

    return {
        "by_stage": dict(by_stage),
        "decode_errs": decode_errs,
        "strict_errs": strict_errs,
        "compat_errs": compat_errs,
    }


def write_report(report_path: pathlib.Path, elf_dir: pathlib.Path, elapsed_s: float, summary: dict) -> None:
    by_stage = summary["by_stage"]
    stages = [s for s in ("cs", "ps", "vs", "gs", "hs", "ds") if s in by_stage]
    stages.append("ALL")

    lines = [
        "# RDNA2 (`gfx1030`) Corpus Sweep Report",
        "",
        "Measured offline using `build/bin/rdna2_sweep.exe` driven by `tools/rdna2_corpus_sweep.py`.",
        "",
        "## Corpus & Methodology",
        "",
        f"- **Corpus directory:** `{elf_dir}` (MIT-licensed HLSL shaders from `DirectX-Graphics-Samples` and `Xbox-GDK-Samples`, compiled via official Microsoft `dxc.exe -spirv` -> AMD RGA 2.9.1 `-s vk-spv-offline -c gfx1030` to real `gfx1030` ELF `.text` binaries).",
        f"- **Total real `gfx1030` ELFs swept:** **{by_stage['ALL']['total']}** (`{by_stage['ALL']['insts']}` decoded RDNA2 instructions total; wall time `{elapsed_s:.2f}s`).",
        "- **Modes measured per shader:**",
        "  - **Decode:** `sc::d3d12::rdna2::DecodeProgram()` decodes the full `.text` instruction stream cleanly.",
        "  - **Strict Lift / Compile:** `allowFallbackOps = false`; lifted HLSL compiled to DXBC via `D3DCompile` (`cs_5_0` / `ps_5_0` / `vs_5_0`).",
        "  - **Compat Lift / Compile:** `allowFallbackOps = true`; lifted HLSL compiled to DXBC via `D3DCompile`.",
        "",
        "## Results by Stage",
        "",
        "| Stage | ELFs | Decode OK | Strict Lift | Strict Compile (DXBC) | Compat Lift | Compat Compile (DXBC) | Decoded Insts |",
        "| :---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]

    for s in stages:
        b = by_stage[s]
        tot = b["total"]
        label = "**Total**" if s == "ALL" else f"`{s}`"
        lines.append(
            f"| {label} | {tot} | "
            f"{b['decode_ok']} ({pct(b['decode_ok'], tot)}) | "
            f"{b['strict_lift']} ({pct(b['strict_lift'], tot)}) | "
            f"{b['strict_compile']} ({pct(b['strict_compile'], tot)}) | "
            f"{b['compat_lift']} ({pct(b['compat_lift'], tot)}) | "
            f"{b['compat_compile']} ({pct(b['compat_compile'], tot)}) | "
            f"{b['insts']} |"
        )

    def add_err_table(title: str, counter: collections.Counter) -> None:
        lines.extend(["", f"## {title}", ""])
        if not counter:
            lines.append("None (0 failures).")
            return
        lines.extend(["| Count | Error |", "| ---: | :--- |"])
        for msg, cnt in counter.most_common(15):
            safe = msg.replace("|", "\\|")
            lines.append(f"| {cnt} | `{safe}` |")

    add_err_table("Top Decode Failures", summary["decode_errs"])
    add_err_table("Top Strict Lift/Compile Failures", summary["strict_errs"])
    add_err_table("Top Compat Lift/Compile Failures", summary["compat_errs"])

    lines.extend([
        "",
        "## Reproduction",
        "",
        "```powershell",
        "python tools/rdna2_corpus_sweep.py --elf-dir E:\\gta\\refs\\corpus_gfx1030 --report docs/RDNA2_CORPUS_REPORT.md",
        "```",
        "",
    ])

    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text("\n".join(lines), encoding="utf-8", newline="\n")


def main() -> int:
    ap = argparse.ArgumentParser(description="Sweep real gfx1030 ELFs through rdna2_sweep.exe")
    ap.add_argument("--sweep-exe", default="build/bin/rdna2_sweep.exe")
    ap.add_argument("--elf-dir", default=r"E:\gta\refs\corpus_gfx1030")
    ap.add_argument("--report", default="docs/RDNA2_CORPUS_REPORT.md")
    ap.add_argument("--workers", type=int, default=max(1, min(16, (os.cpu_count() or 4))))
    args = ap.parse_args()

    sweep_exe = pathlib.Path(args.sweep_exe).resolve()
    elf_dir = pathlib.Path(args.elf_dir).resolve()
    report_path = pathlib.Path(args.report).resolve()

    if not sweep_exe.is_file():
        print(f"ERROR: rdna2_sweep executable not found: {sweep_exe}", file=sys.stderr)
        return 2
    if not elf_dir.is_dir():
        print(f"ERROR: corpus directory not found: {elf_dir}", file=sys.stderr)
        return 2

    elfs = sorted(elf_dir.glob("*.elf"))
    if not elfs:
        print(f"ERROR: no .elf files found in {elf_dir}", file=sys.stderr)
        return 2

    t0 = time.perf_counter()
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as pool:
        futs = [pool.submit(sweep_one, sweep_exe, p) for p in elfs]
        for f in concurrent.futures.as_completed(futs):
            results.append(f.result())
    elapsed = time.perf_counter() - t0

    results.sort(key=lambda r: r["file"])
    summary = summarize(results)
    write_report(report_path, elf_dir, elapsed, summary)

    all_s = summary["by_stage"]["ALL"]
    print(f"Swept {all_s['total']} gfx1030 ELFs in {elapsed:.2f}s -> {report_path}")
    for st in ("cs", "ps", "vs", "ALL"):
        if st not in summary["by_stage"]:
            continue
        b = summary["by_stage"][st]
        tot = b["total"]
        print(
            f"  {st:>3}: n={tot:3d} | decode={b['decode_ok']:3d} ({pct(b['decode_ok'], tot):>5}) "
            f"| strict_comp={b['strict_compile']:3d} ({pct(b['strict_compile'], tot):>5}) "
            f"| compat_comp={b['compat_compile']:3d} ({pct(b['compat_compile'], tot):>5})"
        )
    if summary["decode_errs"]:
        print("Top decode errors:")
        for msg, cnt in summary["decode_errs"].most_common(5):
            print(f"  {cnt:3d}x {msg}")
    if summary["strict_errs"]:
        print("Top strict errors:")
        for msg, cnt in summary["strict_errs"].most_common(5):
            print(f"  {cnt:3d}x {msg}")
    if summary["compat_errs"]:
        print("Top compat errors:")
        for msg, cnt in summary["compat_errs"].most_common(5):
            print(f"  {cnt:3d}x {msg}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())