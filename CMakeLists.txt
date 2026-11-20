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
    src/shims/xgameruntime/xgameruntime.def
)
target_link_libraries(xgameruntime PRIVATE sc_common)
sc_set_runtime_dir(xgameruntime "${SC_BIN_DIR}/shims")

# Shim #2: d3d12_xs.dll -> build/bin/shims/
add_library(d3d12_xs SHARED
    src/shims/d3d12_xs/d3d12_xs.cpp
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
)
target_link_libraries(scarlett_tests PRIVATE sc_common xgameruntime d3d12_xs gameinput xg_xs dstorage_xs xaudio2_9 xmem kernelx d3dcompiler)
add_dependencies(scarlett_tests pixevt engine_plugin)
sc_set_runtime_dir(scarlett_tests "${SC_BIN_DIR}/titles/scarlett_tests")

# Host loader -> build/bin/
add_executable(loader
    src/loader/main.cpp
    src/loader/pe_patcher.cpp
)
target_link_libraries(loader PRIVATE sc_common)
add_dependencies(loader scarlett_hook)
sc_set_runtime_dir(loader "${SC_BIN_DIR}")

# Register headless verification suite via loader.exe in CTest
add_test(
    NAME scarlett_shim_suite
    COMMAND $<TARGET_FILE:loader> $<TARGET_FILE:scarlett_tests>
)