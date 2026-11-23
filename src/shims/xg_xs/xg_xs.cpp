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

// --- Xbox GDK (xg_x.dll) binary-compatible ABI ---
namespace gdk_xg {

struct GDK_XG_TEXTURE2D_DESC {
    uint32_t    Width;             // +0x00
    uint32_t    Height;            // +0x04
    uint32_t    MipLevels;         // +0x08
    uint32_t    ArraySize;         // +0x0C
    DXGI_FORMAT Format;            // +0x10
    uint32_t    SampleCount;       // +0x14
    uint32_t    SampleQuality;     // +0x18
    uint32_t    Usage;             // +0x1C
    uint32_t    BindFlags;         // +0x20
    uint32_t    CPUAccessFlags;    // +0x24
    uint32_t    MiscFlags;         // +0x28
    uint32_t    ESRAMOffsetBytes;  // +0x2C
    uint32_t    ESRAMUsageBytes;   // +0x30
    uint32_t    TileMode;          // +0x34
};
static_assert(sizeof(GDK_XG_TEXTURE2D_DESC) == 0x38, "GDK_XG_TEXTURE2D_DESC must be 0x38 bytes");

struct GDK_D3D12_RESOURCE_DESC {
    uint32_t    Dimension;         // +0x00 (2=1D, 3=2D, 4=3D)
    uint32_t    Pad0;              // +0x04
    uint64_t    Alignment;         // +0x08
    uint64_t    Width;             // +0x10
    uint32_t    Height;            // +0x18
    uint16_t    DepthOrArraySize;  // +0x1C
    uint16_t    MipLevels;         // +0x1E
    DXGI_FORMAT Format;            // +0x20
    uint32_t    Pad1;              // +0x24
    uint32_t    SampleCount;       // +0x28
    uint32_t    SampleQuality;     // +0x2C
    uint32_t    Layout;            // +0x30
    uint32_t    Flags;             // +0x34
};
static_assert(sizeof(GDK_D3D12_RESOURCE_DESC) == 0x38, "GDK_D3D12_RESOURCE_DESC must be 0x38 bytes");

struct GDK_XG_MIPLEVEL_LAYOUT {
    uint64_t SizeBytes;                 // +0x00
    uint64_t OffsetBytes;               // +0x08
    uint64_t Slice2DSizeBytes;          // +0x10
    uint32_t PitchPixels;               // +0x18
    uint32_t PitchBytes;                // +0x1C
    uint32_t AlignmentBytes;            // +0x20
    uint32_t PaddedWidthElements;       // +0x24
    uint32_t PaddedHeightElements;      // +0x28
    uint32_t PaddedDepthOrArraySize;    // +0x2C
    uint32_t WidthElements;             // +0x30
    uint32_t HeightElements;            // +0x34
    uint32_t DepthOrArraySize;          // +0x38
    uint32_t SampleCount;               // +0x3C
    uint32_t TileMode;                  // +0x40
    uint32_t BankRotation;              // +0x44
    uint64_t Reserved[3];               // +0x48..0x5F
};
static_assert(sizeof(GDK_XG_MIPLEVEL_LAYOUT) == 0x60, "GDK_XG_MIPLEVEL_LAYOUT must be 0x60 bytes");

struct GDK_XG_PLANE_LAYOUT {
    uint32_t               Usage;              // +0x00
    uint32_t               Pad0;               // +0x04
    uint64_t               SizeBytes;          // +0x08
    uint64_t               BaseOffsetBytes;    // +0x10
    uint64_t               BaseAlignmentBytes; // +0x18
    uint32_t               BytesPerElement;    // +0x20
    uint32_t               MipLevels;          // +0x24
    GDK_XG_MIPLEVEL_LAYOUT MipLayout[15];      // +0x28..0x5C7
};
static_assert(sizeof(GDK_XG_PLANE_LAYOUT) == 0x5C8, "GDK_XG_PLANE_LAYOUT must be 0x5C8 bytes");

struct GDK_XG_RESOURCE_LAYOUT {
    uint64_t            SizeBytes;          // +0x00
    uint64_t            BaseAlignmentBytes; // +0x08
    uint32_t            Flags;              // +0x10
    uint32_t            Planes;             // +0x14
    GDK_XG_PLANE_LAYOUT Plane[4];           // +0x18..0x1737
};
static_assert(sizeof(GDK_XG_RESOURCE_LAYOUT) == 0x1738, "GDK_XG_RESOURCE_LAYOUT must be 0x1738 bytes");
static_assert(offsetof(GDK_XG_RESOURCE_LAYOUT, Plane) == 0x18, "Plane offset mismatch");
static_assert(offsetof(GDK_XG_RESOURCE_LAYOUT, Plane) +
              offsetof(GDK_XG_PLANE_LAYOUT, MipLayout) +
              offsetof(GDK_XG_MIPLEVEL_LAYOUT, OffsetBytes) == 0x48, "MipLayout[0].OffsetBytes must be at +0x48");

struct FormatMetrics {
    uint32_t blockW;
    uint32_t blockH;
    uint32_t bpe;
};

FormatMetrics GetFormatMetrics(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
            return { 4, 4, 8 };
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return { 4, 4, 16 };
        case DXGI_FORMAT_R8_TYPELESS:
        case DXGI_FORMAT_R8_UNORM:
        case DXGI_FORMAT_R8_UINT:
        case DXGI_FORMAT_R8_SNORM:
        case DXGI_FORMAT_R8_SINT:
        case DXGI_FORMAT_A8_UNORM:
            return { 1, 1, 1 };
        case DXGI_FORMAT_R8G8_TYPELESS:
        case DXGI_FORMAT_R8G8_UNORM:
        case DXGI_FORMAT_R8G8_UINT:
        case DXGI_FORMAT_R8G8_SNORM:
        case DXGI_FORMAT_R8G8_SINT:
        case DXGI_FORMAT_R16_TYPELESS:
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_UNORM:
        case DXGI_FORMAT_R16_UINT:
        case DXGI_FORMAT_R16_SNORM:
        case DXGI_FORMAT_R16_SINT:
        case DXGI_FORMAT_B5G6R5_UNORM:
        case DXGI_FORMAT_B5G5R5A1_UNORM:
        case DXGI_FORMAT_B4G4R4A4_UNORM:
            return { 1, 1, 2 };
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
        case DXGI_FORMAT_R16G16B16A16_UINT:
        case DXGI_FORMAT_R16G16B16A16_SNORM:
        case DXGI_FORMAT_R16G16B16A16_SINT:
        case DXGI_FORMAT_R32G32_TYPELESS:
        case DXGI_FORMAT_R32G32_FLOAT:
        case DXGI_FORMAT_R32G32_UINT:
        case DXGI_FORMAT_R32G32_SINT:
            return { 1, 1, 8 };
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_UINT:
        case DXGI_FORMAT_R32G32B32A32_SINT:
            return { 1, 1, 16 };
        default:
            return { 1, 1, 4 };
    }
}

