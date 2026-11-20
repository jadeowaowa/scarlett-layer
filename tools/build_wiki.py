#!/usr/bin/env python3
"""
Zero-dependency Scarlett Layer wiki & AI context snapshot generator.
Scans CMakeLists.txt, .def files, public headers, CTest suite, PROGRESS.md,
and docs/wiki/INVARIANTS.md to emit browsable Markdown pages under docs/wiki/
plus a single-file AI_CONTEXT_SNAPSHOT.md for lossless session handoff.
"""

from __future__ import annotations

import datetime
import re
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple


ROOT = Path(__file__).resolve().parent.parent
WIKI_DIR = ROOT / "docs" / "wiki"
INVARIANTS_PATH = WIKI_DIR / "INVARIANTS.md"


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content.replace("\r\n", "\n"), encoding="utf-8", newline="\n")


def run_git(args: List[str]) -> str:
    try:
        res = subprocess.run(
            ["git", *args],
            cwd=ROOT,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=5,
        )
        return res.stdout.strip() if res.returncode == 0 else ""
    except Exception:
        return ""


def extract_braced_block(text: str, open_idx: int) -> Optional[Tuple[str, int]]:
    """Return (inner_text, close_idx) matching the '{' at open_idx."""
    if open_idx < 0 or open_idx >= len(text) or text[open_idx] != "{":
        return None
    depth = 0
    i = open_idx
    n = len(text)
    in_line_comment = False
    in_block_comment = False
    in_str = False
    in_char = False

    while i < n:
        ch = text[i]
        nxt = text[i + 1] if i + 1 < n else ""

        if in_line_comment:
            if ch == "\n":
                in_line_comment = False
        elif in_block_comment:
            if ch == "*" and nxt == "/":
                in_block_comment = False
                i += 1
        elif in_str:
            if ch == "\\":
                i += 1
            elif ch == '"':
                in_str = False
        elif in_char:
            if ch == "\\":
                i += 1
            elif ch == "'":
                in_char = False
        else:
            if ch == "/" and nxt == "/":
                in_line_comment = True
                i += 1
            elif ch == "/" and nxt == "*":
                in_block_comment = True
                i += 1
            elif ch == '"':
                in_str = True
            elif ch == "'":
                in_char = True
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return text[open_idx + 1 : i], i
        i += 1
    return None


def strip_cpp_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def collapse_ws(s: str) -> str:
    return re.sub(r"\s+", " ", s).strip()


def parse_def_file(path: Path) -> List[str]:
    exports: List[str] = []
    in_exports = False
    for raw_line in read_text(path).splitlines():
        line = raw_line.split(";", 1)[0].strip()
        if not line:
            continue
        if line.upper() == "EXPORTS":
            in_exports = True
            continue
        if line.upper().startswith(("LIBRARY", "NAME", "DESCRIPTION", "VERSION")):
            continue
        if in_exports:
            exports.append(collapse_ws(line))
    return exports


def parse_com_interfaces(header_text: str) -> List[Tuple[str, str, List[str]]]:
    """Return [(iface_name, base_name, [method_sigs])] in exact vtable declaration order."""
    interfaces: List[Tuple[str, str, List[str]]] = []
    pattern = re.compile(r"\b(?:struct|class)\s+(\w+)\s*:\s*public\s+(\w+)\s*\{")
    for m in pattern.finditer(header_text):
        name, base = m.group(1), m.group(2)
        if base not in ("IUnknown", "ID3D12DeviceX", "IXAudio2Voice", "IXAudio2") and not name.startswith("I"):
            continue
        block = extract_braced_block(header_text, m.end() - 1)
        if not block:
            continue
        body = strip_cpp_comments(block[0])
        methods: List[str] = []
        for vm in re.finditer(r"\bvirtual\s+([^;]+);", body):
            sig = collapse_ws(vm.group(1))
            sig = re.sub(r"\s*=\s*0$", "", sig)
            sig = sig.replace("STDMETHODCALLTYPE ", "")
            methods.append(sig)
        if methods:
            interfaces.append((name, base, methods))
    return interfaces


