cmake_minimum_required(VERSION 3.25)
project(scarlett_layer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

set(SC_BIN_DIR "${CMAKE_BINARY_DIR}/bin")
set(SC_LIB_DIR "${CMAKE_BINARY_DIR}/lib")

function(sc_set_runtime_dir target dir)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${dir}"
        ARCHIVE_OUTPUT_DIRECTORY "${SC_LIB_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${dir}"
    )
    foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER "${cfg}" CFG_UPPER)
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${CFG_UPPER} "${dir}"
            ARCHIVE_OUTPUT_DIRECTORY_${CFG_UPPER} "${SC_LIB_DIR}"
            LIBRARY_OUTPUT_DIRECTORY_${CFG_UPPER} "${dir}"
        )
    endforeach()
endfunction()

if(MSVC)
    add_compile_options(/W4 /permissive-)
endif()

add_library(sc_common INTERFACE)
target_include_directories(sc_common INTERFACE "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(sc_common INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

# Shim #1: xgameruntime.dll -> build/bin/shims/
add_library(xgameruntime SHARED
    src/shims/xgameruntime/xgameruntime.cpp
    src/shims/xgameruntime/task_queue.cpp
    src/shims/xgameruntime/game_save.cpp
    src/shims/xgameruntime/api_broker.cpp
    src/shims/xgameruntime/xgameruntime.def
)
target_link_libraries(xgameruntime PRIVATE sc_common)
sc_set_runtime_dir(xgameruntime "${SC_BIN_DIR}/shims")

# Shim #2: d3d12_xs.dll -> build/bin/shims/
add_library(d3d12_xs SHARED
    src/shims/d3d12_xs/d3d12_xs.cpp
    src/shims/d3d12_xs/d3d12_xs_utils.cpp
    src/shims/d3d12_xs/command_queue_x.cpp
    src/shims/d3d12_xs/command_list_x.cpp
    src/shims/d3d12_xs/device_x.cpp
    src/shims/d3d12_xs/d3d12_xs.def
)
target_link_libraries(d3d12_xs PRIVATE sc_common d3d12 dxgi user32 d3dcompiler)
sc_set_runtime_dir(d3d12_xs "${SC_BIN_DIR}/shims")

# Shim #3: gameinput.dll -> build/bin/shims/
add_library(gameinput SHARED
    src/shims/gameinput/gameinput.cpp
    src/shims/gameinput/gameinput.def
)
target_link_libraries(gameinput PRIVATE sc_common user32 xinput)
sc_set_runtime_dir(gameinput "${SC_BIN_DIR}/shims")

# Shim #4: xg_xs.dll -> build/bin/shims/
add_library(xg_xs SHARED
    src/shims/xg_xs/xg_xs.cpp
    src/shims/xg_xs/xg_xs.def
)
target_link_libraries(xg_xs PRIVATE sc_common)
sc_set_runtime_dir(xg_xs "${SC_BIN_DIR}/shims")

# Shim #5: dstorage_xs.dll -> build/bin/shims/
add_library(dstorage_xs SHARED
    src/shims/dstorage_xs/dstorage_xs.cpp
    src/shims/dstorage_xs/dstorage_xs.def
)
target_link_libraries(dstorage_xs PRIVATE sc_common)
sc_set_runtime_dir(dstorage_xs "${SC_BIN_DIR}/shims")

# Shim #6: pixevt.dll -> build/bin/shims/
add_library(pixevt SHARED
    src/shims/pixevt/pixevt.cpp
    src/shims/pixevt/pixevt.def
)
target_link_libraries(pixevt PRIVATE sc_common)
sc_set_runtime_dir(pixevt "${SC_BIN_DIR}/shims")

# Shim #7: xaudio2_9.dll -> build/bin/shims/
add_library(xaudio2_9 SHARED
    src/shims/xaudio2_9/xaudio2_9.cpp
    src/shims/xaudio2_9/xaudio2_9.def
)
target_link_libraries(xaudio2_9 PRIVATE sc_common ole32)
sc_set_runtime_dir(xaudio2_9 "${SC_BIN_DIR}/shims")

# Shim #8: xmem.dll -> build/bin/shims/
add_library(xmem SHARED
    src/shims/xmem/xmem.cpp
    src/shims/xmem/xmem.def
)
target_link_libraries(xmem PRIVATE sc_common)
sc_set_runtime_dir(xmem "${SC_BIN_DIR}/shims")

# Shim #9: kernelx.dll -> build/bin/shims/ (auto-discovered by loader.exe!)
add_library(kernelx SHARED
    src/shims/kernelx/kernelx.cpp
    src/shims/kernelx/kernelx.def
)
target_link_libraries(kernelx PRIVATE sc_common)
sc_set_runtime_dir(kernelx "${SC_BIN_DIR}/shims")

# Shim #10: AcpHal.dll -> build/bin/shims/
add_library(acphal SHARED
    src/shims/acphal/acphal.cpp
    src/shims/acphal/acphal.def
)
set_target_properties(acphal PROPERTIES OUTPUT_NAME "AcpHal")
target_link_libraries(acphal PRIVATE sc_common)
sc_set_runtime_dir(acphal "${SC_BIN_DIR}/shims")

# Shim #11: libHttpClient.GDK.dll -> build/bin/shims/
add_library(libhttpclient SHARED
    src/shims/libhttpclient/libhttpclient.cpp
    src/shims/libhttpclient/libhttpclient.def
)
set_target_properties(libhttpclient PROPERTIES OUTPUT_NAME "libHttpClient.GDK")
target_link_libraries(libhttpclient PRIVATE sc_common)
sc_set_runtime_dir(libhttpclient "${SC_BIN_DIR}/shims")
# Also stage desktop GDK name (libHttpClient.dll) alongside console name (libHttpClient.GDK.dll)
add_custom_command(TARGET libhttpclient POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:libhttpclient>"
        "${SC_BIN_DIR}/shims/libHttpClient.dll"
    COMMENT "Staging shims/libHttpClient.dll alias for GDK binaries"
)

# Shim #12: XCurl.dll -> build/bin/shims/
add_library(xcurl SHARED
    src/shims/xcurl/xcurl.cpp
    src/shims/xcurl/xcurl.def
)
set_target_properties(xcurl PROPERTIES OUTPUT_NAME "XCurl")
target_link_libraries(xcurl PRIVATE sc_common)
sc_set_runtime_dir(xcurl "${SC_BIN_DIR}/shims")

# Shim #13: XFrontPanelDisplay.dll -> build/bin/shims/
add_library(xfrontpaneldisplay SHARED
    src/shims/xfrontpaneldisplay/xfrontpaneldisplay.cpp
    src/shims/xfrontpaneldisplay/xfrontpaneldisplay.def
)
set_target_properties(xfrontpaneldisplay PROPERTIES OUTPUT_NAME "XFrontPanelDisplay")
target_link_libraries(xfrontpaneldisplay PRIVATE sc_common)
sc_set_runtime_dir(xfrontpaneldisplay "${SC_BIN_DIR}/shims")

# Runtime multi-module LoadLibrary IAT hook DLL -> build/bin/
add_library(scarlett_hook SHARED
    src/loader/scarlett_hook.cpp
)
target_link_libraries(scarlett_hook PRIVATE sc_common)
sc_set_runtime_dir(scarlett_hook "${SC_BIN_DIR}")

# Synthetic middleware plugin -> build/bin/titles/dummy_title/
add_library(engine_plugin SHARED
    src/tests/engine_plugin/engine_plugin.cpp
    src/tests/engine_plugin/engine_plugin.def
)
target_link_libraries(engine_plugin PRIVATE sc_common)
sc_set_runtime_dir(engine_plugin "${SC_BIN_DIR}/titles/dummy_title")

# Isolated synthetic title -> build/bin/titles/dummy_title/
add_executable(dummy_title
    src/tests/dummy_title/main.cpp
)
target_link_libraries(dummy_title PRIVATE sc_common xgameruntime d3d12_xs gameinput xg_xs dstorage_xs xaudio2_9 xmem kernelx d3dcompiler)
add_dependencies(dummy_title pixevt engine_plugin)
sc_set_runtime_dir(dummy_title "${SC_BIN_DIR}/titles/dummy_title")

# Headless automated verification suite -> build/bin/titles/scarlett_tests/
add_executable(scarlett_tests
    src/tests/scarlett_tests/main.cpp
    src/tests/scarlett_tests/tests_d3d12_core.cpp
    src/tests/scarlett_tests/tests_d3d12_shaders_present.cpp
    src/tests/scarlett_tests/tests_storage_swizzle.cpp
    src/tests/scarlett_tests/tests_runtime_io.cpp
    src/tests/scarlett_tests/tests_loader_e2e.cpp
    src/tests/scarlett_tests/tests_rdna2_oracle.cpp
)
target_link_libraries(scarlett_tests PRIVATE sc_common xgameruntime d3d12_xs gameinput xg_xs dstorage_xs xaudio2_9 xmem kernelx acphal libhttpclient xcurl d3dcompiler)
add_dependencies(scarlett_tests pixevt engine_plugin scarlett_studio)
sc_set_runtime_dir(scarlett_tests "${SC_BIN_DIR}/titles/scarlett_tests")

# Live interactive 3D cube demo -> build/bin/titles/scarlett_3d_demo/
add_executable(scarlett_3d_demo WIN32
    src/tests/scarlett_3d_demo/main.cpp
)
target_link_libraries(scarlett_3d_demo PRIVATE sc_common d3d12_xs d3dcompiler user32 gdi32)
sc_set_runtime_dir(scarlett_3d_demo "${SC_BIN_DIR}/titles/scarlett_3d_demo")

# Host loader -> build/bin/
add_executable(loader
    src/loader/main.cpp
    src/loader/pe_patcher.cpp
)
target_link_libraries(loader PRIVATE sc_common)
add_dependencies(loader scarlett_hook)

# Desktop GUI Launcher & Test Hub -> build/bin/scarlett_studio.exe
add_executable(scarlett_studio WIN32
    src/gui/scarlett_studio.cpp
)
target_compile_definitions(scarlett_studio PRIVATE UNICODE _UNICODE)
target_link_libraries(scarlett_studio PRIVATE sc_common user32 gdi32 comdlg32 shell32 ole32 d3d11 dxgi d2d1 dwrite dcomp dwmapi)
add_dependencies(scarlett_studio loader)
sc_set_runtime_dir(scarlett_studio "${SC_BIN_DIR}")
sc_set_runtime_dir(loader "${SC_BIN_DIR}")

# Offline RDNA2 corpus sweep tool -> build/bin/rdna2_sweep.exe
add_executable(rdna2_sweep
    src/tools/rdna2_sweep/main.cpp
)
target_link_libraries(rdna2_sweep PRIVATE sc_common d3dcompiler)
sc_set_runtime_dir(rdna2_sweep "${SC_BIN_DIR}")

# Register each test from scarlett_tests/main.cpp individually in CTest
set(_sc_tests_main "${CMAKE_SOURCE_DIR}/src/tests/scarlett_tests/main.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sc_tests_main}")
file(STRINGS "${_sc_tests_main}" _sc_test_lines REGEX "^[ \t]*\\{\"([A-Za-z0-9_]+)\",")

set(_sc_gpu_tests
    D3D12HeadlessDeviceAndPso
    D3D12XsGdkSampleAlignmentE2E
    D3D12PlacedResourcesAndHeap
    ScxsTranslatorAndPsoCache
    D3D12ComputePsoAndUavExecution
    Rdna2IsaDecoderAndGpuExecution
    D3D12AsyncComputeCrossQueueSync
    Rdna2ComputeIsaExecution
    Rdna2VertexAndPixelPipelineWithCbv
    XgXs64KbsAndBcnBlockSwizzle
    DStorageGpuBufferStreaming
    DStorageGpuTextureStreaming
    DStorageDeviceXBridgeAndMipChain
    Rdna2ImageSampleShader
    PresentXDescriptorAudit
    Rdna2TexturedDrawE2E
    DStorageGpuBufferAndFenceE2E
    Rdna2ComputeDispatchE2E
    D3D12XsCommandListWrapperE2E
    D3D12XsCmdListRenderClearE2E
    D3D12XsCmdListComputeDispatchE2E
    ScarlettTitleBringUpE2E
    ScarlettFullStack50SprintCapstoneE2E
    D3D12Xs3DPerspectiveDepthRenderE2E
    D3D12XsComVtableAbiConformanceE2E
    D3D12XsPresentXSwapChainE2E
    D3D12XsFramePacingTokensE2E
    D3D12XsHeadlessPresentXModeE2E
    D3D12XsAsyncComputeCrossQueueSyncE2E
    DxbcDigestAndXboxChunkStripperE2E
    D3D12XsEnhancedBarriersE2E
    D3D12XsFeatureCapsQueryE2E
    Rdna2OracleAluAndCbvDiffE2E
    Rdna2OracleFp16LdsAndCfDiffE2E
    Rdna2OracleControlFlowAndRealRgaCsDiffE2E
    Rdna2OracleGraphicsPipelineDiffE2E
)

set(_sc_registered_count 0)
foreach(_line IN LISTS _sc_test_lines)
    if(_line MATCHES "^[ \t]*\\{\"([A-Za-z0-9_]+)\",")
        set(_tname "${CMAKE_MATCH_1}")
        add_test(
            NAME "${_tname}"
            COMMAND $<TARGET_FILE:loader> $<TARGET_FILE:scarlett_tests> --exact --filter "${_tname}"
        )
        list(FIND _sc_gpu_tests "${_tname}" _gpu_idx)
        if(_gpu_idx GREATER_EQUAL 0)
            set(_tlabel "gpu")
        else()
            set(_tlabel "cpu")
        endif()
        set_tests_properties("${_tname}" PROPERTIES
            ENVIRONMENT "SCARLETT_HEADLESS=1"
            LABELS "${_tlabel}"
        )
        math(EXPR _sc_registered_count "${_sc_registered_count} + 1")
    endif()
endforeach()

if(_sc_registered_count EQUAL 0)
    message(FATAL_ERROR "Failed to discover any tests from ${_sc_tests_main}")
endif()
message(STATUS "Registered ${_sc_registered_count} individual Scarlett CTest target(s)")