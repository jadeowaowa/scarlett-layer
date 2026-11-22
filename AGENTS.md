# scarlett-layer Agent Instructions

- **Build & Test Command:** `.\tools\verify.bat --verbose` (must compile with 0 `/W4 /permissive-` warnings and pass 100% of registered CTests on both hardware GPU and `SCARLETT_USE_WARP=1` before merging).
- **Encoding:** Always write source, header, and Markdown files as UTF-8 without BOM with LF line endings. Use `tools\patch_lib.ps1` (`Read-Utf8`, `Replace-Exact`, `Write-Utf8`) for surgical edits.
- **Git Workflow:** Develop on dedicated branches (`sprint-NN-<slug>` or `docs/<slug>`), update `docs/PROGRESS.md`, `docs/ROADMAP.md`, and `README.md` accurately, run `py tools\build_wiki.py`, and merge `--no-ff` into `main` only after local verification passes.
- **Truthfulness First:** Report only observed, locally verified behavior. Never use inflated adjectives (`Verified`, `Hardened`, `Golden Master`, `production-ready`). Separate **Tier 1 (self-authored synthetic tests)** from **Tier 2 (external Xbox-targeted binaries)**. Mark every unverified layout or interface `ASSUMED` in code comments and `docs/SPEC_SOURCES.md`.
- **Feature & GUI Freeze:** Do **not** add new `src/gui/` features or deferred backlog subsystems (DXR, XMA2, `GameInput` v2, `EtwPlus`) until Sprints 70-72 deliver real XVD/parser hardening, independent-oracle RDNA2 shader lifting, and an unmodified Xbox-platform-targeted binary boot.
- **Test Integrity:** Never weaken or silence test assertions to force green CI. Fix the implementation or report the failure honestly.
- **Human-Only Gates:** Never close `TODO(jade)` items on the owner's behalf (independent human Windows-internals loader audit of `pe_patcher.cpp` / `scarlett_hook.cpp`, GitHub `v1.0.0` release retitling).
- **Legal & Scope Boundaries:** Reject encrypted XVD/XVC containers unconditionally. Never commit Microsoft GDK headers, retail dumps, leaked binaries, console keys, or GPL-licensed third-party code.
- **Status & Drift Check:** Run `py tools\generate_status.py`, `py tools\generate_status.py --check`, and `py tools\build_wiki.py` before committing. Never hand-edit `docs/STATUS.md`.
- **Current Baseline:** Sprint 70 (`74/74` Tier 1 synthetic CTests: `42 cpu`, `32 gpu`; `0` Tier 2 automated external Xbox-targeted binary tests; Apache-2.0). Next focus: Sprints 71-73 in `docs/ROADMAP.md`.