def parse_extern_c_exports(header_text: str) -> List[str]:
    sigs: List[str] = []
    for m in re.finditer(r'extern\s+"C"\s*\{', header_text):
        block = extract_braced_block(header_text, m.end() - 1)
        if not block:
            continue
        body = strip_cpp_comments(block[0])
        for stmt in body.split(";"):
            s = collapse_ws(stmt)
            if s and "(" in s and ")" in s:
                sigs.append(s)
    return sigs


def parse_enums(header_text: str) -> List[Tuple[str, List[str]]]:
    enums: List[Tuple[str, List[str]]] = []
    pattern = re.compile(r"\benum(?:\s+class)?\s+(\w+)(?:\s*:\s*[\w:\s]+)?\s*\{")
    for m in pattern.finditer(header_text):
        name = m.group(1)
        block = extract_braced_block(header_text, m.end() - 1)
        if not block:
            continue
        body = strip_cpp_comments(block[0])
        items = [collapse_ws(part) for part in body.split(",") if collapse_ws(part)]
        if items:
            enums.append((name, items))
    return enums


def parse_structs(header_text: str) -> List[str]:
    names: List[str] = []
    for m in re.finditer(r"\bstruct\s+(\w+)\s*\{", header_text):
        names.append(m.group(1))
    return names


def parse_cmake_targets(cmake_path: Path) -> List[Tuple[str, str]]:
    if not cmake_path.exists():
        return []
    text = strip_cpp_comments(read_text(cmake_path))
    targets: List[Tuple[str, str]] = []
    for m in re.finditer(r"\badd_(executable|library)\s*\(\s*([^\s\)]+)", text):
        targets.append((m.group(1), m.group(2)))
    return targets


def parse_ctest_cases(test_cpp: Path) -> List[Tuple[str, str, str]]:
    """Return [(test_name, func_name, literal_call_line)]."""
    if not test_cpp.exists():
        return []
    cases: List[Tuple[str, str, str]] = []
    for raw_line in read_text(test_cpp).splitlines():
        m = re.search(r'RunTest\(\s*"([^"]+)"\s*,\s*(\w+)\s*\)\s*;', raw_line)
        if m:
            cases.append((m.group(1), m.group(2), raw_line.strip()))
    return cases


def parse_sprint_log(progress_path: Path) -> List[str]:
    if not progress_path.exists():
        return []
    lines = read_text(progress_path).splitlines()
    out: List[str] = []
    in_table = False
    for line in lines:
        if line.startswith("| Sprint |"):
            in_table = True
        if in_table:
            if line.startswith("|"):
                out.append(line)
            elif out:
                break
    return out


def collect_source_tree() -> List[Tuple[str, int]]:
    files: List[Tuple[str, int]] = []
    scan_dirs = [ROOT / "src", ROOT / "tools"]
    for base in scan_dirs:
        if not base.exists():
            continue
        for p in sorted(base.rglob("*")):
            if not p.is_file():
                continue
            if p.suffix.lower() not in (".cpp", ".hpp", ".h", ".c", ".def", ".py", ".hlsl", ".txt"):
                continue
            rel = p.relative_to(ROOT).as_posix()
            line_count = len(read_text(p).splitlines())
            files.append((rel, line_count))
    cmake_file = ROOT / "CMakeLists.txt"
    if cmake_file.exists():
        files.insert(0, ("CMakeLists.txt", len(read_text(cmake_file).splitlines())))
    return files


def scan_shim(shim_name: str) -> Dict[str, object]:
    shim_dir = ROOT / "src" / "shims" / shim_name
    info: Dict[str, object] = {
        "name": shim_name,
        "files": [],
        "exports": [],
        "interfaces": [],
        "extern_c": [],
        "enums": [],
        "structs": [],
    }
    if not shim_dir.exists():
        return info

    for p in sorted(shim_dir.iterdir()):
        if not p.is_file():
            continue
        rel = p.relative_to(ROOT).as_posix()
        info["files"].append((rel, len(read_text(p).splitlines())))
        if p.suffix.lower() == ".def":
            info["exports"].extend(parse_def_file(p))
        elif p.suffix.lower() in (".hpp", ".h"):
            txt = read_text(p)
            info["interfaces"].extend(parse_com_interfaces(txt))
            info["extern_c"].extend(parse_extern_c_exports(txt))
            info["enums"].extend(parse_enums(txt))
            info["structs"].extend(parse_structs(txt))
    return info


