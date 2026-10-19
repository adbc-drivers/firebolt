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

#include "HttpClient.h"
#include "FireboltAdbcConnection.h"
#include "FireboltAdbcDatabase.h"
#include "HttpHeaderParse.h"
#include "IO/ReadBufferFromMemory.h"
#include "IO/ReadHelpers.h"
#include "fb2/Fb2LegacyMode.h"

#include <cstdio>
#include <cstring>

#include <curl/curl.h>

namespace firebolt::adbc
{

namespace
{

    std::string urlEncode(CURL * handle, const std::string & s)
    {
        char * encoded = curl_easy_escape(handle, s.c_str(), static_cast<int>(s.size()));
        std::string result(encoded ? encoded : s);
        if (encoded)
            curl_free(encoded);
        return result;
    }

    // libcurl's read and seek callbacks for an upload part held in a ReadBufferFromMemory.
    // Seeking to the start lets libcurl send the request again (the FB2 401 retry).
    size_t readUploadPart(char * buffer, size_t size, size_t nitems, void * payload)
    {
        size_t bytes_read = 0;
        static_cast<ReadBufferFromMemory *>(payload)->read(reinterpret_cast<uint8_t *>(buffer), size * nitems, bytes_read);
        return bytes_read;
    }

    int seekUploadPart(void * payload, curl_off_t offset, int origin)
    {
        if (origin != SEEK_SET)
            return CURL_SEEKFUNC_CANTSEEK; // libcurl falls back to reading forward
        if (offset < 0 || !static_cast<ReadBufferFromMemory *>(payload)->seek(static_cast<size_t>(offset)))
            return CURL_SEEKFUNC_FAIL;
        return CURL_SEEKFUNC_OK;
    }

} // namespace

HttpClient::HttpClient(const FireboltConnection & conn) : fb_conn(conn)
{
}

std::string HttpClient::buildUrl(const std::unordered_map<std::string, std::string> & session_params) const
{
    // Use the endpoint URL verbatim — do not append a trailing slash so that
    // paths specified by the user (e.g. "http://host/query") are preserved.
    std::string url = fb_conn.url;

    bool first = (url.find('?') == std::string::npos);
    auto add_param = [&](const std::string & k, const std::string & v) {
        url += (first ? '?' : '&');
        first = false;
        url += k + "=" + urlEncode(handles->easy, v);
    };

    if (!fb_conn.db->database.empty())
        add_param("database", fb_conn.db->database);

    for (const auto & [k, v] : session_params)
        add_param(k, v);

    return url;
}

curl_slist * HttpClient::buildAuthHeader(const std::string & token)
{
    curl_slist * headers = nullptr;
    if (!token.empty())
    {
        std::string auth = "Authorization: Bearer " + token;
        headers = curl_slist_append(headers, auth.c_str());
    }
    headers = curl_slist_append(headers, "Firebolt-Protocol-Version: 2.4");
    return headers;
}

void HttpClient::applyTlsOptions() const
{
    // Peer and host verification are libcurl's defaults and stay on; this only
    // says which CA bundle backs them.  Empty for http://.
    if (!fb_conn.db->ca_bundle_path.empty())
        curl_easy_setopt(handles->easy, CURLOPT_CAINFO, fb_conn.db->ca_bundle_path.c_str());
}

void HttpClient::parseResponseHeaders(HttpResponse & resp) const
{
    resp.reset_session = shouldResetSession(handles->easy);
    resp.update_params = parseUpdateParameters(handles->easy);
    resp.remove_params = parseRemoveParameters(handles->easy);

    // FB2 SaaS (Legacy) mode hook: Firebolt-Update-Endpoint and FB2 error messages.
    if (fb_conn.fb2)
    {
        auto effect = fb_conn.fb2->onResponse(handles->easy, resp.http_code, resp.isSuccess(), resp.error_message);
        if (!effect.endpoint_error.empty())
        {
            // A redirect the mode refused: fail the request rather than use or drop it silently.
            resp.transfer = {ADBC_STATUS_IO, effect.endpoint_error};
        }
        else
            resp.new_endpoint = effect.new_endpoint;
    }
}

void HttpClient::abandonResult(uint64_t response_id)
{
    if (handles->holder && handles->holder->id() == response_id)
        handles->holder->abandon({ADBC_STATUS_INVALID_STATE, "This result was replaced by a new execution of its statement"});
}

bool HttpClient::resultStreamOpen() const
{
    return handles->holder != nullptr;
}

StartedResponse HttpClient::refuseWhileResultStreamOpen()
{
    StartedResponse refused;
    refused.response.transfer
        = {ADBC_STATUS_INVALID_STATE, "A query result stream is still open on this connection; read it to the end or release it first"};
    return refused;
}

CURL * HttpClient::takeHandle()
{
    if (resultStreamOpen())
        return nullptr;
    curl_easy_reset(handles->easy);
    return handles->easy;
}

StartedResponse HttpClient::sendAndAwaitResponseStart(std::initializer_list<const char *> extra_headers, long idle_timeout_sec)
{
    // The headers are rebuilt per attempt: a retry after re-authentication must
    // carry the new token.
    for (int attempt = 0;; ++attempt)
    {
        // FB2 SaaS (Legacy) mode hook: the token comes from the mode's cache.
        const std::string token = fb_conn.fb2 ? fb_conn.fb2->bearerToken() : fb_conn.token;
        curl_slist * headers = buildAuthHeader(token);
        for (const char * header : extra_headers)
            headers = curl_slist_append(headers, header);

        StartedResponse started;
        started.body = std::make_unique<ReadBufferFromHttpResponse>(handles, headers, idle_timeout_sec);
        HttpResponse & resp = started.response;
        resp.transfer = started.body->receiveMoreBody();
        curl_easy_getinfo(handles->easy, CURLINFO_RESPONSE_CODE, &resp.http_code);
        if (resp.transfer.ok() && !resp.isSuccess())
            resp.transfer = readToEnd(*started.body, resp.error_message);

        // FB2 SaaS (Legacy) mode hook: one re-authentication and retry on 401.
        // Safe for any statement: a 401 is decided before the statement runs.
        if (attempt == 0 && resp.transfer.ok() && resp.http_code == 401 && fb_conn.fb2 && fb_conn.fb2->reauthenticate(token))
        {
            started.body->releaseHandles();
            continue;
        }

        parseResponseHeaders(resp);
        if (!resp.isSuccess())
            started.body.reset();
        return started;
    }
}

HttpResponse HttpClient::readWholeResponse(StartedResponse started)
{
    HttpResponse resp = std::move(started.response);
    if (started.body)
        resp.transfer = readToEnd(*started.body, resp.body);
    return resp;
}

StartedResponse HttpClient::startQuery(const std::string & sql, const std::unordered_map<std::string, std::string> & session_params)
{
    CURL * handle = takeHandle();
    if (!handle)
        return refuseWhileResultStreamOpen();

    std::string url = buildUrl(session_params);
    // Append output_format for SELECT queries
    url += (url.find('?') == std::string::npos ? '?' : '&');
    url += "output_format=ArrowStream";

    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_POST, 1L);
    // Copied: the request may outlive `sql`.
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(sql.size()));
    curl_easy_setopt(handle, CURLOPT_COPYPOSTFIELDS, sql.c_str());
    applyTlsOptions();

    // Content-Type: text/plain so the server knows body is raw SQL.  Accept names
    // the Arrow IPC compression codecs firebolt.arrow_compression allows, out of
    // those nanoarrow is built with (CMakeLists.txt).  The timeout bounds each wait
    // for the server, not the time the caller takes to read.
    std::string accept = "Accept: application/vnd.apache.arrow.stream";
    if (!fb_conn.db->arrow_codecs.empty())
        accept += "; codecs=\"" + fb_conn.db->arrow_codecs + "\"";
    return sendAndAwaitResponseStart({"Content-Type: text/plain; charset=utf-8", accept.c_str()}, fb_conn.db->timeout_sec);
}

