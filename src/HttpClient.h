#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <curl/curl.h>

namespace firebolt::adbc
{

struct FireboltDatabase;

struct HttpResponse
{
    CURLcode curl_code = CURLE_OK;
    long http_code = 0;
    std::vector<uint8_t> body;
    std::string error_message;
    std::unordered_map<std::string, std::string> update_params;
    std::vector<std::string> remove_params;
    bool reset_session = false;

    bool isSuccess() const { return curl_code == CURLE_OK && http_code >= 200 && http_code < 300; }
};

class HttpClient
{
public:
    explicit HttpClient(const FireboltDatabase & db);
    ~HttpClient();

    HttpClient(const HttpClient &) = delete;
    HttpClient & operator=(const HttpClient &) = delete;

    // SELECT / DDL
    HttpResponse executeQuery(const std::string & sql, const std::unordered_map<std::string, std::string> & session_params);

    // INSERT with Arrow IPC payload
    HttpResponse executeInsert(
        const std::string & sql, std::vector<uint8_t> arrow_ipc_bytes, const std::unordered_map<std::string, std::string> & session_params);

private:
    CURL * handle_ = nullptr;
    const FireboltDatabase & db_;
    std::string raw_headers_;

    std::string buildUrl(const std::unordered_map<std::string, std::string> & session_params) const;
    curl_slist * buildAuthHeader() const;
    void parseResponseHeaders(HttpResponse & resp) const;
    void resetResponseState();

    static size_t writeBodyCallback(char * ptr, size_t size, size_t nmemb, void * userdata);
    static size_t writeHeaderCallback(char * ptr, size_t size, size_t nmemb, void * userdata);
};

} // namespace firebolt::adbc
