# scarlett-layer Agent Instructions

- **Build & Test Command:** `.\tools\verify.bat --verbose` (must pass 100% with 0 failures before committing).
- **Encoding:** Always write source and markdown files as UTF-8 without BOM. Use `tools\patch_lib.ps1` (`Read-Utf8`, `Replace-Exact`, `Write-Utf8`) for surgical edits.
- **Git Workflow:** Develop on `sprint-NN-<slug>` branches, update `docs/PROGRESS.md` and `README.md`, run `py tools\build_wiki.py`, merge `--no-ff` into `main`, tag `sprint-NN-baseline`, and push `--tags`.
- **Next Milestone:** Phase 3 (Sprints 41–45) — `xaudio2_9`/`AcpHal` runtime audio path, `XCurl`/`libHttpClient` offline stubs, and first Scarlett title bring-up.
