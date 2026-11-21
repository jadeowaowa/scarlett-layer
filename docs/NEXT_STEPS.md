scarlett-layer: Next Steps for the Coding Agent
Read this file and `AGENTS.md` before doing anything. The repository is at the Sprint 65 baseline. This plan pauses the feature roadmap for three sprints (66-68) of fixes, hardening and honesty work. The remaining roadmap items (Velocity pipeline parity, DXR, GameInput v2, XMA2, RDNA2 FP16, EtwPlus/toolhelpx, MSIXVC2, status matrix) resume as Sprint 69+.
Why the pause
The test suite is 64 checks written by the same agent that wrote the code, from the same assumptions. Passing checks prove the code is internally consistent, not compatible with real Xbox/GDK software. Several core formats (`SCLZ`, `SCXS`) are project-defined, and `docs/ARCHITECTURE_PAPER.md` says console COM interfaces are "modeled" rather than matched. The review in `REVIEW.md` also found real bugs. Fix those and the credibility gaps before adding features.
Ground rules (apply to every sprint)
Evidence over adjectives. Never write "Verified", "Hardened", "Golden Master", or "production-ready" unless the PROGRESS entry links to a test or source that proves it. Prefer "implemented", "tested (synthetic)", "assumed", "stub".
No invented specifications. If a format, struct layout, constant, or API behavior came from your own assumption rather than a public source, label it `ASSUMED` in code comments and in `docs/SPEC_SOURCES.md`. Do not present it as real console behavior.
Never weaken a test to make it pass. If a test fails, fix the code or report the failure. Changing an assertion needs a one-line justification in the commit message.
Scope is fixed. No DRM circumvention, no decryption of retail XVD/XVC, no handling of encrypted content. Reject it and say so.
No proprietary material. Do not add Microsoft SDK headers, leaked documents, or console firmware or keys. If the build needs SDK headers, reference the user's installed SDK.
Stop and ask the human (leave a `TODO(jade):` marker, do not guess) for: what has been run on real hardware, what the author reviewed personally, and anything in the README's Legal section.
Keep the existing workflow: `sprint-NN-<slug>` branches, `.\tools\verify.bat --verbose` must pass before committing, UTF-8 without BOM via `tools\patch_lib.ps1`, update `docs/PROGRESS.md` and `README.md`, run `py tools\build_wiki.py`, merge `--no-ff`, tag `sprint-NN-baseline`, push `--tags`.
Report problems honestly in `docs/PROGRESS.md`. A sprint that finds bugs counts as a success.
Timeline
Work is measured in sprints, not dates. Each sprint ends with `verify.bat` green, a tagged baseline, and a PROGRESS entry.
Sprint	Branch slug	Goal
66	`fix-and-clean`	Fix the confirmed bugs, make the build portable, fix docs and claims
67	`audit-and-ground-truth`	Check correctness beyond self-written tests
68	`real-world-proof`	Show something real running
69+	resume roadmap	Remaining roadmap items, under the rules above
Sprint 66: Fix and clean
66.1 VFS path escape (`src/common/vfs.hpp`, `TranslatePath`): after joining and normalizing, verify the result is still inside the drive root (compare normalized root and result); reject or clamp anything that escapes via `..`. Decide and document what unmapped drive letters (for example `C:\`) should do. Add negative tests: `G:\..\..\x`, `T:\..\save_temp\x`, mixed slashes, trailing dots, and paths longer than `MAX_PATH`. Also stop using fixed `MAX_PATH` buffers in `ReadEnvPath` (use `sc::config` helpers).
66.2 XVD bounds overflow (`src/common/xvd_container.hpp`, `Open`): replace `userDataOffset + userDataLength > fileSize` with an overflow-safe form (`userDataOffset > fileSize || userDataLength > fileSize - userDataOffset`). Add tests with huge offset and length values. Check `driveSize` and `contentType` the same way if they are ever used.
66.3 Encryption flag: find a public source for the XVD volume-flag bit meanings and confirm that bit `0x1` really means "encrypted". If not, fix the check and add a test. Document the result in `docs/SPEC_SOURCES.md`.
66.4 Non-ASCII paths (`src/loader/pe_patcher.cpp` `WideToAnsi`, and the ANSI shim map in `scarlett_hook.cpp`): `CP_ACP` conversion silently corrupts shim paths when the install path has characters outside the ANSI code page. Detect a lossy conversion and fail with a clear error, or use a short (8.3) path. Add a test with a non-ASCII shim directory.
66.5 Stale module handles (`scarlett_hook.cpp` `g_seenModules`): a module that is freed and a different module loaded at the same address is never patched. Hook `FreeLibrary` to erase the handle, or key on base address plus timestamp.
66.6 Portable build: remove the hard-coded Visual Studio 18 path from `tools\build.bat` and `tools\verify.bat`. Locate the toolchain with `vswhere` or accept an already-initialized developer prompt.
66.7 Register tests individually: replace the single `scarlett_shim_suite` CTest with one CTest per test name using `scarlett_tests.exe --filter <exact name>`, so `ctest` output matches the "64 tests" claim and one failure no longer hides the others.
66.8 Docs: replace `README.md` with the version in this handoff (keep every `TODO(jade)` marker). Fix `AGENTS.md` "Next Milestone". Update `docs/ARCHITECTURE_PAPER.md` (it still says Sprint 21) and fix its mojibake (`â€”` and similar from double-encoded UTF-8). Remove or replace "clean-room", "Verified", "Hardened", "Golden Master" and "Capstone" wording with evidenced descriptions. Update `docs/ROADMAP.md` (it still lists Sprints 63-65 as future, and the actual Sprint 64 differs from the plan).
66.9 Add `.github/workflows/ci.yml` (build plus non-GPU tests, WARP adapter where possible, GPU tests marked local-only), `SECURITY.md`, `CONTRIBUTING.md`, a repo description and topics, and a `.gitattributes` that enforces UTF-8 and line endings.
Sprint 67: Audit and ground truth
67.1 Create `docs/SPEC_SOURCES.md`: one row for every format, struct, constant and API behavior the layer depends on (`SCLZ`, `SCXS`, the XVD header layout, `2D_THIN` / `64KB_S` swizzle, BC swizzle, the RDNA2 decoder, XUser and XGameSave behavior, the 13.5 GiB and 7c/14t figures, the 16-byte `XMem` alignment claim). Columns: item, where used, public source (URL or document name), status (`sourced` / `assumed` / `project-defined`). If you cannot name a public source, the status is `assumed` or `project-defined`. Do not invent citations.
67.2 Mark every `assumed` and `project-defined` item in code comments and in the README component table.
67.3 Mutation check: for each of the five test suites, deliberately break five things (a swizzle offset, a fence wait, an import rewrite, a path translation, a decode step) and confirm the matching test fails. Record results in `docs/TEST_AUDIT.md`. Fix any test that stays green.
67.4 Build with MSVC AddressSanitizer and run `/analyze`. Fix real findings and list the rest in `docs/KNOWN_ISSUES.md`.
67.5 Add malformed-input tests: truncated and hostile PE files (bad `e_lfanew`, overlapping sections, huge import counts, bad delay-load descriptors), malformed `MicrosoftGame.Config`, corrupt `SCXS` / `SCLZ` blobs, and fuzz the `SCLZ` decoder and the XVD and Config parsers.
67.6 The hand-rolled XML parsing in `game_config.hpp` does not validate `<Executable Name>`. Reject absolute paths and `..` so the executable stays inside the package root.
67.7 Scan the repo for proprietary or non-redistributable files and report in PROGRESS. Ask the human before deleting anything.
Sprint 68: Real-world proof
68.1 Add `tools\demo.bat` that runs the boot-to-frame-to-save test and saves a screenshot or short capture to `docs/media/`.
68.2 Add a "Known limitations" section to the README, generated from `docs/KNOWN_ISSUES.md` and `docs/SPEC_SOURCES.md`.
68.3 Ask the human whether any real GDK sample or homebrew title is available. If yes, attempt it and record exactly what worked and what failed. If no, say so in the README; do not imply otherwise.
68.4 Document that the loader uses early-APC injection into a suspended process. That technique can trigger antivirus or EDR warnings, so add a short "Antivirus note" to the README and keep the loader source readable and unobfuscated.
68.5 Add a "What I learned and verified myself" slot to the README for the human to fill in.
Definition of done for any sprint
`.\tools\verify.bat --verbose` passes with zero failures and zero warnings.
`docs/PROGRESS.md` states what changed, what was found broken, and what remains assumed.
No new claim in the README without evidence.
Branch merged with `--no-ff`, tagged `sprint-NN-baseline`, and tags pushed.
