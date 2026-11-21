scarlett-layer: Next Steps for the Coding Agent
Read this file and `AGENTS.md` before doing anything. This plan pauses the feature roadmap (old Sprints 63-72) for three sprints of hardening and honesty work. The old roadmap resumes afterward, with sprint numbers shifted by 3.
Why the pause
The project has 64 passing tests, but the same agent wrote the code and the tests, from the same assumptions. Passing tests prove the code is internally consistent, not that it is compatible with real Xbox/GDK software. The next three sprints close that gap and fix the project's credibility problems before any new features are added.
Ground rules (apply to every sprint)
Evidence over adjectives. Never write "Verified", "Hardened", "Golden Master", or "production-ready" unless the PROGRESS entry links to a test or source that proves it. Prefer "implemented", "tested (synthetic)", "assumed", "stub".
No invented specifications. If a format, struct layout, constant, or API behavior came from your own assumption rather than a public source, label it `ASSUMED` in code comments and in `docs/SPEC_SOURCES.md`. Do not present it as real console behavior.
Never weaken a test to make it pass. If a test fails, fix the code or report the failure. Changing an assertion needs a one-line justification in the commit message.
Scope is fixed. No DRM circumvention, no decryption of retail XVD/XVC, no handling of encrypted content. Reject it and say so.
No proprietary material. Do not add Microsoft SDK headers, leaked documents, or console firmware or keys. If the build needs SDK headers, reference the user's installed SDK instead of copying files into the repo.
Stop and ask the human (leave a `TODO(jade):` marker, do not guess) for: what has actually been run on real hardware, what the author reviewed personally, and anything in the README's Legal section.
Keep the existing workflow: `sprint-NN-<slug>` branches, `.\tools\verify.bat --verbose` must pass before committing, UTF-8 without BOM via `tools\patch_lib.ps1`, update `docs/PROGRESS.md` and `README.md`, run `py tools\build_wiki.py`, merge `--no-ff`, tag `sprint-NN-baseline`, push `--tags`.
Report problems honestly in `docs/PROGRESS.md`. A sprint that finds bugs counts as a success.
Timeline
Work is measured in sprints, not dates. Each sprint should end with `verify.bat` green, a tagged baseline, and a PROGRESS entry.
Sprint	Branch slug	Goal	Done when
63	`housekeeping-and-honesty`	Fix docs, packaging and claims	Items 63.1-63.7 are checked off
64	`audit-and-ground-truth`	Check correctness beyond self-written tests	Items 64.1-64.6 are checked off
65	`real-world-proof`	Show something real running	Items 65.1-65.4 are checked off
66+	resume roadmap	Old Sprints 63-72 become 66-75	Each feature sprint passes the new rules above
Sprint 63: Housekeeping and honesty
63.1 Replace `README.md` with the drafted version in this repo's handoff (keep every `TODO(jade)` marker; do not fill them in yourself).
63.2 Confirm `LICENSE` is Apache-2.0 and that the README badge matches.
63.3 Fix `AGENTS.md`: replace the stale "Next Milestone" line with "See `docs/ROADMAP.md` and `NEXT_STEPS.md`."
63.4 Make version labels consistent: one current baseline tag, no sprint-62 vs sprint-65 mismatch anywhere in the docs.
63.5 Search the repo for the words "clean-room", "Verified", "Hardened", "Golden Master", "Capstone", and "production". Remove or replace each with an evidenced, plain description.
63.6 Add `.github/workflows/ci.yml`: build with MSVC and CMake, run tests that do not need a GPU (use the WARP software adapter where possible), and mark GPU-only tests as local-only. Add the real CI badge.
63.7 Add `SECURITY.md` (how to report issues) and a short `CONTRIBUTING.md` (what is out of scope, including anything touching encrypted content).
Sprint 64: Audit and ground truth
64.1 Create `docs/SPEC_SOURCES.md`: a table with one row for every format, struct, constant and API behavior the layer depends on (for example SCLZ, the swizzle modes, the RDNA2 decoder, XUser and XGameSave behavior, the 13.5 GiB and 7c/14t figures). Columns: item, where it is used, public source (URL or document name), and status (`sourced` / `assumed` / `placeholder`). If you cannot name a public source, the status is `assumed`. Do not invent citations.
64.2 Mark every `assumed` or `placeholder` item in code comments and in the README component table.
64.3 Mutation check: for each of the five test suites, deliberately break five things (a swizzle offset, a fence wait, an import rewrite, a path translation, a decode step) and confirm the matching test fails. Record the results in `docs/TEST_AUDIT.md`. Fix any test that stays green.
64.4 Build with MSVC AddressSanitizer and run `/analyze`. Fix real findings and list the rest in `docs/KNOWN_ISSUES.md`.
64.5 Security review of `loader.exe`, `scarlett_hook.dll` and the virtual file system: path traversal in `G:\`, `T:\`, `S:\` mounts, unchecked PE header fields, integer overflows in import rewriting, and unsafe handling of malformed input. Add negative tests with malformed PE files and hostile paths.
64.6 Scan the repo for proprietary or non-redistributable files. Report the results in PROGRESS and ask the human before deleting anything.
Sprint 65: Real-world proof
65.1 Add `tools\demo.bat` that runs the boot-to-frame-to-save test and saves a screenshot or a short capture to `docs/media/`.
65.2 Add a "Known limitations" section to the README, generated from `docs/KNOWN_ISSUES.md` and `docs/SPEC_SOURCES.md`.
65.3 Ask the human whether any real GDK sample or homebrew title is available to test. If yes, attempt it and record exactly what worked and what failed, including crashes. If no, say so in the README; do not imply otherwise.
65.4 Add a "What I learned and verified myself" slot to the README for the human to fill in.
After Sprint 65
Resume the feature roadmap (old 63-72). For each feature sprint, add to the PROGRESS entry: the public source for any new format or behavior, the new tests, and the mutation check result for those tests.
Definition of done for any sprint
`.\tools\verify.bat --verbose` passes with zero failures and zero warnings.
`docs/PROGRESS.md` states what changed, what was found broken, and what remains assumed.
No new claim in the README without evidence.
Branch merged with `--no-ff`, tagged `sprint-NN-baseline`, and tags pushed.
