#pragma once

#include <cstddef>
#include <cstdint>
#include <windows.h>

struct HC_CALL;
using HCCallHandle = HC_CALL*;

extern "C" {
    HRESULT WINAPI HCInitialize(void* args);
    void    WINAPI HCCleanup(void);
    uint32_t WINAPI HCIsInitialized(void);
    HRESULT WINAPI HCGetLibVersion(const char** version);

    HRESULT WINAPI HCHttpCallCreate(HCCallHandle* call);
    HRESULT WINAPI HCHttpCallCloseHandle(HCCallHandle call);
    HRESULT WINAPI HCHttpCallRequestSetUrl(HCCallHandle call, const char* method, const char* url);
    HRESULT WINAPI HCHttpCallRequestSetHeader(HCCallHandle call, const char* name, const char* value, uint32_t allowTracing);
    HRESULT WINAPI HCHttpCallRequestSetRequestBodyBytes(HCCallHandle call, const uint8_t* bodyBytes, uint32_t bodySize);
    HRESULT WINAPI HCHttpCallRequestSetTimeout(HCCallHandle call, uint32_t timeoutInSeconds);
    HRESULT WINAPI HCHttpCallPerformAsync(HCCallHandle call, void* asyncBlock);

    HRESULT WINAPI HCHttpCallResponseGetStatusCode(HCCallHandle call, uint32_t* statusCode);
    HRESULT WINAPI HCHttpCallResponseGetNetworkErrorCode(HCCallHandle call, HRESULT* networkErrorCode, uint32_t* platformErrorCode);
    HRESULT WINAPI HCHttpCallResponseGetResponseString(HCCallHandle call, const char** responseString);
    HRESULT WINAPI HCHttpCallResponseGetResponseBodyBytesSize(HCCallHandle call, size_t* bufferSize);
    HRESULT WINAPI HCHttpCallResponseGetResponseBodyBytes(HCCallHandle call, size_t bufferSize, uint8_t* buffer, size_t* bufferUsed);
    HRESULT WINAPI HCHttpCallResponseGetHeader(HCCallHandle call, const char* headerName, const char** headerValue);

    HRESULT WINAPI HCHttpCallRegisterMock(const char* method, const char* urlPrefix, uint32_t statusCode, const char* responseBody);
    void    WINAPI HCHttpCallClearMocks(void);
}