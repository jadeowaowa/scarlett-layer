# 04 â€” Velocity Storage (`dstorage_xs`) & Audio Proxy (`xaudio2_9`)

## `dstorage_xs.dll`
**Files:** `src/shims/dstorage_xs/dstorage_xs.cpp` (1104 L), `src/shims/dstorage_xs/dstorage_xs.def` (4 L), `src/shims/dstorage_xs/dstorage_xs.hpp` (174 L)

**DLL Exports (`.def`):** `DStorageGetFactory`, `DStorageSetConfiguration`

**C Entrypoints:**
- `HRESULT WINAPI DStorageGetFactory(REFIID riid, void** ppv)`
- `HRESULT WINAPI DStorageSetConfiguration(const DSTORAGE_CONFIGURATION* configuration)`

**COM Interface `IDStorageFileX : IUnknown`:**
- `[03]` `void Close()`
- `[04]` `HRESULT GetFileInformation( BY_HANDLE_FILE_INFORMATION* info)`

**COM Interface `IDStorageStatusArrayX : IUnknown`:**
- `[03]` `bool IsComplete(uint32_t index)`
- `[04]` `HRESULT GetHResult(uint32_t index)`

**COM Interface `IDStorageQueueX : IUnknown`:**
- `[03]` `void EnqueueRequest(const DSTORAGE_REQUEST* request)`
- `[04]` `void EnqueueStatus( IDStorageStatusArrayX* statusArray, uint32_t index)`
- `[05]` `void EnqueueSignal( ID3D12Fence* fence, uint64_t value)`
- `[06]` `void Submit()`
- `[07]` `void RetrieveErrorRecord( DSTORAGE_ERROR_RECORD* record)`
- `[08]` `uint32_t GetPendingRequestCount()`

**COM Interface `IDStorageFactoryX : IUnknown`:**
- `[03]` `HRESULT CreateQueue( const DSTORAGE_QUEUE_DESC* desc, REFIID riid, void** ppv)`
- `[04]` `HRESULT OpenFile( const wchar_t* path, REFIID riid, void** ppv)`
- `[05]` `HRESULT CreateStatusArray( uint32_t capacity, const char* name, REFIID riid, void** ppv)`
- `[06]` `void SetDebugFlags(uint32_t flags)`
- `[07]` `HRESULT SetStagingBufferSize(uint32_t size)`

**Key Enums:**
- `DSTORAGE_PRIORITY`: DSTORAGE_PRIORITY_LOW = -1, DSTORAGE_PRIORITY_NORMAL = 0, DSTORAGE_PRIORITY_HIGH = 1, DSTORAGE_PRIORITY_REALTIME = 2
- `DSTORAGE_REQUEST_SOURCE_TYPE`: DSTORAGE_REQUEST_SOURCE_FILE = 0, DSTORAGE_REQUEST_SOURCE_MEMORY = 1
- `DSTORAGE_REQUEST_DESTINATION_TYPE`: DSTORAGE_REQUEST_DESTINATION_MEMORY = 0, DSTORAGE_REQUEST_DESTINATION_BUFFER = 1, DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION = 2
- `DSTORAGE_COMPRESSION_FORMAT`: DSTORAGE_COMPRESSION_FORMAT_NONE = 0, DSTORAGE_COMPRESSION_FORMAT_ZLIB = 1, DSTORAGE_COMPRESSION_FORMAT_BCPACK = 2, DSTORAGE_COMPRESSION_FORMAT_SCARLETT_LZ = 3

## `xaudio2_9.dll`
**Files:** `src/shims/xaudio2_9/xaudio2_9.cpp` (652 L), `src/shims/xaudio2_9/xaudio2_9.def` (3 L)

**DLL Exports (`.def`):** `XAudio2Create = ScarlettXAudio2Create`
