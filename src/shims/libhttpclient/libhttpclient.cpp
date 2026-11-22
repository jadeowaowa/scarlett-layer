#include "libhttpclient.hpp"

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct HC_CALL {
    std::string method{"GET"};
    std::string url;
    std::unordered_map<std::string, std::string> reqHeaders;
    std::vector<uint8_t> reqBody;
    uint32_t timeoutSec{30};

    uint32_t statusCode{0};
    HRESULT  networkError{S_OK};
    std::string responseBody;
    std::unordered_map<std::string, std::string> respHeaders;
};

namespace {

struct MockRule {
    std::string method;
    std::string urlPrefix;
    uint32_t    statusCode;
    std::string body;
};

std::mutex g_hcMtx;
bool g_hcInit = false;
std::unordered_map<HC_CALL*, std::unique_ptr<HC_CALL>> g_calls;
std::vector<MockRule> g_mocks;

} // namespace

extern "C" {

HRESULT WINAPI HCInitialize(void* /*args*/) {
    std::lock_guard<std::mutex> lock(g_hcMtx);
    g_hcInit = true;
    return S_OK;
}

void WINAPI HCCleanup(void) {
    std::lock_guard<std::mutex> lock(g_hcMtx);
    g_calls.clear();
    g_mocks.clear();
    g_hcInit = false;
}

uint32_t WINAPI HCIsInitialized(void) {
    std::lock_guard<std::mutex> lock(g_hcMtx);
    return g_hcInit ? 1u : 0u;
}

HRESULT WINAPI HCGetLibVersion(const char** version) {
    if (!version) return E_INVALIDARG;
    *version = "1.0.0-scarlett-layer";
    return S_OK;
}

HRESULT WINAPI HCHttpCallCreate(HCCallHandle* call) {
    if (!call) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_hcMtx);
    auto obj = std::make_unique<HC_CALL>();
    HC_CALL* raw = obj.get();
    g_calls.emplace(raw, std::move(obj));
    *call = raw;
    return S_OK;
}

HRESULT WINAPI HCHttpCallCloseHandle(HCCallHandle call) {
    if (!call) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_hcMtx);
    return g_calls.erase(call) > 0 ? S_OK : E_INVALIDARG;
}

HRESULT WINAPI HCHttpCallRequestSetUrl(HCCallHandle call, const char* method, const char* url) {
    if (!call || !method || !url) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_hcMtx);
    if (!g_calls.contains(call)) return E_INVALIDARG;
    call->method = method;
    call->url = url;
    return S_OK;
}

HRESULT WINAPI HCHttpCallRequestSetHeader(
    HCCallHandle call, const char* name, const char* value, uint32_t /*allowTracing*/) {
    if (!call || !name || !value) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_hcMtx);
    if (!g_calls.contains(call)) return E_INVALIDARG;
    call->reqHeaders[name] = value;
    return S_OK;
}

HRESULT WINAPI HCHttpCallRequestSetRequestBodyBytes(
    HCCallHandle call, const uint8_t* bodyBytes, uint32_t bodySize) {
    if (!call) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_hcMtx);
    if (!g_calls.contains(call)) return E_INVALIDARG;
    if (bodyBytes && bodySize > 0) {
        call->reqBody.assign(bodyBytes, bodyBytes + bodySize);
    } else {
        call->reqBody.clear();
    }
    return S_OK;
}

HRESULT WINAPI HCHttpCallRequestSetRequestBodyString(
    HCCallHandle call, const char* requestBodyString) {
    if (!call || !requestBodyString) return E_INVALIDARG;
    const size_t len = std::strlen(requestBodyString);
    return HCHttpCallRequestSetRequestBodyBytes(
        call,
        reinterpret_cast<const uint8_t*>(requestBodyString),
        static_cast<uint32_t>(len));
}

HRESULT WINAPI HCHttpCallRequestSetTimeout(HCCallHandle call, uint32_t timeoutInSeconds) {
    if (!call) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_hcMtx);
    if (!g_calls.contains(call)) return E_INVALIDARG;
    call->timeoutSec = timeoutInSeconds;
    return S_OK;
}

