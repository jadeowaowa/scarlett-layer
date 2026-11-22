# Security Policy

## Scope & Threat Model

`scarlett-layer` is an experimental local compatibility layer that launches a user-selected Windows x64 executable suspended, patches PE imports in memory, and injects `scarlett_hook.dll` into the target process. It is intended solely for running unpacked, unencrypted executables that the user owns or compiled.

- **Not a sandbox:** Running untrusted binaries through `loader.exe` executes them with your local Windows user privileges.
- **VFS containment:** `sc::vfs::TranslatePath` rejects traversal escapes outside mapped `G:\`, `T:\`, and `S:\` roots; bypasses are considered security bugs.
- **Container & PE parsers:** Integer overflows, out-of-bounds reads, or crashes when inspecting malformed PE, `MicrosoftGame.Config`, or XVD/XVC headers are security bugs.
- **Out of scope:** Encrypted XVD/XVC decryption, DRM circumvention, and retail key handling are strictly out of scope and intentionally unsupported.

## Reporting a Vulnerability

Please report security issues privately using GitHub Security Advisories on `jadeowaowa/scarlett-layer` (or open an issue without sensitive exploit details if advisories are unavailable). Include reproduction steps and the commit hash.
