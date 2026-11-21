# scarlett-layer

![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078D6?style=flat-square)
![Standard](https://img.shields.io/badge/C%2B%2B-C%2B%2B23%20%2F%20MSVC-00599C?style=flat-square)
![Build](https://img.shields.io/badge/build-CMake%20%2B%20Ninja-064F8C?style=flat-square)
![Tests](https://img.shields.io/badge/CTest-29%2F29%20Passing-2EA043?style=flat-square)
![Baseline](https://img.shields.io/badge/baseline-sprint--30--baseline-blueviolet?style=flat-square)

**`scarlett-layer`** is a Windows x64 translation and shim environment designed to bridge Xbox Series X|S (Scarlett / Gen9) GDK APIs, PE binaries, and RDNA2 (GFX10.3) shaders onto native PC Direct3D 12 hardware.

Built with a strict **Test-Driven Development (TDD)** workflow, every subsystem, binary decoder, and COM ABI surface is verified via automated CTest suites (`29/29` passing).

---

## Architecture Overview

```text
+-------------------------------------------------------------------+
|                     Scarlett / Gen9 Title PE                      |
+---------------------------------+---------------------------------+
                                  |
                  loader.exe (PE Patch / VFS / XVD)
                      + scarlett_hook.dll (IAT)
                                  |
       +--------------------------+--------------------------+
       v                          v                          v
+--------------+         +-----------------+        +----------------+
|  d3d12_xs    |         |  xgameruntime   |        |   Middleware   |
|  - DeviceX   |         |  - XTaskQueue   |        |  - gameinput   |
|  - PresentX  |         |  - XAsync/XUser |        |  - AcpHal      |
|  - RDNA2 ISA |         |  - XGameSave    |        |  - libHttp/XCurl
|    -> HLSL   |         +-----------------+        +----------------+
+------+-------+
       |
       v
+-------------------------------------------------------------------+
|               Host PC Windows x64 + Direct3D 12 GPU               |
+-------------------------------------------------------------------+