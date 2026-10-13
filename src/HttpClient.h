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
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <curl/curl.h>

#include "IO/ReadBufferFromHttpResponse.h"
#include "Status.h"

namespace firebolt::adbc
{

struct FireboltConnection;

struct HttpResponse
{
    // Why no complete response arrived (connection, TLS, timeout, …); OK when one
    // did, whatever its HTTP status.
    Status transfer;
    long http_code = 0;
    std::vector<uint8_t> body;
    // The body of a non-2xx response, or the FB2 mode's explanation of it.
    std::string error_message;
    std::unordered_map<std::string, std::string> update_params;
    std::vector<std::string> remove_params;
    bool reset_session = false;
    // A new endpoint for the connection, set only by FB2 mode (Firebolt-Update-Endpoint).
    std::string new_endpoint;

    bool isSuccess() const { return transfer.ok() && http_code >= 200 && http_code < 300; }
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

// A request whose response has started: the status and headers in `response`,
// and, when it succeeded, the body still to be read.
struct StartedResponse
{
    HttpResponse response;
    std::unique_ptr<ReadBufferFromHttpResponse> body;
};

class HttpClient
{
public:
    // Borrows the owning connection (lifetime guaranteed by its unique_ptr<HttpClient>);
    // token, url, database, and timeout are read live on every request — no internal copies.
    explicit HttpClient(const FireboltConnection & conn);

    HttpClient(const HttpClient &) = delete;
    HttpClient & operator=(const HttpClient &) = delete;

    // SELECT / DDL, returning once the response starts.  The body is read as the
    // caller asks for it, and may outlive this client and its connection.  Until
    // it is read to the end or destroyed, resultStreamOpen() is true and no other
    // request may be sent.
    StartedResponse startQuery(const std::string & sql, const std::unordered_map<std::string, std::string> & session_params);

    // SELECT / DDL, with the whole body read into the response.
    HttpResponse executeQuery(const std::string & sql, const std::unordered_map<std::string, std::string> & session_params);

    // INSERT with Arrow IPC payload.  The byte buffer is borrowed for the
    // duration of the call — libcurl's curl_mime_data copies the bytes into
    // its own storage, so no ownership transfer is needed.
    HttpResponse executeInsert(
        const std::string & sql,
        const std::vector<uint8_t> & arrow_ipc_bytes,
        const std::unordered_map<std::string, std::string> & session_params);

    // Whether a response from startQuery still holds the connection's handles.  A
    // connection sends one request at a time, so every request made meanwhile
    // fails with ADBC_STATUS_INVALID_STATE, without being sent.
    bool resultStreamOpen() const;

    // Abandon the response with this id, if it still holds the handles: a new
    // execution of a statement replaces its previous result (ADBC).
    void abandonResult(uint64_t response_id);

private:
    std::shared_ptr<CurlHandles> handles = std::make_shared<CurlHandles>();
    const FireboltConnection & fb_conn;

    std::string buildUrl(const std::unordered_map<std::string, std::string> & session_params) const;
    static curl_slist * buildAuthHeader(const std::string & token);
    void applyTlsOptions() const;
    // The response to a request made while a result stream is open.
    static StartedResponse refuseWhileResultStreamOpen();
    // The easy handle, reset for a new request; nullptr while a result stream holds
    // it.  Every request takes the handle here.
    CURL * takeHandle();
    // Sends the request configured on the easy handle and waits for its response
    // to start.  In FB2 SaaS (Legacy) mode a 401 re-authenticates and retries once.
    StartedResponse sendAndAwaitResponseStart(const char * content_type, long idle_timeout_sec);
    void parseResponseHeaders(HttpResponse & resp) const;
    // Reads the rest of a started response into its `body`.
    static HttpResponse readWholeResponse(StartedResponse started);
};

} // namespace firebolt::adbc
