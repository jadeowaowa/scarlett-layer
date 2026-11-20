#!/usr/bin/env python3
"""Zero-dependency PE32+ (x64) import auditor for Xbox Series X / GDK binaries."""

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


def audit_pe(path: Path) -> None:
    data = path.read_bytes()
    if len(data) < 0x40 or data[:2] != b"MZ":
        print(f"Not a valid PE file: {path}")
        return

    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_off:pe_off + 4] != b"PE\x00\x00":
        print(f"Missing PE signature: {path}")
        return

    num_sections = struct.unpack_from("<H", data, pe_off + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe_off + 20)[0]
    opt_off = pe_off + 24
    magic = struct.unpack_from("<H", data, opt_off)[0]
    if magic != 0x20B:
        print(f"Unsupported PE magic 0x{magic:04X} (expected x64 PE32+ 0x020B)")
        return

    dd_off = opt_off + 112
    import_rva, import_size = struct.unpack_from("<II", data, dd_off + 1 * 8)

    sec_off = opt_off + opt_size
    sections = []
    for i in range(num_sections):
        off = sec_off + i * 40
        vsize, vaddr, raw_size, raw_ptr = struct.unpack_from("<IIII", data, off + 8)
        sections.append((vaddr, vsize, raw_ptr, raw_size))

    print(f"=== PE32+ Import Audit: {path} ===\n")
    if import_rva == 0 or import_size == 0:
        print("No import directory found.")
        return

    desc_off = rva_to_offset(import_rva, sections)
    if desc_off < 0:
        print("Could not map import directory RVA to file offset.")
        return

    while True:
        orig_first_thunk, _ts, _fwd, name_rva, first_thunk = struct.unpack_from("<IIIII", data, desc_off)
        desc_off += 20
        if name_rva == 0 and first_thunk == 0:
            break

        dll_name = read_cstr(data, rva_to_offset(name_rva, sections))
        is_xbox = any(k in dll_name.lower() for k in XBOX_KEYWORDS)
        thunk_rva = orig_first_thunk or first_thunk
        thunk_off = rva_to_offset(thunk_rva, sections)

        funcs = []
        if thunk_off >= 0:
            while True:
                val = struct.unpack_from("<Q", data, thunk_off)[0]
                thunk_off += 8
                if val == 0:
                    break
                if val & (1 << 63):
                    funcs.append(f"Ordinal #{val & 0xFFFF}")
                else:
                    fn_off = rva_to_offset(val & 0x7FFFFFFF, sections)
                    funcs.append(read_cstr(data, fn_off + 2) if fn_off >= 0 else "<unmapped>")

        tag = " [XBOX/GDK]" if is_xbox else ""
        print(f"{dll_name}{tag} ({len(funcs)} imports)")
        if is_xbox:
            for fn in funcs:
                print(f"    - {fn}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python tools/audit_imports.py <path_to_exe_or_dll>")
        sys.exit(1)
    audit_pe(Path(sys.argv[1]))