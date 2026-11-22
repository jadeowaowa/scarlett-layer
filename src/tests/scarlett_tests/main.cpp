#include "test_harness.hpp"

#include <cctype>
#include <string_view>

bool TestVfsPathEscapeAndLongPaths();
bool TestXvdFlagsAndBoundsNegative();
bool TestNonAsciiShimAndFreeLibraryHook();
bool TestWarpAdapterRenderReadback();
bool TestLocalCorpusReadOnlyAudit();
bool TestD3D12XsGdkSampleAlignmentE2E();
bool TestXGameRuntimeComBrokerE2E();
bool TestScarlettStudioSelfTest();

namespace {

struct TestEntry {
    const char* name;
    bool (*fn)();
};

constexpr TestEntry kTests[] = {
    {"VfsPathTranslation",                 TestVfsPathTranslation},
    {"RuntimeAndXUser",                    TestRuntimeAndXUser},
    {"XGameSaveEdgeCases",                 TestXGameSaveEdgeCases},
    {"XgXsMultiMipAndNpotSwizzle",         TestXgXsMultiMipAndNpotSwizzle},
    {"DStorageVfsAndErrorRecord",          TestDStorageVfsAndErrorRecord},
    {"XAudio2Xma2Rejection",               TestXAudio2Xma2Rejection},
    {"XMemAlignmentAndInvariants",         TestXMemAlignmentAndInvariants},
    {"DynamicAndMultiModuleHooks",         TestDynamicAndMultiModuleHooks},
    {"D3D12HeadlessDeviceAndPso",          TestD3D12HeadlessDeviceAndPso},
    {"KernelXMemoryAndTopology",           TestKernelXMemoryAndTopology},
    {"D3D12PlacedResourcesAndHeap",        TestD3D12PlacedResourcesAndHeap},
    {"DxilSm6BitcodeInspector",            TestDxilSm6BitcodeInspector},
    {"ScxsTranslatorAndPsoCache",          TestScxsTranslatorAndPsoCache},
    {"DStorageLzDecompression",            TestDStorageLzDecompression},
    {"XAudio2ImaAdpcmDecodeBridge",        TestXAudio2ImaAdpcmDecodeBridge},
    {"D3D12ComputePsoAndUavExecution",     TestD3D12ComputePsoAndUavExecution},
    {"Rdna2IsaDecoderAndGpuExecution",     TestRdna2IsaDecoderAndGpuExecution},
    {"D3D12AsyncComputeCrossQueueSync",    TestD3D12AsyncComputeCrossQueueSync},
    {"Rdna2ComputeIsaExecution",           TestRdna2ComputeIsaExecution},
    {"Rdna2VertexAndPixelPipelineWithCbv", TestRdna2VertexAndPixelPipelineWithCbv},
    {"XgXs64KbsAndBcnBlockSwizzle",        TestXgXs64KbsAndBcnBlockSwizzle},
    {"DStorageGpuBufferStreaming",         TestDStorageGpuBufferStreaming},
    {"DStorageGpuTextureStreaming",        TestDStorageGpuTextureStreaming},
    {"DStorageDeviceXBridgeAndMipChain",   TestDStorageDeviceXBridgeAndMipChain},
    {"RetailBringUpShims",                 TestRetailBringUpShims},
    {"LoaderInspectAndXvdMount",           TestLoaderInspectAndXvdMount},
    {"Rdna2ImageSampleShader",             TestRdna2ImageSampleShader},
    {"PresentXDescriptorAudit",            TestPresentXDescriptorAudit},
    {"ScarlettXConformanceSuite",          TestScarlettXConformanceSuite},
    {"GameConfigBootParser",               TestGameConfigBootParser},
    {"Rdna2TexturedDrawE2E",               TestRdna2TexturedDrawE2E},
    {"GameInputLivePolling",               TestGameInputLivePolling},
    {"DynamicLoaderHooks",                 TestDynamicLoaderHooks},
    {"DStorageGpuBufferAndFenceE2E",       TestDStorageGpuBufferAndFenceE2E},
    {"Rdna2ComputeDispatchE2E",            TestRdna2ComputeDispatchE2E},
    {"ShaderDiskCacheE2E",                 TestShaderDiskCacheE2E},
    {"D3D12XsCommandListWrapperE2E",       TestD3D12XsCommandListWrapperE2E},
    {"D3D12XsCmdListRenderClearE2E",       TestD3D12XsCmdListRenderClearE2E},
    {"D3D12XsCmdListComputeDispatchE2E",   TestD3D12XsCmdListComputeDispatchE2E},
    {"XAudio2RuntimePlaybackE2E",          TestXAudio2RuntimePlaybackE2E},
    {"NetworkingShimsE2E",                 TestNetworkingShimsE2E},
    {"XUserAndGameSaveProfileE2E",         TestXUserAndGameSaveProfileE2E},
    {"GameInputRuntimePollingE2E",         TestGameInputRuntimePollingE2E},
    {"ScarlettTitleBringUpE2E",            TestScarlettTitleBringUpE2E},
    {"KernelXAndPixEvtE2E",                TestKernelXAndPixEvtE2E},
    {"XMemAndXgXsSwizzleE2E",              TestXMemAndXgXsSwizzleE2E},
    {"DStorageVelocityDynamicE2E",         TestDStorageVelocityDynamicE2E},
    {"LoaderXvdAndGameConfigE2E",          TestLoaderXvdAndGameConfigE2E},
    {"ScarlettFullStack50SprintCapstoneE2E", TestScarlettFullStack50SprintCapstoneE2E},
    {"PeDelayLoadAndOrdinalAuditE2E",      TestPeDelayLoadAndOrdinalAuditE2E},
    {"RuntimeDelayLoadAndOrdinalHookE2E",  TestRuntimeDelayLoadAndOrdinalHookE2E},
    {"D3D12Xs3DPerspectiveDepthRenderE2E", TestD3D12Xs3DPerspectiveDepthRenderE2E},
    {"D3D12XsComVtableAbiConformanceE2E",  TestD3D12XsComVtableAbiConformanceE2E},
    {"D3D12XsPresentXSwapChainE2E",        TestD3D12XsPresentXSwapChainE2E},
    {"D3D12XsFramePacingTokensE2E",        TestD3D12XsFramePacingTokensE2E},
    {"D3D12XsHeadlessPresentXModeE2E",     TestD3D12XsHeadlessPresentXModeE2E},
    {"D3D12XsAsyncComputeCrossQueueSyncE2E", TestD3D12XsAsyncComputeCrossQueueSyncE2E},
    {"XMemMin16ByteAlignment",             TestXMemMin16ByteAlignment},
    {"Pe64ImplicitTlsAndGuestAtExitE2E",   TestPe64ImplicitTlsAndGuestAtExitE2E},
    {"AutoStubThunkSurvivalE2E",           TestAutoStubThunkSurvivalE2E},
    {"LoaderSymbolCoverageAuditE2E",       TestLoaderSymbolCoverageAuditE2E},
    {"DxbcDigestAndXboxChunkStripperE2E",  TestDxbcDigestAndXboxChunkStripperE2E},
    {"D3D12XsEnhancedBarriersE2E",         TestD3D12XsEnhancedBarriersE2E},
    {"D3D12XsFeatureCapsQueryE2E",         TestD3D12XsFeatureCapsQueryE2E},
    {"VfsPathEscapeAndLongPaths",          TestVfsPathEscapeAndLongPaths},
    {"XvdFlagsAndBoundsNegative",          TestXvdFlagsAndBoundsNegative},
    {"NonAsciiShimAndFreeLibraryHook",     TestNonAsciiShimAndFreeLibraryHook},
    {"WarpAdapterRenderReadback",          TestWarpAdapterRenderReadback},
    {"LocalCorpusReadOnlyAudit",           TestLocalCorpusReadOnlyAudit},
    {"D3D12XsGdkSampleAlignmentE2E",       TestD3D12XsGdkSampleAlignmentE2E},
    {"XGameRuntimeComBrokerE2E",           TestXGameRuntimeComBrokerE2E},
    {"ScarlettStudioSelfTest",             TestScarlettStudioSelfTest},
};

bool EqualsCaseInsensitive(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool ContainsCaseInsensitive(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
    return it != haystack.end();
}

} // namespace

int main(int argc, char** argv) {
    bool listOnly = false;
    bool exactMatch = false;
    std::string_view filter;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--list") {
            listOnly = true;
        } else if (arg == "--exact") {
            exactMatch = true;
        } else if (arg == "--filter" && i + 1 < argc) {
            filter = argv[++i];
        } else if (arg.rfind("--filter=", 0) == 0) {
            filter = arg.substr(9);
        }
    }

    auto matches = [&](const char* name) -> bool {
        if (filter.empty()) {
            return true;
        }
        return exactMatch ? EqualsCaseInsensitive(name, filter)
                          : ContainsCaseInsensitive(name, filter);
    };

    if (listOnly) {
        for (const auto& t : kTests) {
            if (matches(t.name)) {
                std::printf("%s\n", t.name);
            }
        }
        return 0;
    }

    std::printf("=== Scarlett Layer Automated Shim Verification Suite ===\n");

    int passed = 0;
    int failed = 0;
    for (const auto& t : kTests) {
        if (!matches(t.name)) {
            continue;
        }
        std::printf("[TEST] %-34s ... ", t.name);
        std::fflush(stdout);
        if (t.fn()) {
            ++passed;
            std::printf("PASS\n");
        } else {
            ++failed;
            std::printf("[TEST] %-34s ... FAIL\n", t.name);
        }
    }

    XGameRuntimeUninitialize();

    if (exactMatch && !filter.empty() && (passed + failed) == 0) {
        std::printf("ERROR: no test matched --exact --filter '%.*s'\n",
                    static_cast<int>(filter.size()), filter.data());
        return 1;
    }

    std::printf("=== Results: %d passed, %d failed ===\n", passed, failed);
    return (failed == 0) ? 0 : 1;
}