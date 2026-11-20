#pragma once

#include "xg_xs.hpp"
#include "common/stub.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace sc::xg {

struct FormatInfo {
    uint32_t blockW;
    uint32_t blockH;
    uint32_t bytesPerElem;
    bool     isBlockCompressed;
    const char* name;
};

inline FormatInfo GetFormatInfo(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:    return { 4, 4,  8, true,  "BC1" };
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:    return { 4, 4, 16, true,  "BC2" };
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:    return { 4, 4, 16, true,  "BC3" };
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:         return { 4, 4,  8, true,  "BC4" };
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:         return { 4, 4, 16, true,  "BC5" };
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:         return { 4, 4, 16, true,  "BC6H" };
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:    return { 4, 4, 16, true,  "BC7" };
        case DXGI_FORMAT_R16G16B16A16_FLOAT:return { 1, 1,  8, false, "RGBA16F" };
        case DXGI_FORMAT_R32G32B32A32_FLOAT:return { 1, 1, 16, false, "RGBA32F" };
        default:                            return { 1, 1,  4, false, "RGBA8" };
    }
}

inline bool IsBlockCompressed(DXGI_FORMAT fmt) {
    return GetFormatInfo(fmt).isBlockCompressed;
}

inline uint64_t AlignUp64(uint64_t v, uint64_t a) {
    return (v + (a - 1ULL)) & ~(a - 1ULL);
}

// 64KB_S tile geometry: every element size forms a 256 B micro-tile inside a 16x16 grid = 64 KB macro-tile
struct Tile64KbsDims {
    uint32_t tileW, tileH;
    uint32_t microW, microH;
};

inline Tile64KbsDims Get64KbsDims(uint32_t bpe) {
    if (bpe >= 16u) return {  64u,  64u, 4u, 4u }; // 16 B: 4x4=256 B micro, 64x64=64 KB macro
    if (bpe >= 8u)  return { 128u,  64u, 8u, 4u }; //  8 B: 8x4=256 B micro, 128x64=64 KB macro
    return                 { 128u, 128u, 8u, 8u }; //  4 B: 8x8=256 B micro, 128x128=64 KB macro
}

inline uint32_t Spread4(uint32_t v) {
    v &= 0xFu;
    v = (v | (v << 2)) & 0x33u;
    v = (v | (v << 1)) & 0x55u;
    return v;
}

inline uint32_t Morton4x4(uint32_t x, uint32_t y) {
    return Spread4(x) | (Spread4(y) << 1);
}

inline uint32_t MicroTileIndex(uint32_t lx, uint32_t ly, uint32_t bpe) {
    if (bpe >= 16u) {
        return ((lx & 1u) | ((ly & 1u) << 1) | ((lx & 2u) << 1) | ((ly & 2u) << 2)); // 4x4 -> 0..15
    }
    if (bpe >= 8u) {
        return ((lx & 1u) | ((ly & 1u) << 1) | ((lx & 2u) << 1) | ((ly & 2u) << 2) | ((lx & 4u) << 2)); // 8x4 -> 0..31
    }
    return Morton4x4(lx & 7u, ly & 7u); // 8x8 -> 0..63
}

struct InternalMipMeta {
    uint32_t elemsW{0};
    uint32_t elemsH{0};
    uint32_t paddedW{0};
    uint32_t paddedH{0};
    uint32_t pitchBytes{0};
    uint64_t offsetBytes{0};
    uint64_t sizeBytes{0};
    bool     isMacroTiled{false};
};

struct InternalSurfaceMeta {
    FormatInfo      fmt{};
    Tile64KbsDims   dims{};
    uint32_t        mipLevels{1};
    uint32_t        arraySize{1};
    bool            is64Kbs{false};
    uint64_t        sliceSizeBytes{0};
    uint64_t        totalSizeBytes{0};
    uint64_t        baseAlignBytes{512};
    InternalMipMeta mips[16]{};
};

