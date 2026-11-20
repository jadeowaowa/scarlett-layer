#pragma once

#include <cstddef>
#include <cstdint>
#include <windows.h>

enum XMemAlignment : uint32_t {
    XALLOC_ALIGNMENT_DEFAULT = 0,  // 16 bytes
    XALLOC_ALIGNMENT_4       = 1,
    XALLOC_ALIGNMENT_8       = 2,
    XALLOC_ALIGNMENT_16      = 3,
    XALLOC_ALIGNMENT_32      = 4,
    XALLOC_ALIGNMENT_64      = 5,
    XALLOC_ALIGNMENT_128     = 6,
    XALLOC_ALIGNMENT_256     = 7,
    XALLOC_ALIGNMENT_512     = 8,
    XALLOC_ALIGNMENT_1K      = 9,
    XALLOC_ALIGNMENT_2K      = 10,
    XALLOC_ALIGNMENT_4K      = 11,
    XALLOC_ALIGNMENT_8K      = 12,
    XALLOC_ALIGNMENT_16K     = 13,
    XALLOC_ALIGNMENT_32K     = 14,
    XALLOC_ALIGNMENT_64K     = 15
};

enum XMemType : uint32_t {
    XALLOC_MEMTYPE_HEAP     = 0,
    XALLOC_MEMTYPE_PHYSICAL = 1
};

enum XMemAllocatorId : uint8_t {
    XALLOC_ID_DEFAULT        = 0,
    XALLOC_ID_TEXTURE_TILED  = 10,
    XALLOC_ID_TEXTURE_UPLOAD = 11,
    XALLOC_ID_AUDIO_PCM      = 20
};

// Clean-room packed attribute word:
//   [0..7]   AllocatorId (8 bits)
//   [8..11]  Alignment   (4 bits, XMemAlignment)
//   [12..13] MemType     (2 bits, XMemType)
//   [14]     ZeroInit    (1 bit)
constexpr uint32_t MakeXAllocAttributes(
    uint8_t allocatorId,
    XMemAlignment alignment = XALLOC_ALIGNMENT_DEFAULT,
    XMemType memType = XALLOC_MEMTYPE_HEAP,
    bool zeroInit = true) {
    return (static_cast<uint32_t>(allocatorId) & 0xFFu)
         | ((static_cast<uint32_t>(alignment) & 0xFu) << 8)
         | ((static_cast<uint32_t>(memType) & 0x3u) << 12)
         | (zeroInit ? (1u << 14) : 0u);
}

extern "C" {
    void*  WINAPI XMemAlloc(size_t dwSize, uint32_t dwAllocAttributes);
    void   WINAPI XMemFree(void* pAddress, uint32_t dwAllocAttributes);
    size_t WINAPI XMemSize(void* pAddress, uint32_t dwAllocAttributes);

    void*  WINAPI XMemAllocDefault(size_t dwSize, uint32_t dwAllocAttributes);
    void   WINAPI XMemFreeDefault(void* pAddress, uint32_t dwAllocAttributes);
    size_t WINAPI XMemSizeDefault(void* pAddress, uint32_t dwAllocAttributes);
}