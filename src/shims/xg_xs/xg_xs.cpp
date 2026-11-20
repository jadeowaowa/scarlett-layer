#include "xg_xs.hpp"
#include "xg_64kbs.hpp"
#include "common/stub.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace {

uint64_t AlignUp(uint64_t value, uint64_t alignment) {
    return (value + (alignment - 1ULL)) & ~(alignment - 1ULL);
}

uint32_t BytesPerPixelForFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_A8_UNORM:
            return 1;
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_B5G6R5_UNORM:
            return 2;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R32_FLOAT:
            return 4;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
            return 8;
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
            return 16;
        default:
            return 0;
    }
}

const char* TileModeName(XG_TILE_MODE mode) {
    switch (mode) {
        case XG_TILE_MODE_LINEAR:  return "LINEAR";
        case XG_TILE_MODE_2D_THIN: return "2D_THIN(8x8-Morton)";
        case XG_TILE_MODE_64KB_S:  return "64KB_S(8x8-Morton)";
        default:                   return "UNKNOWN";
    }
}

// Interleaves 3-bit coordinates (0..7) into a 6-bit Morton code (0..63) for 8x8 micro-tiles
uint32_t Morton8x8(uint32_t lx, uint32_t ly) {
    auto spread = [](uint32_t v) -> uint32_t {
        v &= 7u;
        return (v & 1u) | ((v & 2u) << 1) | ((v & 4u) << 2);
    };
    return spread(lx) | (spread(ly) << 1);
}

HRESULT ComputeLayoutInternal(const XG_TEXTURE2D_DESC& desc, XG_RESOURCE_LAYOUT& out) {
    if (desc.Width == 0 || desc.Height == 0) {
        return E_INVALIDARG;
    }
    const uint32_t bpp = BytesPerPixelForFormat(desc.Format);
    if (bpp == 0) {
        return E_INVALIDARG;
    }

    const uint32_t mips = std::clamp(desc.MipLevels == 0 ? 1u : desc.MipLevels, 1u, 15u);
    const uint32_t arraySize = std::max(desc.ArraySize, 1u);
    const bool isTiled = (desc.TileMode != XG_TILE_MODE_LINEAR);

    std::memset(&out, 0, sizeof(out));
    out.BaseAlignmentBytes = isTiled ? 65536ULL : 256ULL;
    out.Planes = 1;
    out.Plane[0].BaseOffsetBytes = 0;
    out.Plane[0].BytesPerElement = bpp;
    out.Plane[0].MipLevels = mips;

    uint64_t offset = 0;
    uint32_t mipW = desc.Width;
    uint32_t mipH = desc.Height;

    for (uint32_t m = 0; m < mips; ++m) {
        XG_MIPLEVEL_LAYOUT& ml = out.Plane[0].MipLayout[m];
        ml.TileMode = static_cast<uint32_t>(desc.TileMode);

        if (isTiled) {
            offset = AlignUp(offset, 256ULL);
            ml.PaddedWidthElements = static_cast<uint32_t>(AlignUp(mipW, 8ULL));
            ml.PaddedHeightElements = static_cast<uint32_t>(AlignUp(mipH, 8ULL));
            ml.PitchBytes = ml.PaddedWidthElements * bpp;
            ml.OffsetBytes = offset;
            ml.SizeBytes = static_cast<uint64_t>(ml.PaddedWidthElements) *
                           static_cast<uint64_t>(ml.PaddedHeightElements) * bpp;
        } else {
            offset = AlignUp(offset, 512ULL); // D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT
            ml.PaddedWidthElements = mipW;
            ml.PaddedHeightElements = mipH;
            ml.PitchBytes = static_cast<uint32_t>(AlignUp(static_cast<uint64_t>(mipW) * bpp, 256ULL));
            ml.OffsetBytes = offset;
            ml.SizeBytes = static_cast<uint64_t>(ml.PitchBytes) * mipH;
        }

        offset += ml.SizeBytes;
        mipW = std::max(1u, mipW >> 1);
        mipH = std::max(1u, mipH >> 1);
    }

    const uint64_t singleSliceBytes = AlignUp(offset, out.BaseAlignmentBytes);
    out.Plane[0].SizeBytes = singleSliceBytes * arraySize;
    out.SizeBytes = out.Plane[0].SizeBytes;
    return S_OK;
}