def short_name(sig: str) -> str:
    m = re.search(r"(\w+)\s*\(", sig)
    return m.group(1) if m else sig


def render_shim_slim(shim: Dict[str, object]) -> str:
    parts: List[str] = [f"- **`{shim['name']}.dll`**"]
    if shim["exports"]:
        parts.append("  - Exports: " + ", ".join(f"`{e}`" for e in shim["exports"]))
    for iface_name, base_name, methods in shim["interfaces"]:
        slot_start = 3 if base_name == "IUnknown" else 0
        slots = ", ".join(f"[{slot_start + i:02d}] {short_name(s)}" for i, s in enumerate(methods))
        parts.append(f"  - `{iface_name}:{base_name}`: {slots}")
    return "\n".join(parts)


def render_shim_section(shim: Dict[str, object], detailed: bool = True) -> str:
    lines: List[str] = []
    lines.append(f"## `{shim['name']}.dll`")
    file_summary = ", ".join(f"`{rel}` ({cnt} L)" for rel, cnt in shim["files"])
    lines.append(f"**Files:** {file_summary}\n")

    if shim["exports"]:
        exp_str = ", ".join(f"`{e}`" for e in shim["exports"])
        lines.append(f"**DLL Exports (`.def`):** {exp_str}\n")

    if shim["extern_c"]:
        lines.append("**C Entrypoints:**")
        for sig in shim["extern_c"]:
            lines.append(f"- `{sig}`")
        lines.append("")

    for iface_name, base_name, methods in shim["interfaces"]:
        slot_start = 3 if base_name == "IUnknown" else 0
        lines.append(f"**COM Interface `{iface_name} : {base_name}`:**")
        for idx, sig in enumerate(methods):
            slot = slot_start + idx
            lines.append(f"- `[{slot:02d}]` `{sig}`")
        lines.append("")

    if detailed and shim["enums"]:
        lines.append("**Key Enums:**")
        for ename, items in shim["enums"]:
            preview = ", ".join(items[:14])
            if len(items) > 14:
                preview += f", ... (+{len(items) - 14} more)"
            lines.append(f"- `{ename}`: {preview}")
        lines.append("")

    return "\n".join(lines).rstrip() + "\n"


