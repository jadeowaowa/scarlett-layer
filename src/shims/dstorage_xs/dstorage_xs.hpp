#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>
#include <windows.h>
#include <d3d12.h>
#include <unknwn.h>

enum DSTORAGE_PRIORITY : int8_t {
    DSTORAGE_PRIORITY_LOW      = -1,
    DSTORAGE_PRIORITY_NORMAL   = 0,
    DSTORAGE_PRIORITY_HIGH     = 1,
    DSTORAGE_PRIORITY_REALTIME = 2
};

enum DSTORAGE_REQUEST_SOURCE_TYPE : uint8_t {
    DSTORAGE_REQUEST_SOURCE_FILE   = 0,
    DSTORAGE_REQUEST_SOURCE_MEMORY = 1
};

enum DSTORAGE_REQUEST_DESTINATION_TYPE : uint8_t {
    DSTORAGE_REQUEST_DESTINATION_MEMORY = 0,
    DSTORAGE_REQUEST_DESTINATION_BUFFER = 1
};

enum DSTORAGE_COMPRESSION_FORMAT : uint8_t {
    DSTORAGE_COMPRESSION_FORMAT_NONE        = 0,
    DSTORAGE_COMPRESSION_FORMAT_ZLIB        = 1,
    DSTORAGE_COMPRESSION_FORMAT_BCPACK      = 2,
    DSTORAGE_COMPRESSION_FORMAT_SCARLETT_LZ = 3
};

struct DSTORAGE_CONFIGURATION {
    uint32_t NumSubmitThreads;
    int32_t  NumBuiltInCpuDecompressionThreads;
    bool     ForceMappingLayer;
    bool     DisableBypassIO;
    bool     DisableTelemetry;
};

struct DSTORAGE_QUEUE_DESC {
    DSTORAGE_REQUEST_SOURCE_TYPE SourceType;
    uint16_t                     Capacity;
    DSTORAGE_PRIORITY            Priority;
    const char*                  Name;
    void*                        Device;
};

struct DSTORAGE_REQUEST_OPTIONS {
    DSTORAGE_COMPRESSION_FORMAT       CompressionFormat;
    DSTORAGE_REQUEST_SOURCE_TYPE      SourceType;
    DSTORAGE_REQUEST_DESTINATION_TYPE DestinationType;
};

struct IDStorageFileX;

struct DSTORAGE_SOURCE_FILE {
    IDStorageFileX* Source;
    uint64_t        Offset;
    uint32_t        Size;
};

struct DSTORAGE_SOURCE_MEMORY {
    const void* Source;
    uint32_t    Size;
};

union DSTORAGE_SOURCE {
    DSTORAGE_SOURCE_MEMORY Memory;
    DSTORAGE_SOURCE_FILE   File;
};

struct DSTORAGE_DESTINATION_MEMORY {
    void*    Buffer;
    uint32_t Size;
};

union DSTORAGE_DESTINATION {
    DSTORAGE_DESTINATION_MEMORY Memory;
};

struct DSTORAGE_REQUEST {
    DSTORAGE_REQUEST_OPTIONS Options;
    DSTORAGE_SOURCE          Source;
    DSTORAGE_DESTINATION     Destination;
    uint32_t                 UncompressedSize;
    uint64_t                 CancellationTag;
    const char*              Name;
};

struct DSTORAGE_ERROR_RECORD {
    uint32_t FailureCount;
    HRESULT  FirstFailureHResult;
    char     RequestName[64];
};

inline constexpr GUID IID_IDStorageFactoryX =
    { 0x5c4a4101, 0x9b70, 0x4d1e, { 0xa1, 0x20, 0x00, 0x53, 0x43, 0x41, 0x52, 0x01 } };

inline constexpr GUID IID_IDStorageQueueX =
    { 0x5c4a4102, 0x9b70, 0x4d1e, { 0xa1, 0x20, 0x00, 0x53, 0x43, 0x41, 0x52, 0x02 } };

inline constexpr GUID IID_IDStorageFileX =
    { 0x5c4a4103, 0x9b70, 0x4d1e, { 0xa1, 0x20, 0x00, 0x53, 0x43, 0x41, 0x52, 0x03 } };

inline constexpr GUID IID_IDStorageStatusArrayX =
    { 0x5c4a4104, 0x9b70, 0x4d1e, { 0xa1, 0x20, 0x00, 0x53, 0x43, 0x41, 0x52, 0x04 } };

struct IDStorageFileX : public IUnknown {
    virtual void    STDMETHODCALLTYPE Close() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFileInformation(
        BY_HANDLE_FILE_INFORMATION* info) = 0;
};

struct IDStorageStatusArrayX : public IUnknown {
    virtual bool    STDMETHODCALLTYPE IsComplete(uint32_t index) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetHResult(uint32_t index) = 0;
};

struct IDStorageQueueX : public IUnknown {
    virtual void     STDMETHODCALLTYPE EnqueueRequest(const DSTORAGE_REQUEST* request) = 0;
    virtual void     STDMETHODCALLTYPE EnqueueStatus(
        IDStorageStatusArrayX* statusArray, uint32_t index) = 0;
    virtual void     STDMETHODCALLTYPE EnqueueSignal(
        ID3D12Fence* fence, uint64_t value) = 0;
    virtual void     STDMETHODCALLTYPE Submit() = 0;
    virtual void     STDMETHODCALLTYPE RetrieveErrorRecord(
        DSTORAGE_ERROR_RECORD* record) = 0;
    virtual uint32_t STDMETHODCALLTYPE GetPendingRequestCount() = 0;
};

struct IDStorageFactoryX : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateQueue(
        const DSTORAGE_QUEUE_DESC* desc,
        REFIID riid,
        void** ppv) = 0;
    virtual HRESULT STDMETHODCALLTYPE OpenFile(
        const wchar_t* path,
        REFIID riid,
        void** ppv) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateStatusArray(
        uint32_t capacity,
        const char* name,
        REFIID riid,
        void** ppv) = 0;
    virtual void    STDMETHODCALLTYPE SetDebugFlags(uint32_t flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetStagingBufferSize(uint32_t size) = 0;
};

extern "C" {
    HRESULT WINAPI DStorageGetFactory(REFIID riid, void** ppv);
    HRESULT WINAPI DStorageSetConfiguration(const DSTORAGE_CONFIGURATION* configuration);
}