HRESULT WINAPI HCHttpCallPerformAsync(HCCallHandle call, void* /*asyncBlock*/) {
    if (!call) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_hcMtx);
    if (!g_calls.contains(call)) return E_INVALIDARG;

    bool matched = false;
    for (const auto& rule : g_mocks) {
        const bool methodOk = rule.method.empty() || rule.method == "*" || rule.method == call->method;
        const bool urlOk = call->url.rfind(rule.urlPrefix, 0) == 0;
        if (methodOk && urlOk) {
            call->statusCode = rule.statusCode;
            call->responseBody = rule.body;
            call->networkError = S_OK;
            call->respHeaders["Content-Type"] = "application/json";
            matched = true;
            break;
        }
    }
    if (!matched) {
        call->statusCode = 503u;
        call->responseBody.clear();
        call->networkError = E_FAIL;
    }
    return S_OK;
}

HRESULT WINAPI HCHttpCallResponseGetStatusCode(HCCallHandle call, uint32_t* statusCode) {
    if (!call || !statusCode) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_hcMtx);
    if (!g_calls.contains(call)) return E_INVALIDARG;
    *statusCode = call->statusCode;
    return S_OK;
}

HRESULT WINAPI HCHttpCallResponseGetNetworkErrorCode(
    HCCallHandle call, HRESULT* networkErrorCode, uint32_t* platformErrorCode) {
    if (!call || !networkErrorCode) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_hcMtx);
    if (!g_calls.contains(call)) return E_INVALIDARG;
    *networkErrorCode = call->networkError;
    if (platformErrorCode) *platformErrorCode = 0u;
    return S_OK;
}

HRESULT WINAPI HCHttpCallResponseGetResponseString(HCCallHandle call, const char** responseString) {
    if (!call || !responseString) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_hcMtx);
    if (!g_calls.contains(call)) return E_INVALIDARG;
    *responseString = call->responseBody.c_str();
    return S_OK;
}

HRESULT WINAPI HCHttpCallResponseGetResponseBodyBytesSize(HCCallHandle call, size_t* bufferSize) {
    if (!call || !bufferSize) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_hcMtx);
    if (!g_calls.contains(call)) return E_INVALIDARG;
    *bufferSize = call->responseBody.size();
    return S_OK;
}

HRESULT WINAPI HCHttpCallResponseGetResponseBodyBytes(
    HCCallHandle call, size_t bufferSize, uint8_t* buffer, size_t* bufferUsed) {
    if (!call || !buffer) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_hcMtx);
    if (!g_calls.contains(call)) return E_INVALIDARG;
    const size_t n = call->responseBody.size();
    if (bufferSize < n) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    if (n > 0) std::memcpy(buffer, call->responseBody.data(), n);
    if (bufferUsed) *bufferUsed = n;
    return S_OK;
}

HRESULT WINAPI HCHttpCallResponseGetHeader(
    HCCallHandle call, const char* headerName, const char** headerValue) {
    if (!call || !headerName || !headerValue) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_hcMtx);
    if (!g_calls.contains(call)) return E_INVALIDARG;
    auto it = call->respHeaders.find(headerName);
    *headerValue = (it != call->respHeaders.end()) ? it->second.c_str() : nullptr;
    return S_OK;
}

HRESULT WINAPI HCHttpCallRegisterMock(
    const char* method, const char* urlPrefix, uint32_t statusCode, const char* responseBody) {
    if (!urlPrefix) return E_INVALIDARG;
    std::lock_guard<std::mutex> lock(g_hcMtx);
    g_mocks.push_back(MockRule{
        method ? method : "*",
        urlPrefix,
        statusCode,
        responseBody ? responseBody : ""
    });
    return S_OK;
}

void WINAPI HCHttpCallClearMocks(void) {
    std::lock_guard<std::mutex> lock(g_hcMtx);
    g_mocks.clear();
}

} // extern "C"