def main() -> int:
    WIKI_DIR.mkdir(parents=True, exist_ok=True)

    commit_short = run_git(["rev-parse", "--short", "HEAD"]) or "uncommitted"
    commit_count = run_git(["rev-list", "--count", "HEAD"]) or "?"
    recent_commits = run_git(["log", "-n", "12", "--oneline"]).splitlines()
    generated_utc = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC")

    source_tree = collect_source_tree()
    total_loc = sum(cnt for _, cnt in source_tree)
    cmake_targets = parse_cmake_targets(ROOT / "CMakeLists.txt")
    sprint_log = parse_sprint_log(ROOT / "docs" / "PROGRESS.md")
    ctest_cases = parse_ctest_cases(ROOT / "src" / "tests" / "scarlett_tests" / "main.cpp")

    shims_root = ROOT / "src" / "shims"
    shim_names = sorted(p.name for p in shims_root.iterdir() if p.is_dir()) if shims_root.exists() else []
    shim_map = {name: scan_shim(name) for name in shim_names}

    # --- 1. Home.md ---
    home_lines = [
        "# Scarlett Layer Engineering Wiki",
        "",
        f"> Auto-generated by `tools/build_wiki.py` at commit `{commit_short}` (#{commit_count}, {generated_utc}).  ",
        f"> **Codebase:** {len(source_tree)} source files ({total_loc:,} LOC) | **{len(shim_names)} shims** | **{len(ctest_cases)} automated CTest checks**.",
        "",
        "## Navigation",
        "- **[Engineering Invariants & Sharp Edges](INVARIANTS.md)** *(hand-maintained)*",
        "- **[AI Context Snapshot (Single-File Upload)](AI_CONTEXT_SNAPSHOT.md)** *(dense token-optimized handoff)*",
        "- **[01 â€” Loader, Hooking & Common Utilities](01_Loader_and_VFS.md)**",
        "- **[02 â€” Graphics, Tiling & Binary RDNA2 Lifter](02_Graphics_and_RDNA2.md)**",
        "- **[03 â€” Runtime, Kernel, Memory, Input & PIX](03_Runtime_and_Memory.md)**",
        "- **[04 â€” Velocity DirectStorage & XAudio2](04_Storage_and_Audio.md)**",
        "- **[05 â€” Automated Test Suite & Synthetic Title](05_Test_Suite_Index.md)**",
        "- **[Progress Matrix](../PROGRESS.md)** | **[Technical Architecture Paper](../ARCHITECTURE_PAPER.md)**",
        "",
        "## CMake Targets",
    ]
    for kind, tname in cmake_targets:
        home_lines.append(f"- `{tname}` ({kind})")
    home_lines += ["", "## Recent Commits"]
    for c in recent_commits[:10]:
        home_lines.append(f"- `{c}`")
    write_text(WIKI_DIR / "Home.md", "\n".join(home_lines) + "\n")

    # --- 2. 01_Loader_and_VFS.md ---
    p1 = [
        "# 01 â€” Loader, Runtime IAT Hooking & Common Utilities",
        "",
        "## Source Files",
    ]
    for rel, cnt in source_tree:
        if rel.startswith(("src/loader/", "src/common/", "tools/", "CMakeLists.txt")):
            p1.append(f"- `{rel}` ({cnt} lines)")
    p1.append("")
    for hdr in sorted((ROOT / "src" / "common").glob("*.hpp")):
        txt = read_text(hdr)
        structs = parse_structs(txt)
        enums = parse_enums(txt)
        p1.append(f"## `{hdr.relative_to(ROOT).as_posix()}`")
        if structs:
            p1.append("- **Structs:** " + ", ".join(f"`{s}`" for s in structs))
        for ename, items in enums:
            p1.append(f"- **Enum `{ename}`:** " + ", ".join(items))
        p1.append("")
    write_text(WIKI_DIR / "01_Loader_and_VFS.md", "\n".join(p1).rstrip() + "\n")

    # --- 3. 02_Graphics_and_RDNA2.md ---
    p2 = ["# 02 â€” Graphics (`d3d12_xs`), Surface Tiling (`xg_xs`) & RDNA2 Binary Lifter", ""]
    for sname in ("d3d12_xs", "xg_xs"):
        if sname in shim_map:
            p2.append(render_shim_section(shim_map[sname], detailed=True))
    write_text(WIKI_DIR / "02_Graphics_and_RDNA2.md", "\n".join(p2).rstrip() + "\n")

    # --- 4. 03_Runtime_and_Memory.md ---
    p3 = ["# 03 â€” Runtime (`xgameruntime`), Kernel (`kernelx`), Memory (`xmem`), Input & PIX", ""]
    for sname in ("xgameruntime", "kernelx", "xmem", "gameinput", "pixevt"):
        if sname in shim_map:
            p3.append(render_shim_section(shim_map[sname], detailed=True))
    write_text(WIKI_DIR / "03_Runtime_and_Memory.md", "\n".join(p3).rstrip() + "\n")

    # --- 5. 04_Storage_and_Audio.md ---
    p4 = ["# 04 â€” Velocity Storage (`dstorage_xs`) & Audio Proxy (`xaudio2_9`)", ""]
    for sname in ("dstorage_xs", "xaudio2_9"):
        if sname in shim_map:
            p4.append(render_shim_section(shim_map[sname], detailed=True))
    write_text(WIKI_DIR / "04_Storage_and_Audio.md", "\n".join(p4).rstrip() + "\n")

    # --- 6. 05_Test_Suite_Index.md ---
    p5 = [
        "# 05 â€” Automated Test Suite & Synthetic Title Index",
        "",
        f"`scarlett_tests.exe` runs **{len(ctest_cases)} checks** via `loader.exe` under CTest (`ctest --test-dir build`).",
        "",
        "## Exact `RunTest` Registration Order (`src/tests/scarlett_tests/main.cpp`)",
        "```cpp",
    ]
    for idx, (_, _, literal) in enumerate(ctest_cases, 1):
        p5.append(f"// {idx:02d}")
        p5.append(f"    {literal}")
    p5 += ["```", "", "## Test & Title Files"]
    for rel, cnt in source_tree:
        if rel.startswith("src/tests/"):
            p5.append(f"- `{rel}` ({cnt} lines)")
    write_text(WIKI_DIR / "05_Test_Suite_Index.md", "\n".join(p5).rstrip() + "\n")

    # --- 7. AI_CONTEXT_SNAPSHOT.md (Single-File Upload Bundle) ---
    slim = "--slim" in sys.argv
    snap_sprints = (sprint_log[:2] + sprint_log[-5:]) if (slim and len(sprint_log) > 7) else sprint_log
    snap: List[str] = [
        "# Scarlett Layer â€” AI Context Snapshot" + (" (Slim)" if slim else ""),
        "",
        f"**Commit:** `{commit_short}` (#{commit_count}) | **Generated:** {generated_utc}  ",
        f"**Totals:** {len(source_tree)} files ({total_loc:,} LOC) | {len(shim_names)} shims | {len(ctest_cases)} CTest checks",
        "",
        "---",
        "",
        "## 1. Sprint Status (`docs/PROGRESS.md`)",
        *snap_sprints,
        "",
        "---",
        "",
    ]

    if INVARIANTS_PATH.exists():
        snap.append(read_text(INVARIANTS_PATH).strip())
        snap += ["", "---", ""]

    snap.append("## 2. File Inventory")
    if slim:
        snap.append(", ".join(f"`{rel}`" for rel, _ in source_tree))
    else:
        snap.append("```text")
        for rel, cnt in source_tree:
            snap.append(f"{cnt:5d}  {rel}")
        snap.append("```")

    snap += ["", "## 3. Shim Exports & COM VTable Layouts", ""]
    for sname in shim_names:
        snap.append(render_shim_slim(shim_map[sname]) if slim else render_shim_section(shim_map[sname], detailed=False))

    rdna2_hdr = ROOT / "src" / "shims" / "d3d12_xs" / "rdna2_isa.hpp"
    if rdna2_hdr.exists():
        rdna2_enums = parse_enums(read_text(rdna2_hdr))
        snap += ["", "## 4. RDNA2 ISA Decoder Opcodes (`rdna2_isa.hpp`)"]
        for ename, items in rdna2_enums:
            vals = [it.split("=")[0].strip() for it in items] if slim else items
            snap.append(f"- `{ename}`: " + ", ".join(vals))
        snap.append("")

    anchor_cases = ctest_cases[-3:] if (slim and len(ctest_cases) > 3) else ctest_cases
    snap += [
        f"## 5. Exact CTest Registration Anchors (`src/tests/scarlett_tests/main.cpp`, {len(ctest_cases)} total)",
        "```cpp",
    ]
    if slim and len(ctest_cases) > 3:
        snap.append(f"    // ... ({len(ctest_cases) - 3} earlier tests omitted)")
    for _, _, literal in anchor_cases:
        snap.append(f"    {literal}")
    snap += ["```", "", "## 6. Recent Git History"]
    for c in recent_commits[: (5 if slim else 10)]:
        snap.append(f"- `{c}`")

    snapshot_text = "\n".join(snap).rstrip() + "\n"
    snapshot_path = WIKI_DIR / "AI_CONTEXT_SNAPSHOT.md"
    write_text(snapshot_path, snapshot_text)

    kb = len(snapshot_text.encode("utf-8")) / 1024.0
    approx_tokens = len(snapshot_text) // 4
    mode_tag = " [SLIM]" if slim else ""
    print(f"Built wiki in {WIKI_DIR.relative_to(ROOT)} (7 files)")
    print(f"AI_CONTEXT_SNAPSHOT.md{mode_tag}: {kb:.1f} KB (~{approx_tokens:,} tokens)")
    return 0


if __name__ == "__main__":
    sys.exit(main())