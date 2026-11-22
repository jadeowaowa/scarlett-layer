# 05 — Automated Test Suite & Synthetic Title Index

`scarlett_tests.exe` runs **74 checks** via `loader.exe` under CTest (`ctest --test-dir build`).

## Exact `RunTest` Registration Order (`src/tests/scarlett_tests/main.cpp`)
```cpp
// 01
    {"VfsPathTranslation",                 TestVfsPathTranslation},
// 02
    {"RuntimeAndXUser",                    TestRuntimeAndXUser},
// 03
    {"XGameSaveEdgeCases",                 TestXGameSaveEdgeCases},
// 04
    {"XgXsMultiMipAndNpotSwizzle",         TestXgXsMultiMipAndNpotSwizzle},
// 05
    {"DStorageVfsAndErrorRecord",          TestDStorageVfsAndErrorRecord},
// 06
    {"XAudio2Xma2Rejection",               TestXAudio2Xma2Rejection},
// 07
    {"XMemAlignmentAndInvariants",         TestXMemAlignmentAndInvariants},
// 08
    {"DynamicAndMultiModuleHooks",         TestDynamicAndMultiModuleHooks},
// 09
    {"D3D12HeadlessDeviceAndPso",          TestD3D12HeadlessDeviceAndPso},
// 10
    {"KernelXMemoryAndTopology",           TestKernelXMemoryAndTopology},
// 11
    {"D3D12PlacedResourcesAndHeap",        TestD3D12PlacedResourcesAndHeap},
// 12
    {"DxilSm6BitcodeInspector",            TestDxilSm6BitcodeInspector},
// 13
    {"ScxsTranslatorAndPsoCache",          TestScxsTranslatorAndPsoCache},
// 14
    {"DStorageLzDecompression",            TestDStorageLzDecompression},
// 15
    {"XAudio2ImaAdpcmDecodeBridge",        TestXAudio2ImaAdpcmDecodeBridge},
// 16
    {"D3D12ComputePsoAndUavExecution",     TestD3D12ComputePsoAndUavExecution},
// 17
    {"Rdna2IsaDecoderAndGpuExecution",     TestRdna2IsaDecoderAndGpuExecution},
// 18
    {"D3D12AsyncComputeCrossQueueSync",    TestD3D12AsyncComputeCrossQueueSync},
// 19
    {"Rdna2ComputeIsaExecution",           TestRdna2ComputeIsaExecution},
// 20
    {"Rdna2VertexAndPixelPipelineWithCbv", TestRdna2VertexAndPixelPipelineWithCbv},
// 21
    {"XgXs64KbsAndBcnBlockSwizzle",        TestXgXs64KbsAndBcnBlockSwizzle},
// 22
    {"DStorageGpuBufferStreaming",         TestDStorageGpuBufferStreaming},
// 23
    {"DStorageGpuTextureStreaming",        TestDStorageGpuTextureStreaming},
// 24
    {"DStorageDeviceXBridgeAndMipChain",   TestDStorageDeviceXBridgeAndMipChain},
// 25
    {"RetailBringUpShims",                 TestRetailBringUpShims},
// 26
    {"LoaderInspectAndXvdMount",           TestLoaderInspectAndXvdMount},
// 27
    {"Rdna2ImageSampleShader",             TestRdna2ImageSampleShader},
// 28
    {"PresentXDescriptorAudit",            TestPresentXDescriptorAudit},
// 29
    {"ScarlettXConformanceSuite",          TestScarlettXConformanceSuite},
// 30
    {"GameConfigBootParser",               TestGameConfigBootParser},
// 31
    {"Rdna2TexturedDrawE2E",               TestRdna2TexturedDrawE2E},
// 32
    {"GameInputLivePolling",               TestGameInputLivePolling},
// 33
    {"DynamicLoaderHooks",                 TestDynamicLoaderHooks},
// 34
    {"DStorageGpuBufferAndFenceE2E",       TestDStorageGpuBufferAndFenceE2E},
// 35
    {"Rdna2ComputeDispatchE2E",            TestRdna2ComputeDispatchE2E},
// 36
    {"ShaderDiskCacheE2E",                 TestShaderDiskCacheE2E},
// 37
    {"D3D12XsCommandListWrapperE2E",       TestD3D12XsCommandListWrapperE2E},
// 38
    {"D3D12XsCmdListRenderClearE2E",       TestD3D12XsCmdListRenderClearE2E},
// 39
    {"D3D12XsCmdListComputeDispatchE2E",   TestD3D12XsCmdListComputeDispatchE2E},
// 40
    {"XAudio2RuntimePlaybackE2E",          TestXAudio2RuntimePlaybackE2E},
// 41
    {"NetworkingShimsE2E",                 TestNetworkingShimsE2E},
// 42
    {"XUserAndGameSaveProfileE2E",         TestXUserAndGameSaveProfileE2E},
// 43
    {"GameInputRuntimePollingE2E",         TestGameInputRuntimePollingE2E},
// 44
    {"ScarlettTitleBringUpE2E",            TestScarlettTitleBringUpE2E},
// 45
    {"KernelXAndPixEvtE2E",                TestKernelXAndPixEvtE2E},
// 46
    {"XMemAndXgXsSwizzleE2E",              TestXMemAndXgXsSwizzleE2E},
// 47
    {"DStorageVelocityDynamicE2E",         TestDStorageVelocityDynamicE2E},
// 48
    {"LoaderXvdAndGameConfigE2E",          TestLoaderXvdAndGameConfigE2E},
// 49
    {"ScarlettFullStack50SprintCapstoneE2E", TestScarlettFullStack50SprintCapstoneE2E},
// 50
    {"PeDelayLoadAndOrdinalAuditE2E",      TestPeDelayLoadAndOrdinalAuditE2E},
// 51
    {"RuntimeDelayLoadAndOrdinalHookE2E",  TestRuntimeDelayLoadAndOrdinalHookE2E},
// 52
    {"D3D12Xs3DPerspectiveDepthRenderE2E", TestD3D12Xs3DPerspectiveDepthRenderE2E},
// 53
    {"D3D12XsComVtableAbiConformanceE2E",  TestD3D12XsComVtableAbiConformanceE2E},
// 54
    {"D3D12XsPresentXSwapChainE2E",        TestD3D12XsPresentXSwapChainE2E},
// 55
    {"D3D12XsFramePacingTokensE2E",        TestD3D12XsFramePacingTokensE2E},
// 56
    {"D3D12XsHeadlessPresentXModeE2E",     TestD3D12XsHeadlessPresentXModeE2E},
// 57
    {"D3D12XsAsyncComputeCrossQueueSyncE2E", TestD3D12XsAsyncComputeCrossQueueSyncE2E},
// 58
    {"XMemMin16ByteAlignment",             TestXMemMin16ByteAlignment},
// 59
    {"Pe64ImplicitTlsAndGuestAtExitE2E",   TestPe64ImplicitTlsAndGuestAtExitE2E},
// 60
    {"AutoStubThunkSurvivalE2E",           TestAutoStubThunkSurvivalE2E},
// 61
    {"LoaderSymbolCoverageAuditE2E",       TestLoaderSymbolCoverageAuditE2E},
// 62
    {"DxbcDigestAndXboxChunkStripperE2E",  TestDxbcDigestAndXboxChunkStripperE2E},
// 63
    {"D3D12XsEnhancedBarriersE2E",         TestD3D12XsEnhancedBarriersE2E},
// 64
    {"D3D12XsFeatureCapsQueryE2E",         TestD3D12XsFeatureCapsQueryE2E},
// 65
    {"VfsPathEscapeAndLongPaths",          TestVfsPathEscapeAndLongPaths},
// 66
    {"XvdFlagsAndBoundsNegative",          TestXvdFlagsAndBoundsNegative},
// 67
    {"NonAsciiShimAndFreeLibraryHook",     TestNonAsciiShimAndFreeLibraryHook},
// 68
    {"WarpAdapterRenderReadback",          TestWarpAdapterRenderReadback},
// 69
    {"LocalCorpusReadOnlyAudit",           TestLocalCorpusReadOnlyAudit},
// 70
    {"D3D12XsGdkSampleAlignmentE2E",       TestD3D12XsGdkSampleAlignmentE2E},
// 71
    {"XGameRuntimeComBrokerE2E",           TestXGameRuntimeComBrokerE2E},
// 72
    {"ScarlettStudioSelfTest",             TestScarlettStudioSelfTest},
// 73
    {"XvdRealOffsetsAndGeometry",          TestXvdRealOffsetsAndGeometry},
// 74
    {"MalformedPeAndConfigHardening",      TestMalformedPeAndConfigHardening},
```

## Test & Title Files
- `src/tests/dummy_title/main.cpp` (1651 lines)
- `src/tests/engine_plugin/engine_plugin.cpp` (59 lines)
- `src/tests/engine_plugin/engine_plugin.def` (3 lines)
- `src/tests/scarlett_3d_demo/main.cpp` (328 lines)
- `src/tests/scarlett_tests/main.cpp` (194 lines)
- `src/tests/scarlett_tests/test_harness.hpp` (176 lines)
- `src/tests/scarlett_tests/tests_d3d12_core.cpp` (1312 lines)
- `src/tests/scarlett_tests/tests_d3d12_shaders_present.cpp` (1949 lines)
- `src/tests/scarlett_tests/tests_loader_e2e.cpp` (2009 lines)
- `src/tests/scarlett_tests/tests_runtime_io.cpp` (967 lines)
- `src/tests/scarlett_tests/tests_storage_swizzle.cpp` (1597 lines)
