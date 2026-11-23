# 04 — Velocity Storage (`dstorage_xs`) & Audio Proxy (`xaudio2_9`)

## `dstorage_xs.dll`
**Files:** `src/shims/dstorage_xs/dstorage_xs.cpp` (1982 L), `src/shims/dstorage_xs/dstorage_xs.def` (4 L), `src/shims/dstorage_xs/dstorage_xs.hpp` (181 L)

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
- `DSTORAGE_REQUEST_DESTINATION_TYPE`: DSTORAGE_REQUEST_DESTINATION_MEMORY = 0, DSTORAGE_REQUEST_DESTINATION_BUFFER = 1, DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION = 2, DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES = 3
- `DSTORAGE_COMPRESSION_FORMAT`: DSTORAGE_COMPRESSION_FORMAT_NONE = 0, DSTORAGE_COMPRESSION_FORMAT_ZLIB = 1, DSTORAGE_COMPRESSION_FORMAT_BCPACK = 2, DSTORAGE_COMPRESSION_FORMAT_SCARLETT_LZ = 3

## `xaudio2_9.dll`
**Files:** `src/shims/xaudio2_9/xaudio2_9.cpp` (660 L), `src/shims/xaudio2_9/xaudio2_9.def` (4 L)

**DLL Exports (`.def`):** `XAudio2Create = ScarlettXAudio2Create @1`, `XAudio2CreateWithVersionInfo = ScarlettXAudio2CreateWithVersionInfo`

## `acphal.dll`
**Files:** `src/shims/acphal/acphal.cpp` (157 L), `src/shims/acphal/acphal.def` (11 L), `src/shims/acphal/acphal.hpp` (39 L)

**DLL Exports (`.def`):** `AcpHalCreate`, `AcpHalAllocateShapeContexts`, `AcpHalReleaseShapeContexts`, `ApuAlloc`, `ApuFree`, `ApuMapVirtualAddress`, `ApuIsVirtualAddressValid`, `ApuCreateHeap`, `ApuMapApuAddress`

**C Entrypoints:**
- `HRESULT WINAPI AcpHalCreate(void** acpHal)`
- `HRESULT WINAPI AcpHalAllocateShapeContexts(ACP_SHAPE_CONTEXT_COUNTS* ctx)`
- `HRESULT WINAPI AcpHalReleaseShapeContexts(void)`
- `HRESULT WINAPI ApuAlloc(void** virtualAddress, void* physicalAddress, uint32_t sizeInBytes, uint32_t alignmentInBytes)`
- `HRESULT WINAPI ApuFree(void* virtualAddress)`

## `libhttpclient.dll`
**Files:** `src/shims/libhttpclient/libhttpclient.cpp` (443 L), `src/shims/libhttpclient/libhttpclient.def` (45 L), `src/shims/libhttpclient/libhttpclient.hpp` (34 L)

**DLL Exports (`.def`):** `HCInitialize`, `HCCleanup`, `HCIsInitialized`, `HCGetLibVersion`, `HCHttpCallCreate`, `HCHttpCallCloseHandle`, `HCHttpCallRequestSetUrl`, `HCHttpCallRequestSetHeader`, `HCHttpCallRequestSetRequestBodyBytes`, `HCHttpCallRequestSetRequestBodyString`, `HCHttpCallRequestSetTimeout`, `HCHttpCallPerformAsync`, `HCHttpCallResponseGetStatusCode`, `HCHttpCallResponseGetNetworkErrorCode`, `HCHttpCallResponseGetResponseString`, `HCHttpCallResponseGetResponseBodyBytesSize`, `HCHttpCallResponseGetResponseBodyBytes`, `HCHttpCallResponseGetHeader`, `HCHttpCallRegisterMock`, `HCHttpCallClearMocks`, `HCCleanupAsync`, `HCHttpCallDuplicateHandle`, `HCHttpCallGetRequestUrl`, `HCHttpCallRequestGetHeaderAtIndex`, `HCHttpCallRequestGetNumHeaders`, `HCHttpCallRequestGetRequestBodyBytes`, `HCHttpCallRequestGetRetryAllowed`, `HCHttpCallRequestGetRetryCacheId`, `HCHttpCallRequestGetRetryDelay`, `HCHttpCallRequestGetTimeout`, `HCHttpCallRequestGetTimeoutWindow`, `HCHttpCallRequestGetUrl`, `HCHttpCallRequestSetRetryAllowed`, `HCHttpCallRequestSetRetryCacheId`, `HCHttpCallRequestSetRetryDelay`, `HCHttpCallRequestSetTimeoutWindow`, `HCHttpCallResponseGetHeaderAtIndex`, `HCHttpCallResponseGetNumHeaders`, `HCHttpCallResponseGetPlatformNetworkErrorMessage`, `HCHttpCallSetTracing`, `HCSettingsGetTraceLevel`, `HCTraceImplMessage`, `HCTraceSetEtwEnabled`

