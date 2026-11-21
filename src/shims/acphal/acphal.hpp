#pragma once

#include <cstdint>
#include <windows.h>

struct ACP_SHAPE_CONTEXT_COUNTS {
    uint32_t numSrcContexts;
    uint32_t numEqContexts;
    uint32_t numFiltContexts;
    uint32_t numDmaContexts;
    uint32_t numPcmContexts;
    uint32_t numXmaContexts;
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