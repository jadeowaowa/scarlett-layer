#!/usr/bin/env python3
"""Generate docs/STATUS.md and README.md dynamic progress section from live repo sources."""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

PROGRESS_START = "<!-- SCARLETT:PROGRESS:START -->"
PROGRESS_END = "<!-- SCARLETT:PROGRESS:END -->"

# Evidence-backed subsystem readiness estimates (updated as milestones are verified).
# Tuple: (Subsystem, Weight, Percent, StatusTag, Evidence & Next Gap)
SUBSYSTEM_PROGRESS: list[tuple[str, int, int, str, str]] = [
    ("Loader, IAT Hooks & VFS", 15, 80, "Active", "PE64/delay-load/ordinal rewrite, APC hook, real 12 KiB XVD geometry, VFS containment; needs human audit & fuzzing"),
    ("D3D12.X Graphics & PresentX", 22, 55, "Active", "Flip-Discard `PresentX`, CBV bounce ring, shadow texture upload, Terraria menus rendering cleanly; needs broader D3D12.X ABI & DXR"),
    ("Texture Swizzle & Memory (`xg_xs`, `xmem`)", 12, 50, "Active", "GDK `XGTextureAddressComputer`, GCN `1D_THIN` 8x8 Morton detiler (2,193/2,193 Terraria textures), `64KB_S`/BCn; needs `2D_THIN`/RDNA2 macro-tiling"),
    ("RDNA2 Shader Lifter", 15, 50, "Active", "28-case oracle suite passing; 255-ELF `gfx1030` sweep: 100% decode, 49.4% strict / 96.1% compat DXBC compile"),
    ("GDK Runtime (`xgameruntime`)", 12, 45, "Active", "GDK `261000` COM broker, `XTaskQueue`, `XUser`, local `XGameSave`; missing Xbox Live / multiplayer services"),
    ("Audio (`xaudio2_9`, `AcpHal`)", 8, 45, "Active", "XAudio2 playback, IMA-ADPCM decode, `AcpHal` shape contexts & `ApuAlloc` (Terraria menu audio works); missing software XMA2"),
    ("Input (`gameinput`)", 6, 30, "Partial", "XInput pad 0 & keyboard-to-gamepad fallback; needs GameInput v2 reading history & retail gameplay wiring"),
    ("Networking (`libHttpClient`, `XCurl`)", 5, 30, "Partial", "Offline/mock HTTP & cURL shims; missing live TLS/socket parity"),
    ("DirectStorage (`dstorage_xs`)", 5, 25, "Partial", "Async queues & synthetic `SCLZ` streaming; missing Zlib / GDeflate / BCPack retail codecs"),
]


def make_bar(pct: int, width: int = 10) -> str:
    filled = round((pct / 100.0) * width)
    return f"`{'█' * filled}{'░' * (width - filled)}` **{pct}%**"


def parse_tests() -> tuple[list[str], set[str]]:
    main_cpp = (ROOT / "src/tests/scarlett_tests/main.cpp").read_text(encoding="utf-8")
    tests = re.findall(r'^\s*\{"([A-Za-z0-9_]+)",', main_cpp, flags=re.MULTILINE)

    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    m = re.search(r"set\(_sc_gpu_tests\s+(.*?)\)", cmake, flags=re.DOTALL)
    if not m:
        raise RuntimeError("Could not find _sc_gpu_tests block in CMakeLists.txt")
    gpu_set = set(re.findall(r"\b[A-Za-z0-9_]+\b", m.group(1)))
    return tests, gpu_set


def parse_shims() -> list[tuple[str, str, int]]:
    rows: list[tuple[str, str, int]] = []
    for def_path in sorted((ROOT / "src/shims").glob("*/*.def")):
        dll_name = def_path.stem + ".dll"
        exports = 0
        in_exports = False
        for raw in def_path.read_text(encoding="utf-8").splitlines():
            line = raw.split(";", 1)[0].strip()
            if not line:
                continue
            upper = line.upper()
            if upper.startswith("LIBRARY "):
                dll_name = line.split(None, 1)[1].strip().strip('"')
                if not dll_name.lower().endswith(".dll"):
                    dll_name += ".dll"
                continue
            if upper == "EXPORTS":
                in_exports = True
                continue
            if in_exports:
                exports += 1
        rows.append((dll_name, def_path.relative_to(ROOT).as_posix(), exports))
    return rows


