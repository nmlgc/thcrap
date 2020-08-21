#include "http_curl.h"

HttpHandle::HttpHandle()
    : curl(curl_easy_init())
{}

HttpHandle::HttpHandle(HttpHandle&& other)
    : curl(other.curl)
{
    other.curl = nullptr;
}

HttpHandle::~HttpHandle()
{
    if (this->curl) {
        curl_easy_cleanup(this->curl);
    }
}

size_t HttpHandle::writeCallbackStatic(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto callback = *static_cast<std::function<size_t(const uint8_t*, size_t)>*>(userdata);
    return callback(reinterpret_cast<const uint8_t*>(ptr), size * nmemb);
};

int HttpHandle::progressCallbackStatic(void *userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t /* ultotal */, curl_off_t /* ulnow */)
{
    auto callback = *static_cast<std::function<bool(size_t, size_t)>*>(userdata);
    if (callback(dlnow, dltotal)) {
        return 0;
    }
    else {
        return -1;
    }
};

bool HttpHandle::download(const std::string& url, std::function<size_t(const uint8_t*, size_t)> writeCallback, std::function<bool(size_t, size_t)> progressCallback)
{
    curl_easy_setopt(this->curl, CURLOPT_WRITEFUNCTION, HttpHandle::writeCallbackStatic);
    curl_easy_setopt(this->curl, CURLOPT_WRITEDATA, &writeCallback);

    curl_easy_setopt(this->curl, CURLOPT_NOPROGRESS, 0);
    curl_easy_setopt(this->curl, CURLOPT_XFERINFOFUNCTION, HttpHandle::progressCallbackStatic);
    curl_easy_setopt(this->curl, CURLOPT_XFERINFODATA, &progressCallback);

    char errbuf[CURL_ERROR_SIZE];
    curl_easy_setopt(this->curl, CURLOPT_ERRORBUFFER, errbuf);
    errbuf[0] = 0;
    curl_easy_setopt(this->curl, CURLOPT_URL, url.c_str());

    CURLcode res = curl_easy_perform(this->curl);

    curl_easy_setopt(this->curl, CURLOPT_WRITEFUNCTION, nullptr);
    curl_easy_setopt(this->curl, CURLOPT_WRITEDATA, nullptr);

    std::string error;
    if (res != CURLE_OK) {
        if (errbuf[0]) {
            printf("%s: %s\n", url.c_str(), errbuf);
        }
        else {
            printf("%s: %s\n", url.c_str(), curl_easy_strerror(res));
        }
        curl_easy_setopt(this->curl, CURLOPT_ERRORBUFFER, nullptr);
        return false;
    }
    curl_easy_setopt(this->curl, CURLOPT_ERRORBUFFER, nullptr);

    long response_code = 0;
    curl_easy_getinfo(this->curl, CURLINFO_RESPONSE_CODE, &response_code);
    if (response_code != 200) {
        printf("%s: HTTP error code %ld\n", url.c_str(), response_code);
        return false;
    }

    return true;
}