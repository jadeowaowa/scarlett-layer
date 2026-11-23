# Contributing to scarlett-layer

Contributions are welcome, especially external validation (unmodified public GDK samples, Xbox-platform-targeted homebrew/dev builds, second-GPU testing), RDNA2 shader differential/corpus testing, parser hardening, and independent loader/shim audits.

## Strict Scope Rules

1. **No DRM circumvention or encrypted/leaked content:** Encrypted XVD/XVC containers must remain rejected. Do not submit decryption routines, console keys, firmware dumps, retail/leaked binaries, or proprietary SDK headers/code.
2. **Evidence over adjectives:** Document only what is actually tested and observed. Label unverified or project-defined layouts/interfaces as `ASSUMED` in code comments and `docs/SPEC_SOURCES.md`. Keep Tier 1 (self-authored synthetic tests) distinct from Tier 2 (external Xbox-targeted binaries).
3. **Never weaken tests to make them pass:** If an existing test assertion is genuinely wrong (e.g., an inverted flag), explain why in the commit message and add regression coverage for the corrected behavior.
4. **Independent implementation & Apache-2.0 hygiene:** Derive behavior from public documentation, public samples, and observed runtime behavior only. Do not copy GPL-licensed code into this repository.

## Local Build & Verification

Requirements: Windows 10/11 x64, Visual Studio 2022+ (MSVC C++23), CMake 3.25+, Ninja, Python 3.

```powershell
# Build and run all 79/79 individual Tier 1 CTest targets (`43 cpu`, `36 gpu`) on hardware GPU
.\tools\verify.bat --verbose

# Run the full 79-test suite on Microsoft WARP (matches GitHub Actions Windows CI)
$env:SCARLETT_USE_WARP = "1"
.\tools\verify.bat --output-on-failure
$env:SCARLETT_USE_WARP = $null
```

## Coding & Repository Conventions

- Save source, header, and Markdown files as **UTF-8 without BOM** with LF line endings (`.bat` and `.ps1` use CRLF — enforced by `.gitattributes`).
- Builds must compile cleanly under MSVC `/W4 /permissive-` with 0 warnings.
- Update `docs/PROGRESS.md`, `docs/ROADMAP.md`, and `README.md` accurately and run `py tools\build_wiki.py` when adding shims, exports, or tests.
