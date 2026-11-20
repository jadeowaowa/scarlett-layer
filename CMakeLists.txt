cmake_minimum_required(VERSION 3.25)
project(scarlett_layer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

# Shim #1: xgameruntime.dll -> build/bin/shims/
add_library(xgameruntime SHARED
    src/shims/xgameruntime/xgameruntime.cpp
    src/shims/xgameruntime/task_queue.cpp
    src/shims/xgameruntime/xgameruntime.def
)
target_link_libraries(xgameruntime PRIVATE sc_common)
sc_set_runtime_dir(xgameruntime "${SC_BIN_DIR}/shims")

# Shim #2: d3d12_xs.dll -> build/bin/shims/
add_library(d3d12_xs SHARED
    src/shims/d3d12_xs/d3d12_xs.cpp
    src/shims/d3d12_xs/d3d12_xs.def
)
target_link_libraries(d3d12_xs PRIVATE sc_common d3d12 dxgi user32)
sc_set_runtime_dir(d3d12_xs "${SC_BIN_DIR}/shims")

# Isolated synthetic title -> build/bin/titles/dummy_title/ (ZERO shims beside it!)
add_executable(dummy_title
    src/tests/dummy_title/main.cpp
)
target_link_libraries(dummy_title PRIVATE sc_common xgameruntime d3d12_xs)
sc_set_runtime_dir(dummy_title "${SC_BIN_DIR}/titles/dummy_title")

# Host loader -> build/bin/
add_executable(loader
    src/loader/main.cpp
    src/loader/pe_patcher.cpp
)
target_link_libraries(loader PRIVATE sc_common)
sc_set_runtime_dir(loader "${SC_BIN_DIR}") 
target_compile_definitions(sc_common INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN) 