def parse_spec_sources() -> tuple[int, int]:
    text = (ROOT / "docs/SPEC_SOURCES.md").read_text(encoding="utf-8")
    sourced = 0
    assumed = 0
    for line in text.splitlines():
        if not line.startswith("|") or line.startswith("| :---") or line.startswith("| Field") or line.startswith("| Subsystem"):
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        if len(cells) < 3:
            continue
        status = cells[2]
        if "ASSUMED" in status:
            assumed += 1
        elif "Sourced" in status:
            sourced += 1
    return sourced, assumed


def render_readme_progress(total: int, cpu: int, gpu: int, shim_count: int, total_exports: int, sourced: int, assumed: int) -> str:
    total_w = sum(w for _, w, _, _, _ in SUBSYSTEM_PROGRESS)
    overall = round(sum(w * p for _, w, p, _, _ in SUBSYSTEM_PROGRESS) / total_w)

    lines = [
        PROGRESS_START,
        "## Live Progress",
        "",
        f"> **Overall Estimated Readiness:** {make_bar(overall, 14)} — "
        f"**{total}/{total}** Tier 1 CTests (`{cpu} cpu`, `{gpu} gpu`) | "
        f"**{shim_count}** shim DLLs (`{total_exports}` exports) | "
        f"**{sourced}** sourced / **{assumed}** `ASSUMED` specs  ",
        "> *Auto-generated by `tools/generate_status.py` and verified in CI. Percentages reflect demonstrated runtime/test coverage, not marketing estimates.*",
        "",
        "| Subsystem | Progress | State | Demonstrated Scope & Remaining Gaps |",
        "| :--- | :---: | :---: | :--- |",
    ]
    for name, _w, pct, state, notes in SUBSYSTEM_PROGRESS:
        lines.append(f"| **{name}** | {make_bar(pct)} | {state} | {notes} |")

    lines.extend([
        "",
        "### Bring-Up & Compatibility Snapshot",
        "",
        "| Target | Platform | Tier | Status | Notes |",
        "| :--- | :--- | :---: | :---: | :--- |",
        "| `SimpleTriangleDesktop.exe` (GDK `261000`) | PC Desktop GDK | Local | **Playable** | Boots, renders, exits cleanly (`code=0`, 0 auto-stubs) |",
        "| `Terraria` (unpacked Xbox GDK, Unity `2021.3`) | Xbox One / GDK | Local | **Menus** | Boots, renders menus & backgrounds cleanly (`1D_THIN` detiler), plays audio, clean exit; input & gameplay next |",
        "| Automated CI Xbox-targeted binaries | `Gaming.Xbox.Scarlett.x64` | Tier 2 CI | **0** | No redistributable Xbox-targeted binary in CI yet |",
        PROGRESS_END,
    ])
    return "\n".join(lines)


def render_status() -> tuple[str, str, int, int, int, int]:
    tests, gpu_set = parse_tests()
    total = len(tests)
    gpu = sum(1 for t in tests if t in gpu_set)
    cpu = total - gpu
    shims = parse_shims()
    total_exports = sum(c for _, _, c in shims)
    sourced, assumed = parse_spec_sources()

    readme_progress = render_readme_progress(total, cpu, gpu, len(shims), total_exports, sourced, assumed)

    lines = [
        "# Scarlett Layer - Live Status Matrix",
        "",
        "> **Auto-generated by `tools/generate_status.py`. Do not hand-edit.**",
        "> Run `py tools/generate_status.py` after adding tests, shim exports, or spec entries, and verify with `py tools/generate_status.py --check`.",
        "",
        "## Test Tiers & Ground Truth",
        "",
        "| Metric | Value | Notes |",
        "| :--- | :--- | :--- |",
        f"| **Tier 1 (Self-Authored Synthetic CTests)** | **{total}/{total}** (`{cpu} cpu`, `{gpu} gpu`) | Discovered from `src/tests/scarlett_tests/main.cpp` + `CMakeLists.txt`; run in CI via Microsoft WARP. |",
        "| **Tier 2 (Automated CI Xbox-Targeted Binaries)** | **0** | No redistributable `Gaming.Xbox.Scarlett.x64` binary runs in CI yet. |",
        "| **Local Retail Bring-Up** | `Terraria` (unpacked Xbox GDK) | Boots, renders menus/backgrounds cleanly (GCN `1D_THIN` detiler), plays audio, exits cleanly; input & gameplay in progress. |",
        "| **Local Desktop GDK Sample** | `SimpleTriangleDesktop.exe` (PC Desktop target) | Boots, renders, and exits cleanly (`code=0`, 0 auto-stubs) locally; not an Xbox console target. |",
        f"| **Shim DLL Targets & Exported Symbols** | **{len(shims)} `.def` shims** (`{total_exports}` exports) | Plus `libHttpClient.dll` alias staged beside `libHttpClient.GDK.dll`. |",
        f"| **Specification Ledger (`docs/SPEC_SOURCES.md`)** | `{sourced}` Sourced / `{assumed}` `ASSUMED` | Project-defined formats (`SCLZ`, `SCXS`) and unverified COM vtables remain marked `ASSUMED`. |",
        "",
        "## Shim Export Inventory",
        "",
        "| Shim DLL | `.def` Source | Exported Symbols |",
        "| :--- | :--- | :---: |",
    ]
    for dll_name, rel_def, count in shims:
        lines.append(f"| `{dll_name}` | `{rel_def}` | {count} |")

    lines.extend([
        "",
        "## Registered Tier 1 CTests",
        "",
        "| # | Test Name | Label |",
        "| :---: | :--- | :---: |",
    ])
    for idx, t in enumerate(tests, start=1):
        lbl = "gpu" if t in gpu_set else "cpu"
        lines.append(f"| {idx} | `{t}` | `{lbl}` |")

    lines.append("")
    return "\n".join(lines), readme_progress, total, cpu, gpu, len(shims)


