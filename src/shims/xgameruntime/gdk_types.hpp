#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstddef>
#include <cstdint>
#include <windows.h>

enum class XSystemDeviceType : uint32_t {
    Unknown              = 0x00,
    Pc                   = 0x01,
    XboxOne              = 0x02,
    XboxOneS             = 0x03,
    XboxOneX             = 0x04,
    XboxOneXDevkit       = 0x05,
    XboxScarlettLockhart = 0x06,
    XboxScarlettAnaconda = 0x07,
    XboxScarlettDevkit   = 0x08
};

struct XTaskQueueObject;
using XTaskQueueHandle = XTaskQueueObject*;

enum class XTaskQueueDispatchMode : uint32_t {
    Manual               = 0,
    ThreadPool           = 1,
    SerializedThreadPool = 2,
    Immediate            = 3
};

enum class XTaskQueuePort : uint32_t {
    Work       = 0,
    Completion = 1
};

struct XAsyncBlock;
using XAsyncCompletionRoutine = void(CALLBACK*)(XAsyncBlock* asyncBlock);

struct XAsyncBlock {
    XTaskQueueHandle        queue;
    void*                   context;
    XAsyncCompletionRoutine callback;
    unsigned char           internal[sizeof(void*) * 4];
};
static_assert(sizeof(XAsyncBlock) == sizeof(void*) * 7, "XAsyncBlock x64 ABI mismatch");

struct XUser;
using XUserHandle = XUser*;

enum class XUserAddOptions : uint32_t {
    None                     = 0x00,
    AddDefaultUserSilently   = 0x01,
    AllowGuests              = 0x02,
    AddDefaultUserAllowingUI = 0x04
};

enum class XUserGamertagComponent : uint32_t {
    Classic      = 0,
    Modern       = 1,
    ModernSuffix = 2,
    UniqueModern = 3
};