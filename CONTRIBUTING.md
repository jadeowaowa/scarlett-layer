# Contributing to scarlett-layer

Contributions are welcome, especially external validation (public GDK samples, second-GPU testing), parser hardening, and loader/shim audits.

## Strict Scope Rules

1. **No DRM circumvention or encrypted content:** Encrypted XVD/XVC containers must remain rejected. Do not submit decryption routines, console keys, firmware dumps, retail binaries, or proprietary SDK headers/code.
2. **Evidence over adjectives:** Document what is actually tested and observed. Label unverified or project-defined layouts/interfaces as `ASSUMED` in code comments and `docs/SPEC_SOURCES.md`.
3. **Never weaken tests to make them pass:** If an existing test assertion is genuinely wrong (e.g., an inverted flag), explain why in the commit message and add regression coverage for the corrected behavior.
4. **Independent implementation:** Derive behavior from public documentation, public samples, and observed runtime behavior only.

## Local Build & Verification

Requirements: Windows 10/11 x64, Visual Studio 2022+ (MSVC C++23), CMake 3.25+, Ninja, Python 3.

```powershell
# Build and run all 67 individual CTest targets (requires D3D12 GPU for 'gpu' label)
.\tools\verify.bat --verbose

# Run CPU-only tests (matches GitHub Actions CI)
ctest --test-dir build -L cpu --output-on-failure
```

## Coding & Repository Conventions

- Save source, header, and Markdown files as **UTF-8 without BOM** with LF line endings (`.bat` and `.ps1` use CRLF — enforced by `.gitattributes`).
- Builds must compile cleanly under MSVC `/W4 /permissive-` with 0 warnings.
- Update `docs/PROGRESS.md` and run `py tools\build_wiki.py` when adding shims, exports, or tests.