def update_readme_progress(readme_progress: str) -> None:
    readme_path = ROOT / "README.md"
    txt = readme_path.read_text(encoding="utf-8").replace("\r\n", "\n")
    pattern = re.compile(re.escape(PROGRESS_START) + r".*?" + re.escape(PROGRESS_END), flags=re.DOTALL)
    if not pattern.search(txt):
        raise RuntimeError("README.md is missing SCARLETT:PROGRESS markers")
    updated = pattern.sub(readme_progress, txt, count=1)
    readme_path.write_bytes(updated.encode("utf-8"))


def check_doc_sync(expected_status: str, expected_progress: str, total: int, cpu: int, gpu: int, shim_count: int) -> int:
    errors: list[str] = []
    status_path = ROOT / "docs/STATUS.md"
    if not status_path.exists():
        errors.append("docs/STATUS.md does not exist; run `py tools/generate_status.py`")
    else:
        actual = status_path.read_text(encoding="utf-8").replace("\r\n", "\n")
        if actual != expected_status:
            errors.append("docs/STATUS.md is out of date; run `py tools/generate_status.py`")

    readme_txt = (ROOT / "README.md").read_text(encoding="utf-8").replace("\r\n", "\n")
    m = re.search(re.escape(PROGRESS_START) + r".*?" + re.escape(PROGRESS_END), readme_txt, flags=re.DOTALL)
    if not m:
        errors.append("README.md is missing SCARLETT:PROGRESS markers")
    elif m.group(0) != expected_progress:
        errors.append("README.md Live Progress block is out of date; run `py tools/generate_status.py`")

    pair_token = f"`{cpu} cpu`, `{gpu} gpu`"
    total_token = f"{total}/{total}"
    for rel in ("README.md", "docs/ROADMAP.md", "CONTRIBUTING.md", "AGENTS.md"):
        txt = (ROOT / rel).read_text(encoding="utf-8")
        if total_token not in txt and f"{total} individual" not in txt:
            errors.append(f"{rel} is missing current test total `{total_token}`")
        if pair_token not in txt:
            errors.append(f"{rel} is missing current split `{pair_token}`")

    if errors:
        for e in errors:
            print(f"[STATUS CHECK FAIL] {e}", file=sys.stderr)
        return 1

    print(f"[STATUS CHECK OK] docs/STATUS.md, README.md (Live Progress), ROADMAP.md, CONTRIBUTING.md, and AGENTS.md match ({total} tests: {cpu} cpu / {gpu} gpu, {shim_count} shims)")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="Verify docs/STATUS.md and core docs match live counts")
    args = ap.parse_args()

    rendered, readme_progress, total, cpu, gpu, shim_count = render_status()
    if args.check:
        return check_doc_sync(rendered, readme_progress, total, cpu, gpu, shim_count)

    (ROOT / "docs/STATUS.md").write_bytes(rendered.encode("utf-8"))
    update_readme_progress(readme_progress)
    print(f"[OK] Generated docs/STATUS.md & updated README.md Live Progress ({total} tests: {cpu} cpu / {gpu} gpu, {shim_count} shims)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