uint32_t ComputeFullMipCount(uint32_t w, uint32_t h, uint32_t d) {
    uint32_t maxDim = std::max({ w, h, d, 1u });
    uint32_t mips = 1;
    while (maxDim > 1u && mips < 15u) {
        maxDim >>= 1;
        ++mips;
    }
    return mips;
}

bool IsHostCleanRoomTestProcess() {
    static const bool s_isTest = []() {
        char exePath[MAX_PATH]{};
        DWORD n = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        if (n == 0) return false;
        const char* base = exePath;
        for (const char* p = exePath; *p; ++p) {
            if (*p == '\\' || *p == '/') base = p + 1;
        }
        return (_stricmp(base, "scarlett_tests.exe") == 0) ||
               (_stricmp(base, "dummy_title.exe") == 0);
    }();
    return s_isTest;
}

void BuildGdkLayout(
    uint32_t width, uint32_t height, uint32_t depthOrArray, uint32_t mipLevels,
    DXGI_FORMAT fmt, uint32_t sampleCount, uint32_t tileMode, bool is3D,
    GDK_XG_RESOURCE_LAYOUT& out) {
    std::memset(&out, 0, sizeof(out));
    const FormatMetrics fm = GetFormatMetrics(fmt);
    const uint32_t w0 = std::max(width, 1u);
    const uint32_t h0 = std::max(height, 1u);
    const uint32_t d0 = std::max(depthOrArray, 1u);
    const uint32_t mips = std::clamp(mipLevels == 0 ? ComputeFullMipCount(w0, h0, is3D ? d0 : 1u) : mipLevels, 1u, 15u);
    const uint32_t samples = std::max(sampleCount, 1u);

    const bool is1DThin = (tileMode == 8u);
    const uint64_t baseAlign = (tileMode == 0 || is1DThin) ? 256ULL : 65536ULL;
    out.BaseAlignmentBytes = baseAlign;
    out.Planes = 1;

    GDK_XG_PLANE_LAYOUT& pl = out.Plane[0];
    pl.Usage = 0; // XG_PLANE_USAGE_DEFAULT
    pl.BaseOffsetBytes = 0;
    pl.BaseAlignmentBytes = baseAlign;
    pl.BytesPerElement = fm.bpe;
    pl.MipLevels = mips;

    uint64_t offset = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        const uint32_t mw = std::max(1u, w0 >> m);
        const uint32_t mh = std::max(1u, h0 >> m);
        const uint32_t md = is3D ? std::max(1u, d0 >> m) : d0;
        const uint32_t ew = (mw + fm.blockW - 1u) / fm.blockW;
        const uint32_t eh = (mh + fm.blockH - 1u) / fm.blockH;

        uint32_t pw = 0;
        uint32_t ph = 0;
        uint32_t pitchBytes = 0;
        uint64_t sliceBytes = 0;
        uint64_t mipBytes = 0;
        uint32_t mipAlign = 256u;

        if (is1DThin) {
            // AMD GCN 1D_THIN: 8x8 Morton micro-tiles, 256-byte pipe-interleave alignment
            ph = (eh + 7u) & ~7u;
            pw = (ew + 7u) & ~7u;
            while ((static_cast<uint64_t>(pw) * ph * fm.bpe) % 256ULL != 0ULL) {
                pw += 8u;
            }
            pitchBytes = pw * fm.bpe;
            sliceBytes = static_cast<uint64_t>(pw) * ph * fm.bpe;
            mipBytes = AlignUp(sliceBytes * md * samples, 256ULL);
            mipAlign = 256u;
        } else {
            pitchBytes = static_cast<uint32_t>(AlignUp(static_cast<uint64_t>(ew) * fm.bpe, 256ULL));
            pw = pitchBytes / fm.bpe;
            ph = eh;
            sliceBytes = AlignUp(static_cast<uint64_t>(pitchBytes) * eh, 256ULL);
            mipBytes = AlignUp(sliceBytes * md * samples, 512ULL);
            mipAlign = 512u;
        }

        offset = AlignUp(offset, mipAlign);
        GDK_XG_MIPLEVEL_LAYOUT& ml = pl.MipLayout[m];
        ml.SizeBytes = mipBytes;
        ml.OffsetBytes = offset;
        ml.Slice2DSizeBytes = sliceBytes;
        ml.PitchPixels = pw * fm.blockW;
        ml.PitchBytes = pitchBytes;
        ml.AlignmentBytes = mipAlign;
        ml.PaddedWidthElements = pw;
        ml.PaddedHeightElements = ph;
        ml.PaddedDepthOrArraySize = md;
        ml.WidthElements = ew;
        ml.HeightElements = eh;
        ml.DepthOrArraySize = md;
        ml.SampleCount = samples;
        ml.TileMode = tileMode;
        offset += mipBytes;
    }

    const uint64_t total = AlignUp(offset, baseAlign);
    pl.SizeBytes = total;
    out.SizeBytes = total;
}

