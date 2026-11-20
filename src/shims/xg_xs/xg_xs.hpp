#pragma once

#include <cstdint>
#include <windows.h>
#include <dxgiformat.h>
#include <unknwn.h>

enum XG_TILE_MODE : uint32_t {
    XG_TILE_MODE_LINEAR   = 0,
    XG_TILE_MODE_2D_THIN  = 1,
    XG_TILE_MODE_64KB_S   = 2
};

struct XG_TEXTURE2D_DESC {
    uint32_t     Width;
    uint32_t     Height;
    uint32_t     MipLevels;
    uint32_t     ArraySize;
    DXGI_FORMAT  Format;
    XG_TILE_MODE TileMode;
};

struct XG_MIPLEVEL_LAYOUT {
    uint64_t OffsetBytes;
    uint64_t SizeBytes;
    uint32_t PitchBytes;
    uint32_t PaddedWidthElements;
    uint32_t PaddedHeightElements;
    uint32_t TileMode;
};

struct XG_PLANE_LAYOUT {
    uint64_t           BaseOffsetBytes;
    uint64_t           SizeBytes;
    uint32_t           BytesPerElement;
    uint32_t           MipLevels;
    XG_MIPLEVEL_LAYOUT MipLayout[15];
};

struct XG_RESOURCE_LAYOUT {
    uint64_t        SizeBytes;
    uint64_t        BaseAlignmentBytes;
    uint32_t        Planes;
    XG_PLANE_LAYOUT Plane[1];
};

inline constexpr GUID IID_IXGTextureAddressComputer =
    { 0x5c4a3101, 0x9b70, 0x4d1e, { 0xa1, 0x20, 0x00, 0x53, 0x43, 0x41, 0x52, 0x01 } };

struct IXGTextureAddressComputer : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetResourceLayout(
        XG_RESOURCE_LAYOUT* pLayout) = 0;
    virtual HRESULT STDMETHODCALLTYPE CopyIntoSubresource(
        void* pResourceBaseAddress,
        uint32_t Subresource,
        const void* pSourceData,
        uint32_t SourceRowPitch,
        uint32_t SourceSlicePitch) = 0;
    virtual HRESULT STDMETHODCALLTYPE CopyFromSubresource(
        void* pDestinationData,
        uint32_t Subresource,
        const void* pResourceBaseAddress,
        uint32_t DestRowPitch,
        uint32_t DestSlicePitch) = 0;
};

extern "C" {
    HRESULT WINAPI XGCreateTexture2DComputer(
        const XG_TEXTURE2D_DESC* pDesc,
        IXGTextureAddressComputer** ppComputer);

    HRESULT WINAPI XGComputeTexture2DLayout(
        const XG_TEXTURE2D_DESC* pDesc,
        XG_RESOURCE_LAYOUT* pLayout);
}