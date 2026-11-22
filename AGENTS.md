# scarlett-layer Agent Instructions

- **Build & Test Command:** `.\tools\verify.bat --verbose` (must pass 100% with 0 failures and 0 `/W4` warnings before committing).
- **Status & Doc Drift Check:** Run `py tools\generate_status.py`, `py tools\generate_status.py --check`, and `py tools\build_wiki.py` before committing. Never hand-edit `docs/STATUS.md`.
- **Encoding:** Always write source and markdown files as UTF-8 without BOM with LF line endings. Use `tools\patch_lib.ps1` (`Read-Utf8`, `Replace-Exact`, `Write-Utf8`) for surgical edits.
- **Git Workflow:** Never push directly to `main`. Develop on dedicated feature/sprint branches (e.g., `sprint-NN-<slug>` or `chore/<slug>`), update `docs/PROGRESS.md` and `docs/ROADMAP.md` as needed, regenerate `docs/STATUS.md` and `docs/wiki/`, and merge via Pull Request (`--no-ff`) after review.
- **Feature & GUI Freeze:** Do **not** add new launcher/GUI features (`src/gui/`) or speculative runtime shims until the external-validation milestones in `docs/ROADMAP.md` (Sprint 70–71) are complete.
- **Test-Tier Guardrail:** Distinguish **Tier 1 (self-authored synthetic tests)** from **Tier 2 (external-input tests)** in `docs/STATUS.md`. Never weaken, silence, or modify assertions in external-input tests to make CI green—fix the implementation or report the failure honestly.
- **Human-Only Gates:** Do **not** mark `TODO(jade)` items complete on a human's behalf (including the independent Windows-internals audit of `pe_patcher.cpp` / `scarlett_hook.cpp` and GitHub release retitling).
- **No DRM / No Proprietary Material:** Reject encrypted retail XVD/XVC images unconditionally. Never commit Microsoft GDK headers, retail dumps, keys, or GPL-licensed third-party code.
- **Current Milestone:** Sprint 70–71 (differential RDNA2 shader lifter corpus, real `emoose/xvdtool` XVD header offsets `0x284`/`0x288`/`0x28C`, Xbox-platform-targeted unencrypted binary bring-up, and independent human loader audit) as tracked in `docs/ROADMAP.md`.
