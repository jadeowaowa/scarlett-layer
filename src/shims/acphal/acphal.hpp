#pragma once

#include <cstdint>
#include <windows.h>

struct ACP_SHAPE_CONTEXT_COUNTS {
    uint32_t numSrcContexts;    // +0x00 (32 B/entry)
    uint32_t numEqContexts;     // +0x04
    uint32_t numFiltContexts;   // +0x08
    uint32_t numDmaContexts;    // +0x0C (16 B/entry)
    uint32_t numXmaContexts;    // +0x10 (64 B/entry)
    uint32_t numPcmContexts;    // +0x14
    void*    srcContextBuffer;  // +0x18
    void*    eqContextBuffer;   // +0x20
    void*    filtContextBuffer; // +0x28
    void*    dmaContextBuffer;  // +0x30
    void*    xmaContextBuffer;  // +0x38
    void*    pcmContextBuffer;  // +0x40
};

struct IAcpHalVtbl {
    HRESULT (WINAPI* Connect)(void* self, uint32_t cmdQueueLen, uint32_t msgQueueLen);
    HRESULT (WINAPI* Disconnect)(void* self);
    HRESULT (WINAPI* SubmitCommand)(void* self, uint32_t type, uint32_t id, uint32_t flags, const void* data, uint32_t extra);
    uint8_t (WINAPI* PopMessage)(void* self, void* msg);
    HRESULT (WINAPI* ReservedSlot4)(void* self);
};

struct IAcpHal {
    const IAcpHalVtbl* lpVtbl;
};

extern "C" {
    HRESULT WINAPI AcpHalCreate(void** acpHal);
    HRESULT WINAPI AcpHalAllocateShapeContexts(ACP_SHAPE_CONTEXT_COUNTS* ctx);
    HRESULT WINAPI AcpHalReleaseShapeContexts(void);
    HRESULT WINAPI ApuAlloc(void** virtualAddress, void* physicalAddress, uint32_t sizeInBytes, uint32_t alignmentInBytes);
    HRESULT WINAPI ApuFree(void* virtualAddress);
}