// Xbox GDK XGTextureAddressComputer vtable (NO IUnknown::QueryInterface!)
//   slot 0 (+0x00): AddRef
//   slot 1 (+0x08): Release
//   slot 2 (+0x10): GetResourceLayout
//   slot 3 (+0x18): GetResourceSizeBytes
//   slot 4 (+0x20): GetResourceBaseAlignmentBytes
//   slot 5 (+0x28): GetMipLevelOffsetBytes
//   slot 6 (+0x30): GetTexelElementOffsetBytes
//   slot 7 (+0x38): GetTexelCoordinate
//   slot 8 (+0x40): CopyIntoSubresource
//   slot 9 (+0x48): CopyFromSubresource
class GdkXGTextureAddressComputer final {
public:
    explicit GdkXGTextureAddressComputer(const GDK_XG_RESOURCE_LAYOUT& layout)
        : m_layout(layout) {}

    virtual ULONG STDMETHODCALLTYPE AddRef() {
        return m_refs.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    virtual ULONG STDMETHODCALLTYPE Release() {
        ULONG c = m_refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (c == 0) delete this;
        return c;
    }

    virtual HRESULT STDMETHODCALLTYPE GetResourceLayout(GDK_XG_RESOURCE_LAYOUT* pLayout) {
        if (!pLayout) return E_POINTER;
        *pLayout = m_layout;
        return S_OK;
    }

    virtual uint64_t STDMETHODCALLTYPE GetResourceSizeBytes() {
        return m_layout.SizeBytes;
    }

    virtual uint64_t STDMETHODCALLTYPE GetResourceBaseAlignmentBytes() {
        return m_layout.BaseAlignmentBytes;
    }

    virtual uint64_t STDMETHODCALLTYPE GetMipLevelOffsetBytes(uint32_t plane, uint32_t mipLevel) {
        if (plane >= m_layout.Planes || mipLevel >= 15u) return 0;
        return m_layout.Plane[plane].MipLayout[mipLevel].OffsetBytes;
    }

    virtual uint64_t STDMETHODCALLTYPE GetTexelElementOffsetBytes(
        uint32_t plane, uint32_t mipLevel, uint64_t x, uint32_t y, uint32_t zOrSlice, uint32_t /*sample*/) {
        if (plane >= m_layout.Planes || mipLevel >= 15u) return 0;
        const auto& pl = m_layout.Plane[plane];
        const auto& ml = pl.MipLayout[mipLevel];
        return ml.OffsetBytes + static_cast<uint64_t>(zOrSlice) * ml.Slice2DSizeBytes +
               static_cast<uint64_t>(y) * ml.PitchBytes + x * pl.BytesPerElement;
    }

    virtual HRESULT STDMETHODCALLTYPE GetTexelCoordinate(
        uint64_t /*offsetBytes*/, uint32_t* pPlane, uint32_t* pMipLevel,
        uint64_t* pX, uint32_t* pY, uint32_t* pZOrSlice, uint32_t* pSample) {
        if (pPlane) *pPlane = 0;
        if (pMipLevel) *pMipLevel = 0;
        if (pX) *pX = 0;
        if (pY) *pY = 0;
        if (pZOrSlice) *pZOrSlice = 0;
        if (pSample) *pSample = 0;
        return S_OK;
    }

    virtual HRESULT STDMETHODCALLTYPE CopyIntoSubresource(
        void* pResourceBaseAddress, uint32_t plane, uint32_t subresource,
        const void* pSourceData, uint32_t sourceRowPitch, uint32_t /*sourceSlicePitch*/) {
        return TransferSubresource(pResourceBaseAddress, plane, subresource,
                                   const_cast<void*>(pSourceData), sourceRowPitch, true);
    }

    virtual HRESULT STDMETHODCALLTYPE CopyFromSubresource(
        void* pDestinationData, uint32_t plane, uint32_t subresource,
        const void* pResourceBaseAddress, uint32_t destRowPitch, uint32_t /*destSlicePitch*/) {
        return TransferSubresource(const_cast<void*>(pResourceBaseAddress), plane, subresource,
                                   pDestinationData, destRowPitch, false);
    }

    // slot 10 (+0x50): GetResourceTiling
    virtual HRESULT STDMETHODCALLTYPE GetResourceTiling(
        uint32_t* pNumTilesForEntireResource,
        void* pPackedMipDesc,
        void* pStandardTileShapeForNonPackedMips,
        uint32_t* pNumSubresourceTilings,
        uint32_t firstSubresourceTilingToGet,
        void* pSubresourceTilingsForNonPackedMips) {
        const auto& pl = m_layout.Plane[0];
        const uint32_t mips = std::max(pl.MipLevels, 1u);

        // Compute standard vs packed mips per D3D12/GDK 64 KB tile rules
        uint32_t numStandardMips = 0;
        uint32_t stdTilesPerSlice = 0;
        uint32_t mipW[15]{};
        uint32_t mipH[15]{};
        uint32_t mipStartTile[15]{};

        for (uint32_t m = 0; m < std::min(mips, 15u); ++m) {
            const auto& ml = pl.MipLayout[m];
            const uint64_t sliceBytes = (ml.Slice2DSizeBytes > 0)
                ? ml.Slice2DSizeBytes
                : (static_cast<uint64_t>(ml.PitchBytes) * std::max(ml.HeightElements, 1u));
            if (m == 0 || sliceBytes >= 65536ULL) {
                const uint32_t tilesForMip = static_cast<uint32_t>(
                    std::max<uint64_t>(1ULL, (sliceBytes + 65535ULL) / 65536ULL));
                uint32_t wTiles = std::max(1u, static_cast<uint32_t>((ml.WidthElements + 63u) / 64u));
                uint32_t hTiles = std::max(1u, (tilesForMip + wTiles - 1u) / wTiles);
                if (wTiles * hTiles < tilesForMip) hTiles = tilesForMip;
                mipW[m] = wTiles;
                mipH[m] = hTiles;
                mipStartTile[m] = stdTilesPerSlice;
                stdTilesPerSlice += wTiles * hTiles;
                ++numStandardMips;
            } else {
                break;
            }
        }

        const uint32_t numPackedMips = (mips > numStandardMips) ? (mips - numStandardMips) : 0u;
        const uint32_t numTilesForPackedMips = (numPackedMips > 0) ? 1u : 0u;
        const uint32_t tilesPerSlice = stdTilesPerSlice + numTilesForPackedMips;
        const uint32_t layoutTiles = static_cast<uint32_t>(
            std::max<uint64_t>(1ULL, (m_layout.SizeBytes + 65535ULL) / 65536ULL));
        const uint32_t totalTiles = std::max(layoutTiles, tilesPerSlice);

        if (pNumTilesForEntireResource) *pNumTilesForEntireResource = totalTiles;
        if (pPackedMipDesc) {
            struct PackedMipInfo {
                uint8_t  NumStandardMips;
                uint8_t  NumPackedMips;
                uint32_t NumTilesForPackedMips;
                uint32_t StartTileIndexInOverallResource;
            };
            auto* pm = static_cast<PackedMipInfo*>(pPackedMipDesc);
            pm->NumStandardMips = static_cast<uint8_t>(numStandardMips);
            pm->NumPackedMips = static_cast<uint8_t>(numPackedMips);
            pm->NumTilesForPackedMips = numTilesForPackedMips;
            pm->StartTileIndexInOverallResource = (numPackedMips > 0) ? stdTilesPerSlice : 0u;
        }
        if (pStandardTileShapeForNonPackedMips) {
            struct TileShape {
                uint32_t WidthInTexels;
                uint32_t HeightInTexels;
                uint32_t DepthInTexels;
            };
            auto* ts = static_cast<TileShape*>(pStandardTileShapeForNonPackedMips);
            ts->WidthInTexels = (pl.BytesPerElement >= 16u) ? 256u : 128u;
            ts->HeightInTexels = (pl.BytesPerElement >= 16u) ? 256u : 128u;
            ts->DepthInTexels = 1u;
        }
        if (pNumSubresourceTilings) {
            const uint32_t avail = (firstSubresourceTilingToGet < mips) ? (mips - firstSubresourceTilingToGet) : 0u;
            const uint32_t count = std::min(*pNumSubresourceTilings, avail);
            *pNumSubresourceTilings = count;
            if (pSubresourceTilingsForNonPackedMips && count > 0) {
                struct SubresourceTiling {
                    uint32_t WidthInTiles;
                    uint16_t HeightInTiles;
                    uint16_t DepthInTiles;
                    uint32_t StartTileIndexInOverallResource;
                };
                auto* st = static_cast<SubresourceTiling*>(pSubresourceTilingsForNonPackedMips);
                for (uint32_t i = 0; i < count; ++i) {
                    const uint32_t mip = firstSubresourceTilingToGet + i;
                    if (mip < numStandardMips && mip < 15u) {
                        st[i].WidthInTiles = mipW[mip];
                        st[i].HeightInTiles = static_cast<uint16_t>(std::min(mipH[mip], 0xFFFFu));
                        st[i].DepthInTiles = 1;
                        st[i].StartTileIndexInOverallResource = mipStartTile[mip];
                    } else {
                        st[i].WidthInTiles = 0;
                        st[i].HeightInTiles = 0;
                        st[i].DepthInTiles = 0;
                        st[i].StartTileIndexInOverallResource = 0xFFFFFFFFu; // D3D12_PACKED_TILE
                    }
                }
            }
        }
        if (mips > 1 || m_layout.SizeBytes >= 2000000ULL) {
            char msg[224];
            std::snprintf(msg, sizeof(msg),
                          "-> [GDK] GetResourceTiling tiles=%u stdMips=%u packMips=%u packTiles=%u first=%u cnt=%u size=%llu B",
                          totalTiles, numStandardMips, numPackedMips, numTilesForPackedMips,
                          firstSubresourceTilingToGet,
                          pNumSubresourceTilings ? *pNumSubresourceTilings : 0u,
                          static_cast<unsigned long long>(m_layout.SizeBytes));
            sc::log::Write(sc::log::Level::Info, "xg_xs", "GdkXGTextureAddressComputer", msg);
        }
        return S_OK;
    }
    // Trailing safety slots 11..15 to prevent null vtable calls on extended GDK interfaces
    virtual HRESULT STDMETHODCALLTYPE ExtSlot11(uintptr_t a1, uintptr_t a2, uintptr_t a3) { return LogUnimpl(11, a1, a2, a3); }
    virtual HRESULT STDMETHODCALLTYPE ExtSlot12(uintptr_t a1, uintptr_t a2, uintptr_t a3) { return LogUnimpl(12, a1, a2, a3); }
    virtual HRESULT STDMETHODCALLTYPE ExtSlot13(uintptr_t a1, uintptr_t a2, uintptr_t a3) { return LogUnimpl(13, a1, a2, a3); }
    virtual HRESULT STDMETHODCALLTYPE ExtSlot14(uintptr_t a1, uintptr_t a2, uintptr_t a3) { return LogUnimpl(14, a1, a2, a3); }
    virtual HRESULT STDMETHODCALLTYPE ExtSlot15(uintptr_t a1, uintptr_t a2, uintptr_t a3) { return LogUnimpl(15, a1, a2, a3); }

private:
    static HRESULT LogUnimpl(int slot, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
        static std::atomic<uint32_t> s_u{0};
        if (s_u.fetch_add(1, std::memory_order_relaxed) < 8) {
            char msg[160];
            std::snprintf(msg, sizeof(msg), "-> UNIMPLEMENTED slot=%d a1=0x%llX a2=0x%llX a3=0x%llX",
                          slot, static_cast<unsigned long long>(a1),
                          static_cast<unsigned long long>(a2), static_cast<unsigned long long>(a3));
            sc::log::Write(sc::log::Level::Warn, "xg_xs", "GdkXGTextureAddressComputer", msg);
        }
        return S_OK;
    }
    HRESULT TransferSubresource(
        void* resBase, uint32_t plane, uint32_t subresource,
        void* linearData, uint32_t linearRowPitch, bool copyInto) {
        if (!resBase || !linearData || plane >= m_layout.Planes) return E_INVALIDARG;
        const auto& pl = m_layout.Plane[plane];
        const uint32_t mips = std::max(pl.MipLevels, 1u);
        const uint32_t mip = subresource % mips;
        const uint32_t slice = subresource / mips;
        const auto& ml = pl.MipLayout[mip];
        const size_t rowBytes = static_cast<size_t>(ml.WidthElements) * pl.BytesPerElement;
        auto* dstBase = static_cast<uint8_t*>(resBase) + ml.OffsetBytes + static_cast<uint64_t>(slice) * ml.Slice2DSizeBytes;
        auto* linBase = static_cast<uint8_t*>(linearData);
        for (uint32_t y = 0; y < ml.HeightElements; ++y) {
            uint8_t* rRow = dstBase + static_cast<size_t>(y) * ml.PitchBytes;
            uint8_t* lRow = linBase + static_cast<size_t>(y) * linearRowPitch;
            if (copyInto) std::memcpy(rRow, lRow, rowBytes);
            else          std::memcpy(lRow, rRow, rowBytes);
        }
        return S_OK;
    }

    std::atomic<ULONG>     m_refs{1};
    GDK_XG_RESOURCE_LAYOUT m_layout{};
};

} // namespace gdk_xg

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
    if (!gdk_xg::IsHostCleanRoomTestProcess()) {
        if (!pDesc || !ppComputer) return E_INVALIDARG;
        *ppComputer = nullptr;
        const auto* gd = reinterpret_cast<const gdk_xg::GDK_XG_TEXTURE2D_DESC*>(pDesc);
        gdk_xg::GDK_XG_RESOURCE_LAYOUT layout{};
        gdk_xg::BuildGdkLayout(gd->Width, gd->Height, gd->ArraySize, gd->MipLevels,
                               gd->Format, gd->SampleCount, gd->TileMode, false, layout);
        *ppComputer = reinterpret_cast<IXGTextureAddressComputer*>(new gdk_xg::GdkXGTextureAddressComputer(layout));
        char msg[192];
        std::snprintf(msg, sizeof(msg),
                      "-> [GDK] %ux%u mips=%u arr=%u fmt=%u tile=%u size=%llu B align=%llu B",
                      gd->Width, gd->Height, layout.Plane[0].MipLevels, gd->ArraySize,
                      static_cast<unsigned>(gd->Format), gd->TileMode,
                      static_cast<unsigned long long>(layout.SizeBytes),
                      static_cast<unsigned long long>(layout.BaseAlignmentBytes));
        sc::log::Write(sc::log::Level::Info, "xg_xs", __FUNCTION__, msg);
        return S_OK;
    }
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


