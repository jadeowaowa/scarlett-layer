#pragma once

#include <cstddef>
#include <cstdint>

struct CURL;

struct curl_slist {
    char*       data;
    curl_slist* next;
};

using curl_write_callback = size_t (*)(char* ptr, size_t size, size_t nmemb, void* userdata);

enum CURLcode : int {
    CURLE_OK = 0,
    CURLE_UNSUPPORTED_PROTOCOL = 1,
    CURLE_COULDNT_CONNECT = 7,
    CURLE_OUT_OF_MEMORY = 27,
    CURLE_BAD_FUNCTION_ARGUMENT = 43
};

enum CURLoption : int {
    CURLOPT_WRITEDATA = 10001,
    CURLOPT_URL = 10002,
    CURLOPT_HTTPHEADER = 10023,
    CURLOPT_WRITEFUNCTION = 20011,
    CURLOPT_TIMEOUT = 13
};

enum CURLINFO : int {
    CURLINFO_RESPONSE_CODE = 0x200002
};

extern "C" {
    CURLcode    curl_global_init(int64_t flags);
    void        curl_global_cleanup(void);
    CURL*       curl_easy_init(void);
    void        curl_easy_cleanup(CURL* curl);
    void        curl_easy_reset(CURL* curl);
    CURLcode    curl_easy_setopt(CURL* curl, CURLoption option, ...);
    CURLcode    curl_easy_perform(CURL* curl);
    CURLcode    curl_easy_getinfo(CURL* curl, CURLINFO info, ...);
    const char* curl_easy_strerror(CURLcode code);
    curl_slist* curl_slist_append(curl_slist* list, const char* string);
    void        curl_slist_free_all(curl_slist* list);
    const char* curl_version(void);

    CURLcode    xcurl_register_mock_response(const char* urlPrefix, long statusCode, const char* body);
    void        xcurl_clear_mocks(void);
}