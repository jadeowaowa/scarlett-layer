# 05 â€” Automated Test Suite & Synthetic Title Index

`scarlett_tests.exe` runs **57 checks** via `loader.exe` under CTest (`ctest --test-dir build`).

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
// 27
    RunTest("Rdna2ImageSampleShader",            TestRdna2ImageSampleShader);
// 28
    RunTest("PresentXDescriptorAudit",           TestPresentXDescriptorAudit);
// 29
    RunTest("ScarlettXConformanceSuite",         TestScarlettXConformanceSuite);
// 30
    RunTest("GameConfigBootParser",              TestGameConfigBootParser);
// 31
    RunTest("Rdna2TexturedDrawE2E",              TestRdna2TexturedDrawE2E);
// 32
    RunTest("GameInputLivePolling",              TestGameInputLivePolling);
// 33
    RunTest("DynamicLoaderHooks",                TestDynamicLoaderHooks);
// 34
    RunTest("DStorageGpuBufferAndFenceE2E",      TestDStorageGpuBufferAndFenceE2E);
// 35
    RunTest("Rdna2ComputeDispatchE2E",           TestRdna2ComputeDispatchE2E);
// 36
    RunTest("ShaderDiskCacheE2E",                TestShaderDiskCacheE2E);
// 37
    RunTest("D3D12XsCommandListWrapperE2E",      TestD3D12XsCommandListWrapperE2E);
// 38
    RunTest("D3D12XsCmdListRenderClearE2E",      TestD3D12XsCmdListRenderClearE2E);
// 39
    RunTest("D3D12XsCmdListComputeDispatchE2E",  TestD3D12XsCmdListComputeDispatchE2E);
// 40
    RunTest("XAudio2RuntimePlaybackE2E",         TestXAudio2RuntimePlaybackE2E);
// 41
    RunTest("NetworkingShimsE2E",                TestNetworkingShimsE2E);
// 42
    RunTest("XUserAndGameSaveProfileE2E",        TestXUserAndGameSaveProfileE2E);
// 43
    RunTest("GameInputRuntimePollingE2E",        TestGameInputRuntimePollingE2E);
// 44
    RunTest("ScarlettTitleBringUpE2E",           TestScarlettTitleBringUpE2E);
// 45
    RunTest("KernelXAndPixEvtE2E",               TestKernelXAndPixEvtE2E);
// 46
    RunTest("XMemAndXgXsSwizzleE2E",             TestXMemAndXgXsSwizzleE2E);
// 47
    RunTest("DStorageVelocityDynamicE2E",        TestDStorageVelocityDynamicE2E);
// 48
    RunTest("LoaderXvdAndGameConfigE2E",         TestLoaderXvdAndGameConfigE2E);
// 49
    RunTest("ScarlettFullStack50SprintCapstoneE2E", TestScarlettFullStack50SprintCapstoneE2E);
// 50
    RunTest("PeDelayLoadAndOrdinalAuditE2E",     TestPeDelayLoadAndOrdinalAuditE2E);
// 51
    RunTest("RuntimeDelayLoadAndOrdinalHookE2E", TestRuntimeDelayLoadAndOrdinalHookE2E);
// 52
    RunTest("D3D12Xs3DPerspectiveDepthRenderE2E", TestD3D12Xs3DPerspectiveDepthRenderE2E);
// 53
    RunTest("D3D12XsComVtableAbiConformanceE2E",  TestD3D12XsComVtableAbiConformanceE2E);
// 54
    RunTest("D3D12XsPresentXSwapChainE2E",        TestD3D12XsPresentXSwapChainE2E);
// 55
    RunTest("D3D12XsFramePacingTokensE2E",        TestD3D12XsFramePacingTokensE2E);
// 56
    RunTest("D3D12XsHeadlessPresentXModeE2E",     TestD3D12XsHeadlessPresentXModeE2E);
// 57
    RunTest("D3D12XsAsyncComputeCrossQueueSyncE2E", TestD3D12XsAsyncComputeCrossQueueSyncE2E);
```

## Test & Title Files
- `src/tests/dummy_title/main.cpp` (1651 lines)
- `src/tests/engine_plugin/engine_plugin.cpp` (59 lines)
- `src/tests/engine_plugin/engine_plugin.def` (3 lines)
- `src/tests/scarlett_3d_demo/main.cpp` (328 lines)
- `src/tests/scarlett_tests/main.cpp` (6534 lines)
