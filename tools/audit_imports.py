#!/usr/bin/env python3
"""Zero-dependency PE32+ (x64) import auditor, shim coverage checker, and C++ stub generator."""

import argparse
import re
import struct
import sys
from pathlib import Path

XBOX_KEYWORDS = (
    "d3d12_xs", "xg_xs", "xgame", "xmem", "dstorage",
    "pixevt", "gameinput", "kernelx", "xaudio2", "xcrypt"
)


def read_cstr(data: bytes, offset: int) -> str:
    if offset < 0 or offset >= len(data):
        return ""
    end = data.find(b"\x00", offset)
    if end == -1:
        end = len(data)
    return data[offset:end].decode("ascii", errors="replace")


def rva_to_offset(rva: int, sections: list[tuple[int, int, int, int]]) -> int:
    for vaddr, vsize, raw_ptr, raw_size in sections:
        span = max(vsize, raw_size)
        if vaddr <= rva < vaddr + span:
            return raw_ptr + (rva - vaddr)
    return -1


def parse_thunk_list(data: bytes, thunk_rva: int, sections: list[tuple[int, int, int, int]]) -> list[str]:
    thunk_off = rva_to_offset(thunk_rva, sections)
    funcs = []
    if thunk_off < 0:
        return funcs
    while thunk_off + 8 <= len(data):
        val = struct.unpack_from("<Q", data, thunk_off)[0]
        thunk_off += 8
        if val == 0:
            break
        if val & (1 << 63):
            funcs.append(f"@{val & 0xFFFF}")
        else:
            fn_off = rva_to_offset(val & 0x7FFFFFFF, sections)
            funcs.append(read_cstr(data, fn_off + 2) if fn_off >= 0 else "<unmapped>")
    return funcs


def parse_pe_imports(path: Path) -> list[tuple[str, str, list[str]]]:
    """Returns list of (dll_name, kind ['static'|'delay'], [func_names])."""
    data = path.read_bytes()
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise ValueError(f"Not a valid PE file: {path}")

    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_off:pe_off + 4] != b"PE\x00\x00":
        raise ValueError(f"Missing PE signature: {path}")

    num_sections = struct.unpack_from("<H", data, pe_off + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe_off + 20)[0]
    opt_off = pe_off + 24
    magic = struct.unpack_from("<H", data, opt_off)[0]
    if magic != 0x20B:
        raise ValueError(f"Unsupported PE magic 0x{magic:04X} (expected x64 PE32+ 0x020B)")

    dd_off = opt_off + 112
    import_rva, import_size = struct.unpack_from("<II", data, dd_off + 1 * 8)
    delay_rva, delay_size = struct.unpack_from("<II", data, dd_off + 13 * 8)

    sec_off = opt_off + opt_size
    sections = []
    for i in range(num_sections):
        off = sec_off + i * 40
        vsize, vaddr, raw_size, raw_ptr = struct.unpack_from("<IIII", data, off + 8)
        sections.append((vaddr, vsize, raw_ptr, raw_size))

    results: list[tuple[str, str, list[str]]] = []

    if import_rva != 0 and import_size != 0:
        desc_off = rva_to_offset(import_rva, sections)
        while desc_off >= 0 and desc_off + 20 <= len(data):
            orig_first_thunk, _ts, _fwd, name_rva, first_thunk = struct.unpack_from("<IIIII", data, desc_off)
            desc_off += 20
            if name_rva == 0 and first_thunk == 0:
                break
            dll_name = read_cstr(data, rva_to_offset(name_rva, sections))
            funcs = parse_thunk_list(data, orig_first_thunk or first_thunk, sections)
            results.append((dll_name, "static", funcs))

    if delay_rva != 0 and delay_size != 0:
        desc_off = rva_to_offset(delay_rva, sections)
        while desc_off >= 0 and desc_off + 32 <= len(data):
            attrs, name_rva, _hmod, _iat, int_rva, _biat, _uiat, _ts = struct.unpack_from("<IIIIIIII", data, desc_off)
            desc_off += 32
            if name_rva == 0:
                break
            if (attrs & 1) == 0:
                continue
            dll_name = read_cstr(data, rva_to_offset(name_rva, sections))
            funcs = parse_thunk_list(data, int_rva, sections)
            results.append((dll_name, "delay", funcs))

    return results


def sanitize_ident(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]", "_", name)


