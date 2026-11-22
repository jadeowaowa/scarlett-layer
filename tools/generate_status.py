#!/usr/bin/env python3
"""
Deterministic status matrix generator and doc-drift verifier for scarlett-layer.

Parses CMakeLists.txt, src/shims/*/*.def, src/tests/scarlett_tests/main.cpp,
and docs/SPEC_SOURCES.md to generate docs/STATUS.md without volatile timestamps
so `python tools/generate_status.py --check` is reproducible in CI.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Dict, List, Set, Tuple

ROOT = Path(__file__).resolve().parent.parent
CMAKE_PATH = ROOT / "CMakeLists.txt"
TESTS_MAIN_PATH = ROOT / "src" / "tests" / "scarlett_tests" / "main.cpp"
SHIMS_DIR = ROOT / "src" / "shims"
SPEC_SOURCES_PATH = ROOT / "docs" / "SPEC_SOURCES.md"
STATUS_OUT_PATH = ROOT / "docs" / "STATUS.md"
README_PATH = ROOT / "README.md"
ROADMAP_PATH = ROOT / "docs" / "ROADMAP.md"

# Tests that exercise an opt-in local external corpus when environment variables
# are configured, falling back to synthetic inputs in default CI runs.
OPT_IN_EXTERNAL_TESTS: Set[str] = {
    "LocalCorpusReadOnlyAudit",
}

# Automated CI tests exercising unmodified Xbox-platform-targeted external binaries.
# Keep strictly empty until real external Xbox-targeted test inputs run in CI.
EXTERNAL_CI_TESTS: Set[str] = set()


def read_lf(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace").replace("\r\n", "\n")


def parse_def_exports(def_path: Path) -> Tuple[int, int]:
    """Return (total_exports, explicit_ordinal_exports)."""
    total = 0
    ordinals = 0
    in_exports = False
    for raw in read_lf(def_path).splitlines():
        line = raw.split(";", 1)[0].strip()
        if not line:
            continue
        upper = line.upper()
        if upper == "EXPORTS":
            in_exports = True
            continue
        if upper.startswith(("LIBRARY", "NAME", "DESCRIPTION", "VERSION")):
            continue
        if in_exports:
            total += 1
            if re.search(r"@\d+\b", line):
                ordinals += 1
    return total, ordinals


def parse_cmake_shims(cmake_text: str) -> List[Dict[str, object]]:
    """Discover shim libraries, output DLL names, staged aliases, and .def export counts."""
    output_names: Dict[str, str] = {}
    for m in re.finditer(
        r'set_target_properties\(\s*(\w+)\s+PROPERTIES\s+OUTPUT_NAME\s+"([^"]+)"\s*\)',
        cmake_text,
    ):
        output_names[m.group(1)] = m.group(2)

    aliases: Dict[str, List[str]] = {}
    for m in re.finditer(
        r'add_custom_command\(TARGET\s+(\w+)\s+POST_BUILD.*?\$\{SC_BIN_DIR\}/shims/([^"\s]+)"',
        cmake_text,
        flags=re.S,
    ):
        aliases.setdefault(m.group(1), []).append(m.group(2))

    shims: List[Dict[str, object]] = []
    for m in re.finditer(r"add_library\(\s*(\w+)\s+SHARED\s+([^\)]+)\)", cmake_text, flags=re.S):
        target, body = m.group(1), m.group(2)
        dir_match = re.search(r"src/shims/([^/\s]+)/", body)
        if not dir_match:
            continue
        shim_dir_name = dir_match.group(1)
        dll_name = f"{output_names.get(target, target)}.dll"
        def_files = sorted((SHIMS_DIR / shim_dir_name).glob("*.def"))
        total_exports = 0
        ordinal_exports = 0
        for df in def_files:
            t, o = parse_def_exports(df)
            total_exports += t
            ordinal_exports += o
        shims.append(
            {
                "target": target,
                "dir": f"src/shims/{shim_dir_name}",
                "dll": dll_name,
                "aliases": aliases.get(target, []),
                "exports": total_exports,
                "ordinals": ordinal_exports,
            }
        )
    return shims


def parse_gpu_test_set(cmake_text: str) -> Set[str]:
    m = re.search(r"set\(_sc_gpu_tests\s+([^\)]+)\)", cmake_text, flags=re.S)
    if not m:
        raise RuntimeError("Could not locate _sc_gpu_tests list in CMakeLists.txt")
    return {tok.strip() for tok in m.group(1).split() if tok.strip() and not tok.strip().startswith("#")}


def parse_registered_tests(tests_cpp: str, gpu_tests: Set[str]) -> List[Dict[str, str]]:
    tests: List[Dict[str, str]] = []
    for line in tests_cpp.splitlines():
        m = re.match(r'^\s*\{"([A-Za-z0-9_]+)"\s*,\s*([A-Za-z0-9_]+)\}', line)
        if not m:
            continue
        name = m.group(1)
        label = "gpu" if name in gpu_tests else "cpu"
        if name in EXTERNAL_CI_TESTS:
            tier = "Tier 2: External-Input (CI)"
        elif name in OPT_IN_EXTERNAL_TESTS:
            tier = "Tier 1: Self-Authored (opt-in local external audit)"
        else:
            tier = "Tier 1: Self-Authored (Synthetic)"
        tests.append({"name": name, "label": label, "tier": tier})
    if not tests:
        raise RuntimeError("No tests discovered in src/tests/scarlett_tests/main.cpp")
    return tests


def parse_spec_sources(spec_text: str) -> Dict[str, int]:
    counts = {"sourced": 0, "assumed": 0, "other": 0}
    for line in spec_text.splitlines():
        if not line.startswith("|") or line.startswith("| ---") or line.startswith("| :---"):
            continue
        cols = [c.strip() for c in line.strip("|").split("|")]
        if len(cols) < 3:
            continue
        if cols[0] in ("Field / Rule", "Subsystem"):
            continue
        status_col = cols[2]
        if "ASSUMED" in status_col:
            counts["assumed"] += 1
        elif "Sourced" in status_col:
            counts["sourced"] += 1
        else:
            counts["other"] += 1
    return counts


def render_status_markdown(
    shims: List[Dict[str, object]],
    tests: List[Dict[str, str]],
    spec_counts: Dict[str, int],
) -> str:
    total_tests = len(tests)
    cpu_tests = sum(1 for t in tests if t["label"] == "cpu")
    gpu_tests = sum(1 for t in tests if t["label"] == "gpu")
    tier2_tests = sum(1 for t in tests if t["tier"].startswith("Tier 2"))
    opt_in_tests = sum(1 for t in tests if "opt-in local external" in t["tier"])
    tier1_pure = total_tests - tier2_tests - opt_in_tests
    total_exports = sum(int(s["exports"]) for s in shims)

    lines: List[str] = [
        "# Scarlett Layer — Factual Status Matrix",
        "",
        "> Auto-generated by `tools/generate_status.py` from `CMakeLists.txt`, `src/shims/*/*.def`,",
        "> `src/tests/scarlett_tests/main.cpp`, and `docs/SPEC_SOURCES.md`.",
        "> Do not hand-edit this file; re-run `py tools/generate_status.py` and verify with `--check`.",
        "",
        "## Headline Validation Summary",
        "",
        "| Metric | Value | Scope / Caveat |",
        "| :--- | :---: | :--- |",
        f"| **Registered CTests (Total)** | **{total_tests}** (`{cpu_tests} cpu`, `{gpu_tests} gpu`) | Discovered from `src/tests/scarlett_tests/main.cpp` and `CMakeLists.txt` |",
        f"| **Tier 1: Self-Authored Synthetic Tests** | **{tier1_pure}** (+ {opt_in_tests} opt-in local audit) | Verify internal shim behavior against project-authored assertions; prove self-consistency, not retail compatibility |",
        f"| **Tier 2: Automated External Xbox-Targeted Tests (CI)** | **{tier2_tests}** | Unmodified `SimpleTriangleDesktop.exe` (PC Desktop GDK target) has been booted locally; no Xbox-platform-targeted binary runs in CI yet |",
        f"| **Shim DLL Targets** | **{len(shims)}** ({total_exports} `.def` exports) | Export counts reflect declared `.def` symbols, not verified retail ABI parity |",
        f"| **Spec Contracts (`docs/SPEC_SOURCES.md`)** | **{spec_counts['sourced']} sourced / {spec_counts['assumed']} `ASSUMED`** | See `docs/SPEC_SOURCES.md` for project-defined formats (`SCXS`, `SCLZ`, synthetic XVD offsets, modeled COM vtables) |",
        "",
        "## Shim DLL Export Inventory (`.def`)",
        "",
        "| Shim DLL | Source Directory | `.def` Exports | Explicit Ordinals | Staged Aliases |",
        "| :--- | :--- | :---: | :---: | :--- |",
    ]

    for s in shims:
        alias_list = s["aliases"]
        alias_str = ", ".join(f"`{a}`" for a in alias_list) if alias_list else "—"
        lines.append(
            f"| `{s['dll']}` | `{s['dir']}` | {s['exports']} | {s['ordinals']} | {alias_str} |"
        )

    lines += [
        "",
        "## CTest Inventory by Tier & Execution Label",
        "",
        "| # | Test Name | CTest Label | Validation Tier |",
        "| :---: | :--- | :---: | :--- |",
    ]

    for idx, t in enumerate(tests, 1):
        lines.append(f"| {idx:02d} | `{t['name']}` | `{t['label']}` | {t['tier']} |")

    lines.append("")
    return "\n".join(lines)


def check_doc_sync(expected_status: str, total: int, cpu: int, gpu: int) -> List[str]:
    errors: List[str] = []

    if not STATUS_OUT_PATH.exists():
        errors.append("docs/STATUS.md does not exist. Run: py tools/generate_status.py")
    elif read_lf(STATUS_OUT_PATH) != expected_status:
        errors.append("docs/STATUS.md is out of date. Run: py tools/generate_status.py")

    readme = read_lf(README_PATH)
    expected_badge = f"checks-{total}%2F{total}"
    expected_split = f"`{cpu} cpu`, `{gpu} gpu`"
    if expected_badge not in readme:
        errors.append(f"README.md missing matching test badge `{expected_badge}`")
    if f"**{total} individual CTest targets**" not in readme:
        errors.append(f"README.md missing matching count `**{total} individual CTest targets**`")
    if expected_split not in readme:
        errors.append(f"README.md missing matching cpu/gpu split `{expected_split}`")

    roadmap = read_lf(ROADMAP_PATH)
    if f"**{total}/{total} individual CTests passing**" not in roadmap:
        errors.append(f"docs/ROADMAP.md missing matching baseline `**{total}/{total} individual CTests passing**`")
    if expected_split not in roadmap:
        errors.append(f"docs/ROADMAP.md missing matching cpu/gpu split `{expected_split}`")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate or verify docs/STATUS.md and doc counts.")
    parser.add_argument(
        "--check",
        action="store_true",
        help="Exit non-zero if docs/STATUS.md or README/ROADMAP test counts are out of sync.",
    )
    args = parser.parse_args()

    cmake_text = read_lf(CMAKE_PATH)
    tests_cpp = read_lf(TESTS_MAIN_PATH)
    spec_text = read_lf(SPEC_SOURCES_PATH)

    shims = parse_cmake_shims(cmake_text)
    gpu_tests = parse_gpu_test_set(cmake_text)
    tests = parse_registered_tests(tests_cpp, gpu_tests)
    spec_counts = parse_spec_sources(spec_text)

    total = len(tests)
    cpu = sum(1 for t in tests if t["label"] == "cpu")
    gpu = sum(1 for t in tests if t["label"] == "gpu")

    rendered = render_status_markdown(shims, tests, spec_counts)

    if args.check:
        errors = check_doc_sync(rendered, total, cpu, gpu)
        if errors:
            for err in errors:
                print(f"[STATUS CHECK FAIL] {err}", file=sys.stderr)
            return 1
        print(f"[STATUS CHECK OK] docs/STATUS.md, README.md, and docs/ROADMAP.md match ({total} tests: {cpu} cpu / {gpu} gpu, {len(shims)} shims)")
        return 0

    STATUS_OUT_PATH.write_text(rendered, encoding="utf-8", newline="\n")
    print(f"Wrote {STATUS_OUT_PATH.relative_to(ROOT)} ({total} tests: {cpu} cpu / {gpu} gpu, {len(shims)} shims)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
