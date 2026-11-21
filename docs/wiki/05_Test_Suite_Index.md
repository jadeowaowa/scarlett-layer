# 05 â€” Automated Test Suite & Synthetic Title Index

`scarlett_tests.exe` runs **26 checks** via `loader.exe` under CTest (`ctest --test-dir build`).

## Exact `RunTest` Registration Order (`src/tests/scarlett_tests/main.cpp`)
```cpp
// 01
    RunTest("VfsPathTranslation",         TestVfsPathTranslation);
// 02
    RunTest("RuntimeAndXUser",            TestRuntimeAndXUser);
// 03
    RunTest("XGameSaveEdgeCases",         TestXGameSaveEdgeCases);
// 04
    RunTest("XgXsMultiMipAndNpotSwizzle", TestXgXsMultiMipAndNpotSwizzle);
// 05
    RunTest("DStorageVfsAndErrorRecord",  TestDStorageVfsAndErrorRecord);
// 06
    RunTest("XAudio2Xma2Rejection",       TestXAudio2Xma2Rejection);
// 07
    RunTest("XMemAlignmentAndInvariants", TestXMemAlignmentAndInvariants);
// 08
    RunTest("DynamicAndMultiModuleHooks", TestDynamicAndMultiModuleHooks);
// 09
    RunTest("D3D12HeadlessDeviceAndPso",  TestD3D12HeadlessDeviceAndPso);
// 10
    RunTest("KernelXMemoryAndTopology",   TestKernelXMemoryAndTopology);
// 11
    RunTest("D3D12PlacedResourcesAndHeap", TestD3D12PlacedResourcesAndHeap);
// 12
    RunTest("DxilSm6BitcodeInspector",    TestDxilSm6BitcodeInspector);
// 13
    RunTest("ScxsTranslatorAndPsoCache",  TestScxsTranslatorAndPsoCache);
// 14
    RunTest("DStorageLzDecompression",    TestDStorageLzDecompression);
// 15
    RunTest("XAudio2ImaAdpcmDecodeBridge", TestXAudio2ImaAdpcmDecodeBridge);
// 16
    RunTest("D3D12ComputePsoAndUavExecution", TestD3D12ComputePsoAndUavExecution);
// 17
    RunTest("Rdna2IsaDecoderAndGpuExecution", TestRdna2IsaDecoderAndGpuExecution);
// 18
    RunTest("D3D12AsyncComputeCrossQueueSync", TestD3D12AsyncComputeCrossQueueSync);
// 19
    RunTest("Rdna2ComputeIsaExecution", TestRdna2ComputeIsaExecution);
// 20
    RunTest("Rdna2VertexAndPixelPipelineWithCbv", TestRdna2VertexAndPixelPipelineWithCbv);
// 21
    RunTest("XgXs64KbsAndBcnBlockSwizzle",       TestXgXs64KbsAndBcnBlockSwizzle);
// 22
    RunTest("DStorageGpuBufferStreaming",        TestDStorageGpuBufferStreaming);
// 23
    RunTest("DStorageGpuTextureStreaming",       TestDStorageGpuTextureStreaming);
// 24
    RunTest("DStorageDeviceXBridgeAndMipChain",  TestDStorageDeviceXBridgeAndMipChain);
// 25
    RunTest("RetailBringUpShims",                TestRetailBringUpShims);
// 26
    RunTest("LoaderInspectAndXvdMount",          TestLoaderInspectAndXvdMount);
```

## Test & Title Files
- `src/tests/dummy_title/main.cpp` (1651 lines)
- `src/tests/engine_plugin/engine_plugin.cpp` (59 lines)
- `src/tests/engine_plugin/engine_plugin.def` (3 lines)
- `src/tests/scarlett_tests/main.cpp` (2936 lines)