def generate_stub(dll_name: str, funcs: list[str], out_dir: Path) -> None:
    stem = Path(dll_name).stem.lower()
    target_dir = out_dir / stem
    target_dir.mkdir(parents=True, exist_ok=True)

    def_lines = [f"LIBRARY {stem}", "EXPORTS"]
    cpp_stubs = []

    for fn in funcs:
        if fn.startswith("@"):
            ordinal = fn[1:]
            sym = f"{stem}_ordinal_{ordinal}"
            def_lines.append(f"    {sym} @{ordinal} NONAME")
            cpp_stubs.append(
                f'HRESULT WINAPI {sym}() {{\n'
                f'    SC_STUB_ONCE("{stem}", E_NOTIMPL);\n'
                f'}}\n'
            )
        else:
            safe_sym = sanitize_ident(fn)
            if safe_sym == fn:
                def_lines.append(f"    {fn}")
            else:
                def_lines.append(f"    {fn} = {safe_sym}")
            cpp_stubs.append(
                f'HRESULT WINAPI {safe_sym}() {{\n'
                f'    SC_STUB_ONCE("{stem}", E_NOTIMPL);\n'
                f'}}\n'
            )

    def_path = target_dir / f"{stem}.def"
    cpp_path = target_dir / f"{stem}.cpp"

    def_path.write_text("\n".join(def_lines) + "\n", encoding="utf-8")

    cpp_content = (
        '#include "common/stub.hpp"\n\n'
        'BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {\n'
        '    if (reason == DLL_PROCESS_ATTACH) {\n'
        '        DisableThreadLibraryCalls(hModule);\n'
        '        char path[MAX_PATH]{};\n'
        '        GetModuleFileNameA(hModule, path, MAX_PATH);\n'
        f'        sc::log::Write(sc::log::Level::Info, "{stem}", "DllMain", path);\n'
        '    }\n'
        '    return TRUE;\n'
        '}\n\n'
        'extern "C" {\n\n'
        + "\n".join(cpp_stubs)
        + '\n} // extern "C"\n'
    )
    cpp_path.write_text(cpp_content, encoding="utf-8")
    print(f"Generated stub shim ({len(funcs)} exports):")
    print(f"  - {def_path}")
    print(f"  - {cpp_path}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Scarlett Layer PE32+ Import Auditor & Stub Generator")
    parser.add_argument("binary", type=Path, help="Path to x64 .exe or .dll")
    parser.add_argument("--shims-dir", type=Path, default=Path("build/bin/shims"),
                        help="Directory of built shim DLLs for coverage check")
    parser.add_argument("--gen-stub", type=str, default=None,
                        help="Generate C++/.def stub shim for a specific imported DLL name")
    parser.add_argument("--out-dir", type=Path, default=Path("build/generated_stubs"),
                        help="Output root for --gen-stub (default: build/generated_stubs)")
    args = parser.parse_args()

    try:
        imports = parse_pe_imports(args.binary)
    except Exception as exc:
        print(f"Error: {exc}")
        return 1

    built_shims = set()
    if args.shims_dir.exists():
        built_shims = {p.name.lower() for p in args.shims_dir.glob("*.dll")}

    if args.gen_stub:
        target_lower = args.gen_stub.lower()
        if not target_lower.endswith(".dll"):
            target_lower += ".dll"
        matched_funcs: list[str] = []
        matched_name = args.gen_stub
        for dll_name, _kind, funcs in imports:
            if dll_name.lower() == target_lower:
                matched_name = dll_name
                for fn in funcs:
                    if fn not in matched_funcs:
                        matched_funcs.append(fn)
        if not matched_funcs:
            print(f"No imports from '{args.gen_stub}' found in {args.binary}")
            return 1
        generate_stub(matched_name, matched_funcs, args.out_dir)
        return 0

    print(f"=== PE32+ Import Audit: {args.binary} ===\n")
    if not imports:
        print("No import directory found.")
        return 0

    xbox_total = 0
    xbox_shimmed = 0

    for dll_name, kind, funcs in imports:
        lower = dll_name.lower()
        is_xbox = any(k in lower for k in XBOX_KEYWORDS)
        has_shim = lower in built_shims
        tags = [f"[{kind}]"]
        if is_xbox:
            xbox_total += 1
            tags.append("[XBOX/GDK]")
            if has_shim:
                xbox_shimmed += 1
                tags.append("[SHIMMED]")
            else:
                tags.append("[MISSING]")
        elif has_shim:
            tags.append("[SHIMMED]")

        print(f"{dll_name} {' '.join(tags)} ({len(funcs)} imports)")
        if is_xbox:
            for fn in funcs:
                print(f"    - {fn}")

    if xbox_total > 0:
        print(f"\nXbox/GDK Shim Coverage: {xbox_shimmed}/{xbox_total} imported DLLs shimmed")
    return 0


if __name__ == "__main__":
    sys.exit(main())