**C Entrypoints:**
- `HRESULT WINAPI HCInitialize(void* args)`
- `void WINAPI HCCleanup(void)`
- `uint32_t WINAPI HCIsInitialized(void)`
- `HRESULT WINAPI HCGetLibVersion(const char** version)`
- `HRESULT WINAPI HCHttpCallCreate(HCCallHandle* call)`
- `HRESULT WINAPI HCHttpCallCloseHandle(HCCallHandle call)`
- `HRESULT WINAPI HCHttpCallRequestSetUrl(HCCallHandle call, const char* method, const char* url)`
- `HRESULT WINAPI HCHttpCallRequestSetHeader(HCCallHandle call, const char* name, const char* value, uint32_t allowTracing)`
- `HRESULT WINAPI HCHttpCallRequestSetRequestBodyBytes(HCCallHandle call, const uint8_t* bodyBytes, uint32_t bodySize)`
- `HRESULT WINAPI HCHttpCallRequestSetRequestBodyString(HCCallHandle call, const char* requestBodyString)`
- `HRESULT WINAPI HCHttpCallRequestSetTimeout(HCCallHandle call, uint32_t timeoutInSeconds)`
- `HRESULT WINAPI HCHttpCallPerformAsync(HCCallHandle call, void* asyncBlock)`
- `HRESULT WINAPI HCHttpCallResponseGetStatusCode(HCCallHandle call, uint32_t* statusCode)`
- `HRESULT WINAPI HCHttpCallResponseGetNetworkErrorCode(HCCallHandle call, HRESULT* networkErrorCode, uint32_t* platformErrorCode)`
- `HRESULT WINAPI HCHttpCallResponseGetResponseString(HCCallHandle call, const char** responseString)`
- `HRESULT WINAPI HCHttpCallResponseGetResponseBodyBytesSize(HCCallHandle call, size_t* bufferSize)`
- `HRESULT WINAPI HCHttpCallResponseGetResponseBodyBytes(HCCallHandle call, size_t bufferSize, uint8_t* buffer, size_t* bufferUsed)`
- `HRESULT WINAPI HCHttpCallResponseGetHeader(HCCallHandle call, const char* headerName, const char** headerValue)`
- `HRESULT WINAPI HCHttpCallRegisterMock(const char* method, const char* urlPrefix, uint32_t statusCode, const char* responseBody)`
- `void WINAPI HCHttpCallClearMocks(void)`

## `xcurl.dll`
**Files:** `src/shims/xcurl/xcurl.cpp` (242 L), `src/shims/xcurl/xcurl.def` (24 L), `src/shims/xcurl/xcurl.hpp` (51 L)

**DLL Exports (`.def`):** `curl_global_init`, `curl_global_cleanup`, `curl_easy_init`, `curl_easy_cleanup`, `curl_easy_reset`, `curl_easy_setopt`, `curl_easy_perform`, `curl_easy_getinfo`, `curl_easy_strerror`, `curl_slist_append`, `curl_slist_free_all`, `curl_version`, `xcurl_register_mock_response`, `xcurl_clear_mocks`, `curl_multi_init`, `curl_multi_cleanup`, `curl_multi_add_handle`, `curl_multi_remove_handle`, `curl_multi_perform`, `curl_multi_wait`, `curl_multi_poll`, `curl_multi_info_read`

**C Entrypoints:**
- `CURLcode curl_global_init(int64_t flags)`
- `void curl_global_cleanup(void)`
- `CURL* curl_easy_init(void)`
- `void curl_easy_cleanup(CURL* curl)`
- `void curl_easy_reset(CURL* curl)`
- `CURLcode curl_easy_setopt(CURL* curl, CURLoption option, ...)`
- `CURLcode curl_easy_perform(CURL* curl)`
- `CURLcode curl_easy_getinfo(CURL* curl, CURLINFO info, ...)`
- `const char* curl_easy_strerror(CURLcode code)`
- `curl_slist* curl_slist_append(curl_slist* list, const char* string)`
- `void curl_slist_free_all(curl_slist* list)`
- `const char* curl_version(void)`
- `CURLcode xcurl_register_mock_response(const char* urlPrefix, long statusCode, const char* body)`
- `void xcurl_clear_mocks(void)`

**Key Enums:**
- `CURLcode`: CURLE_OK = 0, CURLE_UNSUPPORTED_PROTOCOL = 1, CURLE_COULDNT_CONNECT = 7, CURLE_OUT_OF_MEMORY = 27, CURLE_BAD_FUNCTION_ARGUMENT = 43
- `CURLoption`: CURLOPT_WRITEDATA = 10001, CURLOPT_URL = 10002, CURLOPT_HTTPHEADER = 10023, CURLOPT_WRITEFUNCTION = 20011, CURLOPT_TIMEOUT = 13
- `CURLINFO`: CURLINFO_RESPONSE_CODE = 0x200002
