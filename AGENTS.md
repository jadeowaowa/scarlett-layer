# scarlett-layer Agent Instructions

- **Build & Test Command:** `.\tools\verify.bat --verbose` (must pass 100% with 0 failures before committing).
- **Encoding:** Always write source and markdown files as UTF-8 without BOM. Use `tools\patch_lib.ps1` (`Read-Utf8`, `Replace-Exact`, `Write-Utf8`) for surgical edits.
- **Git Workflow:** Develop on `sprint-NN-<slug>` branches, update `docs/PROGRESS.md` and `README.md`, run `py tools\build_wiki.py`, merge `--no-ff` into `main`, tag `sprint-NN-baseline`, and push `--tags`.
- **Next Milestone:** Sprints 66-68 (fix and clean, audit, real-world proof) as described in docs/NEXT_STEPS.md, then docs/ROADMAP.md.
