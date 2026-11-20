#pragma once

#include <cstdint>
#include <windows.h>

using PIXBeginEventFn = void(WINAPI*)(uint64_t color, const char* name);
using PIXEndEventFn   = void(WINAPI*)();
using PIXSetMarkerFn  = void(WINAPI*)(uint64_t color, const char* name);

extern "C" {
    void WINAPI PIXBeginEvent(uint64_t color, const char* name);
    void WINAPI PIXEndEvent();
    void WINAPI PIXSetMarker(uint64_t color, const char* name);
}