inline HRESULT ComputeBcnOr64KbsMeta(
    const XG_TEXTURE2D_DESC& desc,
    InternalSurfaceMeta& meta,
    XG_RESOURCE_LAYOUT& layout) {
    if (desc.Width == 0 || desc.Height == 0) {
        return E_INVALIDARG;
    }
    std::memset(&layout, 0, sizeof(layout));
    meta = {};
    meta.fmt = GetFormatInfo(desc.Format);
    meta.dims = Get64KbsDims(meta.fmt.bytesPerElem);
    meta.mipLevels = std::clamp<uint32_t>(desc.MipLevels ? desc.MipLevels : 1u, 1u, 15u);
    meta.arraySize = std::max<uint32_t>(desc.ArraySize, 1u);
    meta.is64Kbs = (desc.TileMode == XG_TILE_MODE_64KB_S);

    uint64_t offset = 0;
    for (uint32_t m = 0; m < meta.mipLevels; ++m) {
        const uint32_t mw = std::max<uint32_t>(1u, desc.Width >> m);
        const uint32_t mh = std::max<uint32_t>(1u, desc.Height >> m);
        const uint32_t bw = (mw + meta.fmt.blockW - 1u) / meta.fmt.blockW;
        const uint32_t bh = (mh + meta.fmt.blockH - 1u) / meta.fmt.blockH;
        const uint32_t pitch = static_cast<uint32_t>(AlignUp64(static_cast<uint64_t>(bw) * meta.fmt.bytesPerElem, 256ULL));

        auto& mm = meta.mips[m];
        mm.elemsW = bw;
        mm.elemsH = bh;
        mm.pitchBytes = pitch;

        if (meta.is64Kbs) {
            const bool macro = (m == 0u) || (bw >= meta.dims.tileW) || (bh >= meta.dims.tileH);
            mm.isMacroTiled = macro;
            if (macro) {
                offset = AlignUp64(offset, 65536ULL);
                const uint32_t tx = (bw + meta.dims.tileW - 1u) / meta.dims.tileW;
                const uint32_t ty = (bh + meta.dims.tileH - 1u) / meta.dims.tileH;
                mm.paddedW = tx * meta.dims.tileW;
                mm.paddedH = ty * meta.dims.tileH;
                mm.offsetBytes = offset;
                mm.sizeBytes = static_cast<uint64_t>(tx) * ty * 65536ULL;
            } else {
                offset = AlignUp64(offset, 256ULL);
                const uint32_t mtx = (bw + meta.dims.microW - 1u) / meta.dims.microW;
                const uint32_t mty = (bh + meta.dims.microH - 1u) / meta.dims.microH;
                mm.paddedW = mtx * meta.dims.microW;
                mm.paddedH = mty * meta.dims.microH;
                mm.offsetBytes = offset;
                mm.sizeBytes = static_cast<uint64_t>(mtx) * mty * 256ULL;
            }
        } else {
            offset = AlignUp64(offset, 512ULL);
            mm.paddedW = pitch / meta.fmt.bytesPerElem;
            mm.paddedH = bh;
            mm.offsetBytes = offset;
            mm.sizeBytes = AlignUp64(static_cast<uint64_t>(pitch) * bh, 512ULL);
        }
        offset += mm.sizeBytes;

        auto& outMip = layout.Plane[0].MipLayout[m];
        outMip.OffsetBytes = mm.offsetBytes;
        outMip.SizeBytes = mm.sizeBytes;
        outMip.PitchBytes = mm.pitchBytes;
        outMip.PaddedWidthElements = mm.paddedW;
        outMip.PaddedHeightElements = mm.paddedH;
        outMip.TileMode = desc.TileMode;
    }

    meta.baseAlignBytes = meta.is64Kbs ? 65536ULL : 512ULL;
    meta.sliceSizeBytes = AlignUp64(offset, meta.baseAlignBytes);
    meta.totalSizeBytes = meta.sliceSizeBytes * meta.arraySize;

    layout.SizeBytes = meta.totalSizeBytes;
    layout.BaseAlignmentBytes = meta.baseAlignBytes;
    layout.Planes = 1;
    layout.Plane[0].SizeBytes = meta.totalSizeBytes;
    layout.Plane[0].BytesPerElement = meta.fmt.bytesPerElem;
    return S_OK;
}

inline HRESULT ComputeBcnOr64KbsLayout(const XG_TEXTURE2D_DESC& desc, XG_RESOURCE_LAYOUT& layout) {
    InternalSurfaceMeta meta{};
    return ComputeBcnOr64KbsMeta(desc, meta, layout);
}