HRESULT WINAPI XGCreateTextureComputer(const void* desc, void** ppComputer) {
    if (!desc || !ppComputer) return E_INVALIDARG;
    *ppComputer = nullptr;
    const auto* rd = static_cast<const gdk_xg::GDK_D3D12_RESOURCE_DESC*>(desc);
    const bool is3D = (rd->Dimension == 4u);
    const uint32_t w = static_cast<uint32_t>(std::max<uint64_t>(rd->Width, 1ULL));
    const uint32_t h = (rd->Dimension == 2u) ? 1u : std::max<uint32_t>(rd->Height, 1u);
    gdk_xg::GDK_XG_RESOURCE_LAYOUT layout{};
    gdk_xg::BuildGdkLayout(w, h, rd->DepthOrArraySize, rd->MipLevels,
                           rd->Format, rd->SampleCount, rd->Layout, is3D, layout);
    *ppComputer = new gdk_xg::GdkXGTextureAddressComputer(layout);
    char msg[192];
    std::snprintf(msg, sizeof(msg),
                  "-> [GDK] dim=%u %ux%ux%u mips=%u fmt=%u size=%llu B",
                  rd->Dimension, w, h, static_cast<unsigned>(rd->DepthOrArraySize),
                  layout.Plane[0].MipLevels, static_cast<unsigned>(rd->Format),
                  static_cast<unsigned long long>(layout.SizeBytes));
    sc::log::Write(sc::log::Level::Info, "xg_xs", __FUNCTION__, msg);
    return S_OK;
}

uint32_t WINAPI XGComputeOptimalTileMode(
    uint32_t /*dimension*/, uint32_t /*format*/, uint32_t /*width*/,
    uint32_t /*height*/, uint32_t /*depth*/, uint32_t /*sampleCount*/, uint32_t /*bindFlags*/) {
    return 8u; // XG_TILE_MODE_1D_THIN
}

uint32_t WINAPI XGSuggestSwizzleMode(
    uint32_t /*dimension*/, uint32_t /*format*/, uint64_t /*width*/,
    uint32_t /*height*/, uint16_t /*depthOrArraySize*/, uint32_t /*sampleCount*/, uint32_t /*flags*/) {
    static std::atomic<bool> s_logged{false};
    if (!s_logged.exchange(true, std::memory_order_relaxed)) {
        sc::log::Write(sc::log::Level::Info, "xg_xs", __FUNCTION__, "-> XG_SWIZZLE_MODE_LINEAR (0)");
    }
    return 0u;
}

} // extern "C"