cmake_minimum_required(VERSION 3.25)
project(scarlett_layer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SC_BIN_DIR "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${SC_BIN_DIR}")
foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${cfg}" CFG_UPPER)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CFG_UPPER} "${SC_BIN_DIR}")
endforeach()

if(MSVC)
    add_compile_options(/W4 /permissive-)
endif()

add_library(sc_common INTERFACE)
target_include_directories(sc_common INTERFACE "${CMAKE_SOURCE_DIR}/src")

# Shim #1: xgameruntime.dll
add_library(xgameruntime SHARED
    src/shims/xgameruntime/xgameruntime.cpp
    src/shims/xgameruntime/task_queue.cpp
    src/shims/xgameruntime/xgameruntime.def
)
target_link_libraries(xgameruntime PRIVATE sc_common)

# Shim #2: d3d12_xs.dll (Scarlett D3D12.x -> Desktop D3D12/DXGI)
add_library(d3d12_xs SHARED
    src/shims/d3d12_xs/d3d12_xs.cpp
    src/shims/d3d12_xs/d3d12_xs.def
)
target_link_libraries(d3d12_xs PRIVATE sc_common d3d12 dxgi user32)

# Synthetic Series X title (links ONLY to our shims, never desktop d3d12.lib)
add_executable(dummy_title
    src/tests/dummy_title/main.cpp
)
target_link_libraries(dummy_title PRIVATE sc_common xgameruntime d3d12_xs)

# Host loader
add_executable(loader
    src/loader/main.cpp
)
target_link_libraries(loader PRIVATE sc_common)