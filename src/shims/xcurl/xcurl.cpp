#include "xcurl.hpp"

#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct CURL {
    std::string         url;
    curl_write_callback writeCb{nullptr};
    void*               writeData{nullptr};
    long                timeoutSec{30};
    long                lastStatusCode{0};
};

namespace {

struct CurlMock {
    std::string urlPrefix;
    long        statusCode;
    std::string body;
};

std::mutex g_curlMtx;
bool g_curlInit = false;
std::unordered_map<CURL*, std::unique_ptr<CURL>> g_handles;
std::vector<CurlMock> g_curlMocks;

} // namespace

extern "C" {

CURLcode curl_global_init(int64_t /*flags*/) {
    std::lock_guard<std::mutex> lock(g_curlMtx);
    g_curlInit = true;
    return CURLE_OK;
}

void curl_global_cleanup(void) {
    std::lock_guard<std::mutex> lock(g_curlMtx);
    g_handles.clear();
    g_curlMocks.clear();
    g_curlInit = false;
}

CURL* curl_easy_init(void) {
    std::lock_guard<std::mutex> lock(g_curlMtx);
    auto obj = std::make_unique<CURL>();
    CURL* raw = obj.get();
    g_handles.emplace(raw, std::move(obj));
    return raw;
}

void curl_easy_cleanup(CURL* curl) {
    if (!curl) return;
    std::lock_guard<std::mutex> lock(g_curlMtx);
    g_handles.erase(curl);
}

void curl_easy_reset(CURL* curl) {
    if (!curl) return;
    std::lock_guard<std::mutex> lock(g_curlMtx);
    if (!g_handles.contains(curl)) return;
    *curl = CURL{};
}

CURLcode curl_easy_setopt(CURL* curl, CURLoption option, ...) {
    if (!curl) return CURLE_BAD_FUNCTION_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_curlMtx);
    if (!g_handles.contains(curl)) return CURLE_BAD_FUNCTION_ARGUMENT;

    va_list args;
    va_start(args, option);
    switch (option) {
        case CURLOPT_URL: {
            const char* url = va_arg(args, const char*);
            curl->url = url ? url : "";
            break;
        }
        case CURLOPT_WRITEFUNCTION:
            curl->writeCb = va_arg(args, curl_write_callback);
            break;
        case CURLOPT_WRITEDATA:
            curl->writeData = va_arg(args, void*);
            break;
        case CURLOPT_TIMEOUT:
            curl->timeoutSec = va_arg(args, long);
            break;
        case CURLOPT_HTTPHEADER:
            (void)va_arg(args, curl_slist*);
            break;
        default:
            break;
    }
    va_end(args);
    return CURLE_OK;
}

CURLcode curl_easy_perform(CURL* curl) {
    if (!curl) return CURLE_BAD_FUNCTION_ARGUMENT;

    curl_write_callback cb = nullptr;
    void* userdata = nullptr;
    std::string body;
    bool matched = false;

    {
        std::lock_guard<std::mutex> lock(g_curlMtx);
        if (!g_handles.contains(curl)) return CURLE_BAD_FUNCTION_ARGUMENT;
        cb = curl->writeCb;
        userdata = curl->writeData;
        for (const auto& m : g_curlMocks) {
            if (curl->url.rfind(m.urlPrefix, 0) == 0) {
                curl->lastStatusCode = m.statusCode;
                body = m.body;
                matched = true;
                break;
            }
        }
        if (!matched) {
            curl->lastStatusCode = 0;
            return CURLE_COULDNT_CONNECT;
        }
    }

    if (cb && !body.empty()) {
        cb(body.data(), 1, body.size(), userdata);
    }
    return CURLE_OK;
}

CURLcode curl_easy_getinfo(CURL* curl, CURLINFO info, ...) {
    if (!curl) return CURLE_BAD_FUNCTION_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_curlMtx);
    if (!g_handles.contains(curl)) return CURLE_BAD_FUNCTION_ARGUMENT;

    va_list args;
    va_start(args, info);
    if (info == CURLINFO_RESPONSE_CODE) {
        long* out = va_arg(args, long*);
        if (out) *out = curl->lastStatusCode;
    }
    va_end(args);
    return CURLE_OK;
}

const char* curl_easy_strerror(CURLcode code) {
    switch (code) {
        case CURLE_OK:                    return "No error";
        case CURLE_UNSUPPORTED_PROTOCOL:  return "Unsupported protocol";
        case CURLE_COULDNT_CONNECT:       return "Couldn't connect to server";
        case CURLE_OUT_OF_MEMORY:         return "Out of memory";
        case CURLE_BAD_FUNCTION_ARGUMENT: return "Bad function argument";
        default:                          return "Unknown error";
    }
}

curl_slist* curl_slist_append(curl_slist* list, const char* string) {
    if (!string) return list;
    auto* node = static_cast<curl_slist*>(std::malloc(sizeof(curl_slist)));
    if (!node) return nullptr;
    const size_t len = std::strlen(string) + 1;
    node->data = static_cast<char*>(std::malloc(len));
    if (!node->data) {
        std::free(node);
        return nullptr;
    }
    std::memcpy(node->data, string, len);
    node->next = nullptr;
    if (!list) return node;
    curl_slist* tail = list;
    while (tail->next) tail = tail->next;
    tail->next = node;
    return list;
}

void curl_slist_free_all(curl_slist* list) {
    while (list) {
        curl_slist* next = list->next;
        std::free(list->data);
        std::free(list);
        list = next;
    }
}

const char* curl_version(void) {
    return "XCurl-scarlett-layer/1.0";
}

CURLcode xcurl_register_mock_response(const char* urlPrefix, long statusCode, const char* body) {
    if (!urlPrefix) return CURLE_BAD_FUNCTION_ARGUMENT;
    std::lock_guard<std::mutex> lock(g_curlMtx);
    g_curlMocks.push_back(CurlMock{ urlPrefix, statusCode, body ? body : "" });
    return CURLE_OK;
}

void xcurl_clear_mocks(void) {
    std::lock_guard<std::mutex> lock(g_curlMtx);
    g_curlMocks.clear();
}

} // extern "C"