cmake_minimum_required(VERSION 3.25)
project(scarlett_layer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Put all built .exe and .dll files in build/bin so app-local DLL loading
# overrides C:\Windows\System32\xgameruntime.dll cleanly during Sprint 1.
set(SC_BIN_DIR "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${SC_BIN_DIR}")
foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${cfg}" CFG_UPPER)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CFG_UPPER} "${SC_BIN_DIR}")
endforeach()

if(MSVC)
    add_compile_options(/W4 /permissive-)
endif()

# Shared header-only utilities
add_library(sc_common INTERFACE)
target_include_directories(sc_common INTERFACE "${CMAKE_SOURCE_DIR}/src")

# Shim #1: xgameruntime.dll
add_library(xgameruntime SHARED
    src/shims/xgameruntime/xgameruntime.cpp
    src/shims/xgameruntime/xgameruntime.def
)
target_link_libraries(xgameruntime PRIVATE sc_common)

# Synthetic Series X title (our test harness)
add_executable(dummy_title
    src/tests/dummy_title/main.cpp
)
target_link_libraries(dummy_title PRIVATE xgameruntime)

# Host loader
add_executable(loader
    src/loader/main.cpp
)
target_link_libraries(loader PRIVATE sc_common)