class ScarlettTexture2DComputer final : public IXGTextureAddressComputer {
public:
    ScarlettTexture2DComputer(const XG_TEXTURE2D_DESC& desc, const XG_RESOURCE_LAYOUT& layout)
        : m_desc(desc)
        , m_layout(layout) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) {
            return E_POINTER;
        }
        if (IsEqualGUID(riid, __uuidof(IUnknown)) || IsEqualGUID(riid, IID_IXGTextureAddressComputer)) {
            *ppvObject = static_cast<IXGTextureAddressComputer*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        ULONG count = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (count == 0) {
            delete this;
        }
        return count;
    }

    HRESULT STDMETHODCALLTYPE GetResourceLayout(XG_RESOURCE_LAYOUT* pLayout) override {
        if (!pLayout) {
            return E_POINTER;
        }
        *pLayout = m_layout;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE CopyIntoSubresource(
        void* pResourceBaseAddress,
        uint32_t Subresource,
        const void* pSourceData,
        uint32_t SourceRowPitch,
        uint32_t /*SourceSlicePitch*/) override {
        return TransferSubresource(pResourceBaseAddress, Subresource,
                                   const_cast<void*>(pSourceData), SourceRowPitch, true);
    }

    HRESULT STDMETHODCALLTYPE CopyFromSubresource(
        void* pDestinationData,
        uint32_t Subresource,
        const void* pResourceBaseAddress,
        uint32_t DestRowPitch,
        uint32_t /*DestSlicePitch*/) override {
        return TransferSubresource(const_cast<void*>(pResourceBaseAddress), Subresource,
                                   pDestinationData, DestRowPitch, false);
    }

private:
    HRESULT TransferSubresource(
        void* resourceBase,
        uint32_t subresource,
        void* linearData,
        uint32_t linearRowPitch,
        bool intoResource) {
        if (!resourceBase || !linearData) {
            return E_INVALIDARG;
        }

        const uint32_t mipLevels = m_layout.Plane[0].MipLevels;
        const uint32_t mip = subresource % mipLevels;
        const uint32_t arraySlice = subresource / mipLevels;
        const uint32_t arrayCount = std::max(m_desc.ArraySize, 1u);
        if (arraySlice >= arrayCount) {
            return E_INVALIDARG;
        }

        const uint32_t bpp = m_layout.Plane[0].BytesPerElement;
        const uint32_t mipW = std::max(1u, m_desc.Width >> mip);
        const uint32_t mipH = std::max(1u, m_desc.Height >> mip);
        const XG_MIPLEVEL_LAYOUT& ml = m_layout.Plane[0].MipLayout[mip];
        const uint64_t sliceStride = m_layout.SizeBytes / arrayCount;

        auto* resPtr = static_cast<uint8_t*>(resourceBase) + (sliceStride * arraySlice) + ml.OffsetBytes;
        auto* linPtr = static_cast<uint8_t*>(linearData);

        if (m_desc.TileMode == XG_TILE_MODE_LINEAR) {
            const size_t rowCopyBytes = static_cast<size_t>(mipW) * bpp;
            for (uint32_t y = 0; y < mipH; ++y) {
                uint8_t* rRow = resPtr + static_cast<size_t>(y) * ml.PitchBytes;
                uint8_t* lRow = linPtr + static_cast<size_t>(y) * linearRowPitch;
                if (intoResource) {
                    std::memcpy(rRow, lRow, rowCopyBytes);
                } else {
                    std::memcpy(lRow, rRow, rowCopyBytes);
                }
            }
            return S_OK;
        }

        const uint32_t tilesX = ml.PaddedWidthElements / 8u;
        for (uint32_t y = 0; y < mipH; ++y) {
            const uint32_t ty = y >> 3;
            const uint32_t ly = y & 7u;
            uint8_t* linRow = linPtr + static_cast<size_t>(y) * linearRowPitch;

            for (uint32_t x = 0; x < mipW; ++x) {
                const uint32_t tx = x >> 3;
                const uint32_t lx = x & 7u;
                const uint64_t tileIndex = static_cast<uint64_t>(ty) * tilesX + tx;
                const uint64_t pixelIndex = tileIndex * 64ULL + Morton8x8(lx, ly);
                uint8_t* tiledPixel = resPtr + pixelIndex * bpp;
                uint8_t* linearPixel = linRow + static_cast<size_t>(x) * bpp;

                if (intoResource) {
                    std::memcpy(tiledPixel, linearPixel, bpp);
                } else {
                    std::memcpy(linearPixel, tiledPixel, bpp);
                }
            }
        }
        return S_OK;
    }

    std::atomic<ULONG> m_refs{1};
    XG_TEXTURE2D_DESC m_desc{};
    XG_RESOURCE_LAYOUT m_layout{};
};

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        char path[MAX_PATH]{};
        GetModuleFileNameA(hModule, path, MAX_PATH);
        sc::log::Write(sc::log::Level::Info, "xg_xs", "DllMain", path);
    }
    return TRUE;
}

extern "C" {

HRESULT WINAPI XGComputeTexture2DLayout(
    const XG_TEXTURE2D_DESC* pDesc,
    XG_RESOURCE_LAYOUT* pLayout) {
    if (pDesc && pLayout && (pDesc->TileMode == XG_TILE_MODE_64KB_S || sc::xg::IsBlockCompressed(pDesc->Format))) {
        return sc::xg::ComputeBcnOr64KbsLayout(*pDesc, *pLayout);
    }

    if (!pDesc || !pLayout) {
        return E_INVALIDARG;
    }
    return ComputeLayoutInternal(*pDesc, *pLayout);
}

HRESULT WINAPI XGCreateTexture2DComputer(
    const XG_TEXTURE2D_DESC* pDesc,
    IXGTextureAddressComputer** ppComputer) {
    if (pDesc && ppComputer && (pDesc->TileMode == XG_TILE_MODE_64KB_S || sc::xg::IsBlockCompressed(pDesc->Format))) {
        return sc::xg::CreateBcnOr64KbsComputer(*pDesc, ppComputer);
    }

    if (!pDesc || !ppComputer) {
        return E_INVALIDARG;
    }
    *ppComputer = nullptr;

    XG_RESOURCE_LAYOUT layout{};
    HRESULT hr = ComputeLayoutInternal(*pDesc, layout);
    if (FAILED(hr)) {
        return hr;
    }

    char msg[160];
    std::snprintf(msg, sizeof(msg),
                  "-> %ux%u mode=%s size=%llu B align=%llu B pitch=%u B",
                  pDesc->Width, pDesc->Height, TileModeName(pDesc->TileMode),
                  static_cast<unsigned long long>(layout.SizeBytes),
                  static_cast<unsigned long long>(layout.BaseAlignmentBytes),
                  layout.Plane[0].MipLayout[0].PitchBytes);
    sc::log::Write(sc::log::Level::Info, "xg_xs", __FUNCTION__, msg);

    *ppComputer = new ScarlettTexture2DComputer(*pDesc, layout);
    return S_OK;
}

} // extern "C"