HttpResponse HttpClient::executeQuery(const std::string & sql, const std::unordered_map<std::string, std::string> & session_params)
{
    return readWholeResponse(startQuery(sql, session_params));
}

HttpResponse HttpClient::executeInsert(
    const std::string & sql, std::span<const uint8_t> arrow_ipc_bytes, const std::unordered_map<std::string, std::string> & session_params)
{
    CURL * handle = takeHandle();
    if (!handle)
        return refuseWhileResultStreamOpen().response;

    std::string url = buildUrl(session_params);
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, fb_conn.db->timeout_sec);
    applyTlsOptions();

    // Build multipart/form-data using curl_mime
    curl_mime * mime = curl_mime_init(handle);

    // SQL part
    curl_mimepart * sql_part = curl_mime_addpart(mime);
    curl_mime_name(sql_part, "sql");
    curl_mime_type(sql_part, "text/plain; charset=utf-8");
    curl_mime_data(sql_part, sql.c_str(), sql.size());

    // Arrow IPC part
    curl_mimepart * arrow_part = curl_mime_addpart(mime);
    curl_mime_name(arrow_part, "data.arrow");
    curl_mime_filename(arrow_part, "data.arrow");
    curl_mime_type(arrow_part, "application/octet-stream");
    // Handed to libcurl a slice at a time, so the payload is never copied.
    ReadBufferFromMemory payload(arrow_ipc_bytes);
    curl_mime_data_cb(arrow_part, static_cast<curl_off_t>(arrow_ipc_bytes.size()), readUploadPart, seekUploadPart, nullptr, &payload);

    curl_easy_setopt(handle, CURLOPT_MIMEPOST, mime);

    // CURLOPT_TIMEOUT bounds the whole upload, so no wait of its own.
    HttpResponse resp = readWholeResponse(sendAndAwaitResponseStart({}, 0));
    curl_mime_free(mime);
    return resp;
}

} // namespace firebolt::adbc
