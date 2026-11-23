#include "acphal.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <mutex>
#include <vector>

namespace {

struct AcpHalImpl {
    const IAcpHalVtbl* lpVtbl;
    std::atomic<bool>  connected{false};
    std::atomic<bool>  disconnectPending{false};
};

HRESULT WINAPI AcpConnect(void* self, uint32_t /*cmdQueueLen*/, uint32_t /*msgQueueLen*/) {
    if (!self) return E_INVALIDARG;
    auto* obj = static_cast<AcpHalImpl*>(self);
    obj->connected.store(true, std::memory_order_release);
    obj->disconnectPending.store(false, std::memory_order_release);
    return S_OK;
}

HRESULT WINAPI AcpDisconnect(void* self) {
    if (!self) return E_INVALIDARG;
    auto* obj = static_cast<AcpHalImpl*>(self);
    obj->connected.store(false, std::memory_order_release);
    obj->disconnectPending.store(true, std::memory_order_release);
    return S_OK;
}

HRESULT WINAPI AcpSubmitCommand(
    void* self, uint32_t /*type*/, uint32_t /*id*/, uint32_t /*flags*/,
    const void* /*data*/, uint32_t /*extra*/) {
    if (!self) return E_INVALIDARG;
    return S_OK;
}

uint8_t WINAPI AcpPopMessage(void* self, void* msg) {
    if (!self || !msg) return 0;
    auto* obj = static_cast<AcpHalImpl*>(self);
    if (obj->disconnectPending.exchange(false, std::memory_order_acq_rel)) {
        std::memset(msg, 0, 16);
        static_cast<uint8_t*>(msg)[0] = 0x80u;
        return 1;
    }
    return 0;
}

HRESULT WINAPI AcpReservedSlot4(void* self) {
    return self ? S_OK : E_INVALIDARG;
}

const IAcpHalVtbl kAcpHalVtbl = {
    &AcpConnect,
    &AcpDisconnect,
    &AcpSubmitCommand,
    &AcpPopMessage,
    &AcpReservedSlot4
};

} // namespace

extern "C" {

HRESULT WINAPI AcpHalCreate(void** acpHal) {
    if (!acpHal) return E_INVALIDARG;
    auto* obj = new AcpHalImpl();
    obj->lpVtbl = &kAcpHalVtbl;
    *acpHal = obj;
    return S_OK;
}

} // extern "C"

namespace {
std::mutex g_shapeMtx;
std::vector<void*> g_shapeBuffers;

void* AllocShapeBuf(uint32_t count, size_t stride) {
    if (count == 0) return nullptr;
    const size_t bytes = static_cast<size_t>(count) * stride;
    void* p = _aligned_malloc(bytes, 64);
    if (p) {
        std::memset(p, 0, bytes);
        g_shapeBuffers.push_back(p);
    }
    return p;
}
} // namespace

extern "C" {

HRESULT WINAPI AcpHalAllocateShapeContexts(ACP_SHAPE_CONTEXT_COUNTS* ctx) {
    if (!ctx) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_shapeMtx);
    ctx->srcContextBuffer  = AllocShapeBuf(ctx->numSrcContexts,  64);
    ctx->eqContextBuffer   = AllocShapeBuf(ctx->numEqContexts,   64);
    ctx->filtContextBuffer = AllocShapeBuf(ctx->numFiltContexts, 64);
    ctx->dmaContextBuffer  = AllocShapeBuf(ctx->numDmaContexts,  64);
    ctx->xmaContextBuffer  = AllocShapeBuf(ctx->numXmaContexts,  64);
    ctx->pcmContextBuffer  = AllocShapeBuf(ctx->numPcmContexts,  64);
    return S_OK;
}

HRESULT WINAPI AcpHalReleaseShapeContexts(void) {
    std::lock_guard<std::mutex> lock(g_shapeMtx);
    for (void* p : g_shapeBuffers) {
        _aligned_free(p);
    }
    g_shapeBuffers.clear();
    return S_OK;
}

HRESULT WINAPI ApuAlloc(
    void** virtualAddress, void* physicalAddress,
    uint32_t sizeInBytes, uint32_t alignmentInBytes) {
    if (!virtualAddress || sizeInBytes == 0) return E_INVALIDARG;
    uint32_t align = (alignmentInBytes < 16u) ? 16u : alignmentInBytes;
    if ((align & (align - 1u)) != 0u) return E_INVALIDARG;

    void* ptr = _aligned_malloc(sizeInBytes, align);
    if (!ptr) return E_OUTOFMEMORY;
    std::memset(ptr, 0, sizeInBytes);
    *virtualAddress = ptr;
    if (physicalAddress) {
        *static_cast<uint64_t*>(physicalAddress) = reinterpret_cast<uint64_t>(ptr);
    }
    return S_OK;
}

HRESULT WINAPI ApuFree(void* virtualAddress) {
    if (!virtualAddress) return E_INVALIDARG;
    _aligned_free(virtualAddress);
    return S_OK;
}


HRESULT WINAPI ApuMapVirtualAddress(void* /*virtualAddress*/, uint32_t /*sizeInBytes*/) {
    return S_OK;
}

BOOL WINAPI ApuIsVirtualAddressValid(const void* virtualAddress) {
    return virtualAddress != nullptr ? TRUE : FALSE;
}

} // extern "C"