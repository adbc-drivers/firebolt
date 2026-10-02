// Copyright (c) 2026 ADBC Drivers Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//         http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <curl/curl.h>

namespace firebolt::adbc
{

struct FireboltConnection;

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

// Apply session-parameter mutations advertised in a response (Firebolt-Update-Parameters,
// Firebolt-Remove-Parameters, Firebolt-Reset-Session) to the supplied session_params map
// — but ONLY when the response succeeded.
//
// On non-2xx responses the server is by definition either not the legitimate Firebolt
// endpoint (e.g. an injecting MITM, a generic-error proxy, a 502 from a sidecar) or
// rejected the request, so its session-state hints are not trustworthy.  Applying
// them unconditionally let a single 5xx response rebind `database=` for the rest of
// the connection's life.
//
// Returns true when at least one mutation was applied.
//
// Defined inline so unit tests can call it without re-exporting symbols from the
// hidden-visibility shared library.
inline bool applySessionUpdatesIfSuccess(std::unordered_map<std::string, std::string> & session_params, const HttpResponse & resp)
{
    if (!resp.isSuccess())
        return false;
    bool changed = false;
    if (resp.reset_session)
    {
        if (!session_params.empty())
            changed = true;
        session_params.clear();
    }
    for (const auto & kv : resp.update_params)
    {
        auto it = session_params.find(kv.first);
        if (it == session_params.end() || it->second != kv.second)
            changed = true;
        session_params[kv.first] = kv.second;
    }
    for (const auto & k : resp.remove_params)
    {
        if (session_params.erase(k) > 0)
            changed = true;
    }
    return changed;
}

class HttpClient
{
public:
    // Borrows the owning connection (lifetime guaranteed by its unique_ptr<HttpClient>);
    // token, url, database, and timeout are read live on every request — no internal copies.
    explicit HttpClient(const FireboltConnection & conn);
    ~HttpClient();

    HttpClient(const HttpClient &) = delete;
    HttpClient & operator=(const HttpClient &) = delete;

    // SELECT / DDL
    HttpResponse executeQuery(const std::string & sql, const std::unordered_map<std::string, std::string> & session_params);

    // INSERT with Arrow IPC payload.  The byte buffer is borrowed for the
    // duration of the call — libcurl's curl_mime_data copies the bytes into
    // its own storage, so no ownership transfer is needed.
    HttpResponse executeInsert(
        const std::string & sql,
        const std::vector<uint8_t> & arrow_ipc_bytes,
        const std::unordered_map<std::string, std::string> & session_params);

private:
    CURL * handle = nullptr;
    const FireboltConnection & fb_conn;

    std::string buildUrl(const std::unordered_map<std::string, std::string> & session_params) const;
    curl_slist * buildAuthHeader() const;
    void applyTlsOptions() const;
    void parseResponseHeaders(HttpResponse & resp) const;

    static size_t writeBodyCallback(char * ptr, size_t size, size_t nmemb, void * userdata);
};

} // namespace firebolt::adbc