class ScarlettTexture64KbsComputer final : public IXGTextureAddressComputer {
public:
    ScarlettTexture64KbsComputer(const InternalSurfaceMeta& meta, const XG_RESOURCE_LAYOUT& layout)
        : m_meta(meta), m_layout(layout), m_totalSizeBytes(meta.totalSizeBytes), m_baseAlignBytes(meta.baseAlignBytes) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
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
        ULONG c = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (c == 0) delete this;
        return c;
    }

    HRESULT STDMETHODCALLTYPE GetResourceLayout( XG_RESOURCE_LAYOUT* pLayout) override {
        if (!pLayout) return E_POINTER;
        *pLayout = m_layout;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CopyIntoSubresource( void* pResourceBaseAddress, uint32_t Subresource, const void* pSourceData, uint32_t SourceRowPitch, uint32_t SourceSlicePitch) override {
        (void)SourceSlicePitch;
        return TransferSubresource(pResourceBaseAddress, Subresource, pSourceData, SourceRowPitch, true);
    }
    HRESULT STDMETHODCALLTYPE CopyFromSubresource( void* pDestinationData, uint32_t Subresource, const void* pResourceBaseAddress, uint32_t DestRowPitch, uint32_t DestSlicePitch) override {
        (void)DestSlicePitch;
        return TransferSubresource(pResourceBaseAddress, Subresource, pDestinationData, DestRowPitch, false);
    }

private:
    uint64_t ComputeElementOffsetBytes(uint32_t mip, uint32_t bx, uint32_t by, uint32_t slice) const {
        const uint32_t m = std::min(mip, m_meta.mipLevels - 1u);
        const auto& mm = m_meta.mips[m];
        const uint64_t base = static_cast<uint64_t>(slice) * m_meta.sliceSizeBytes + mm.offsetBytes;
        const uint32_t bpe = m_meta.fmt.bytesPerElem;

        if (!m_meta.is64Kbs) {
            return base + static_cast<uint64_t>(by) * mm.pitchBytes + static_cast<uint64_t>(bx) * bpe;
        }

        const uint32_t lx = bx % m_meta.dims.microW;
        const uint32_t ly = by % m_meta.dims.microH;
        const uint32_t microByteOff = MicroTileIndex(lx, ly, bpe) * bpe;

        if (mm.isMacroTiled) {
            const uint32_t tilesX = mm.paddedW / m_meta.dims.tileW;
            const uint32_t tx = bx / m_meta.dims.tileW;
            const uint32_t ty = by / m_meta.dims.tileH;
            const uint32_t mx = (bx % m_meta.dims.tileW) / m_meta.dims.microW;
            const uint32_t my = (by % m_meta.dims.tileH) / m_meta.dims.microH;
            const uint64_t macroIdx = static_cast<uint64_t>(ty) * tilesX + tx;
            const uint64_t subMicro = Morton4x4(mx, my);
            return base + macroIdx * 65536ULL + subMicro * 256ULL + microByteOff;
        }

        const uint32_t microTilesX = mm.paddedW / m_meta.dims.microW;
        const uint32_t mtx = bx / m_meta.dims.microW;
        const uint32_t mty = by / m_meta.dims.microH;
        return base + (static_cast<uint64_t>(mty) * microTilesX + mtx) * 256ULL + microByteOff;
    }

    HRESULT TransferSubresource(
        const void* tiledBase, uint32_t subresource, const void* linearConst, uint32_t rowPitch, bool copyInto) {
        if (!tiledBase || !linearConst) return E_INVALIDARG;
        const uint32_t mip = subresource % m_meta.mipLevels;
        const uint32_t slice = subresource / m_meta.mipLevels;
        if (slice >= m_meta.arraySize) return E_INVALIDARG;

        const auto& mm = m_meta.mips[mip];
        const uint32_t bpe = m_meta.fmt.bytesPerElem;
        auto* tiled = const_cast<uint8_t*>(static_cast<const uint8_t*>(tiledBase));
        auto* linear = const_cast<uint8_t*>(static_cast<const uint8_t*>(linearConst));

        for (uint32_t by = 0; by < mm.elemsH; ++by) {
            uint8_t* rowPtr = linear + static_cast<uint64_t>(by) * rowPitch;
            for (uint32_t bx = 0; bx < mm.elemsW; ++bx) {
                const uint64_t off = ComputeElementOffsetBytes(mip, bx, by, slice);
                if (copyInto) {
                    std::memcpy(tiled + off, rowPtr + static_cast<uint64_t>(bx) * bpe, bpe);
                } else {
                    std::memcpy(rowPtr + static_cast<uint64_t>(bx) * bpe, tiled + off, bpe);
                }
            }
        }
        return S_OK;
    }

    std::atomic<ULONG>  m_refs{1};
    InternalSurfaceMeta m_meta;
    XG_RESOURCE_LAYOUT  m_layout;
    uint64_t            m_totalSizeBytes;
    uint64_t            m_baseAlignBytes;
};

inline HRESULT CreateBcnOr64KbsComputer(
    const XG_TEXTURE2D_DESC& desc, IXGTextureAddressComputer** computer) {
    if (!computer) return E_POINTER;
    *computer = nullptr;
    InternalSurfaceMeta meta{};
    XG_RESOURCE_LAYOUT layout{};
    HRESULT hr = ComputeBcnOr64KbsMeta(desc, meta, layout);
    if (FAILED(hr)) return hr;

    *computer = new ScarlettTexture64KbsComputer(meta, layout);
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "-> %ux%u (%u mips) fmt=%s(%uB) mode=%s size=%llu B align=%llu B pitch=%u B",
                  desc.Width, desc.Height, meta.mipLevels, meta.fmt.name, meta.fmt.bytesPerElem,
                  meta.is64Kbs ? "64KB_S(256B-micro)" : "LINEAR",
                  static_cast<unsigned long long>(meta.totalSizeBytes),
                  static_cast<unsigned long long>(meta.baseAlignBytes),
                  meta.mips[0].pitchBytes);
    sc::log::Write(sc::log::Level::Info, "xg_xs", "XGCreateTexture2DComputer", buf);
    return S_OK;
}

} // namespace sc::xg
