# Scarlett Layer — Engineering Invariants & Sharp Edges

> **Hand-maintained file.** `tools/build_wiki.py` never overwrites this document and embeds it verbatim into `AI_CONTEXT_SNAPSHOT.md`. Only record non-obvious architectural constraints, past failure modes, and hard rules here.

## 1. Loader, PE Rewriting & Runtime Hooking
- **Near-4GB RVA Window:** PE32+ `IMAGE_IMPORT_DESCRIPTOR::Name` and `IMAGE_DELAYLOAD_DESCRIPTOR::DllNameRVA` are 32-bit unsigned RVAs relative to `ImageBase`. Remote shim path strings MUST be committed inside `[ImageBase, ImageBase + 0xFFFFFFFF]` via `VirtualQueryEx` scanning, then hardened to `PAGE_READONLY`.
- **Clear Bound Imports:** Always zero `IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT` when rewriting static imports so `ntdll` does not short-circuit patched descriptors with stale timestamps.
- **Loader-Lock Safety (`scarlett_hook.dll`):** Inside `DllMain`, avoid C++ filesystem iterators and heavy CRT allocations; enumerate shims with raw Win32 `FindFirstFileW`.
- **Recursion & System Exclusion:** Never IAT-hook modules residing under `%SystemRoot%` or `SCARLETT_SHIM_DIR`. This guarantees `xaudio2_9.dll` can load native `System32\xaudio2_9.dll` without self-recursion.
- **Search-Flag Sanitization:** When redirecting `LoadLibraryExA/W` to an absolute shim path outside `System32`, mask off `LOAD_LIBRARY_SEARCH_*` flags (`SYSTEM32`, `APPLICATION_DIR`, `USER_DIRS`, `DEFAULT_DIRS`) to prevent `ERROR_INVALID_PARAMETER`.

## 2. Graphics, PSO Caching & Binary RDNA2 Lifting (`d3d12_xs.dll`)
- **No `D3DCOMPILE_DEBUG` on Cached Shaders:** Compiling HLSL with `D3DCOMPILE_DEBUG` embeds a `SPDB` chunk with a freshly generated GUID on every run, breaking cross-launch PSO cache determinism. Always compile runtime/lifted shaders with `D3DCOMPILE_OPTIMIZATION_LEVEL3`.
- **Post-Translation Content Hashing:** Key `ID3D12PipelineLibrary` entries (`GFX_<hash>`, `CMP_<hash>`) on **post-translation** DXBC/DXIL bytes plus the content hash of serialized root-signature blobs—never raw COM pointer addresses.
- **Translator Precedence & Error Contracts:** `Rdna2IsaTranslator` MUST be registered ahead of `ScxsContainerTranslator` in `ShaderTranslatorRegistry`. Corrupted `SCXS` checksums return `E_INVALIDARG`; missing required stage outputs (`EXP pos0` for VS, `EXP mrt0` for PS, `MUBUF` store for CS) return `E_NOTIMPL`.
- **Cache-Stable HLSL Emission:** Keep lifted Pixel Shader HLSL byte-identical when no `SMEM` or `VINTRP` instructions are present so existing cached PSO keys (`GFX_F15306853EDC188B`) do not churn across refactors.
- **Cross-Queue GPU Synchronization:** Handoffs between `COMPUTE` and `DIRECT` queues must transition resources through `D3D12_RESOURCE_STATE_COMMON` paired with GPU-side `Signal`/`Wait` fences to maintain zero D3D12 Debug Layer warnings. Buffer copies after compute writes require a UAV barrier plus `UNORDERED_ACCESS -> COPY_SOURCE`.

## 3. Memory, Audio & Storage Shims
- **Unified Memory & Alignment (`xmem.dll`, `kernelx.dll`):** Route `XALLOC_MEMTYPE_PHYSICAL` and `>= 64 KB` alignments to `VirtualAlloc`; smaller heap pools to `_aligned_malloc`. Strip console-only flags (`SC_MEM_GRAPHICS = 0x10000000`) before calling host `VirtualAlloc`, clamp oversized affinity masks against host process affinity (`0xFFF` on 12-thread testbed), and maintain **0 leaks (0 B)** on unload.
- **Audio Proxy & Decode Lifetimes (`xaudio2_9.dll`):** Resolve native `xaudio2_9.dll` via `GetSystemDirectoryW` and assert `mod != g_selfModule`. `ScarlettDecodingSourceVoice` owns decoded PCM16 blocks until `OnBufferEnd` via an internal `IXAudio2VoiceCallback` proxy and translates `pContext` back to the caller's pointer in callbacks and `GetState()`.
- **Velocity Streaming (`dstorage_xs.dll`):** Translate `G:\`, `T:\`, and `S:\` VFS paths in `OpenFile`. Run `SCLZ` decompression and FNV-1a verification on the background worker thread *before* updating status slots or signaling `ID3D12Fence`.

## 4. Build, Testing & Documentation Policy
- **Toolchain & Encoding:** MSVC x64 C++23 (`/W4 /permissive-`), CMake + Ninja, UTF-8 without BOM. CTest always launches `scarlett_tests.exe` *through* `loader.exe` to verify full-stack interposition.
- **Docs Cadence:** Update `docs/PROGRESS.md` every sprint; regenerate `docs/wiki/` locally via `py tools\build_wiki.py` every 3–5 commits (zero AI tokens); update `docs/ARCHITECTURE_PAPER.md` strictly every 5 sprints (next scheduled revision: **Sprint 26**).