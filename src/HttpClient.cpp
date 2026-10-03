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
#include "fb2/Fb2LegacyMode.h"

#include <cstring>
#include <stdexcept>

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

} // namespace

HttpClient::HttpClient(const FireboltConnection & conn) : fb_conn(conn)
{
    handle = curl_easy_init();
    if (!handle)
        throw std::runtime_error("Failed to initialize curl handle");
}

HttpClient::~HttpClient()
{
    if (handle)
        curl_easy_cleanup(handle);
}

size_t HttpClient::writeBodyCallback(char * ptr, size_t size, size_t nmemb, void * userdata)
{
    auto * body = static_cast<std::vector<uint8_t> *>(userdata);
    const size_t total = size * nmemb;
    body->insert(body->end(), ptr, ptr + total);
    return total;
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
        url += k + "=" + urlEncode(handle, v);
    };

    if (!fb_conn.db->database.empty())
        add_param("database", fb_conn.db->database);

    for (const auto & [k, v] : session_params)
        add_param(k, v);

    return url;
}

curl_slist * HttpClient::buildAuthHeader() const
{
    curl_slist * headers = nullptr;
    // FB2 SaaS (Legacy) mode hook: the token comes from the mode's cache.
    const std::string token = fb_conn.fb2 ? fb_conn.fb2->bearerToken() : fb_conn.token;
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
        curl_easy_setopt(handle, CURLOPT_CAINFO, fb_conn.db->ca_bundle_path.c_str());
}

void HttpClient::parseResponseHeaders(HttpResponse & resp) const
{
    resp.reset_session = shouldResetSession(handle);
    resp.update_params = parseUpdateParameters(handle);
    resp.remove_params = parseRemoveParameters(handle);

    // FB2 SaaS (Legacy) mode hook: Firebolt-Update-Endpoint and FB2 error messages.
    if (fb_conn.fb2)
    {
        auto effect = fb_conn.fb2->onResponse(handle, resp.http_code, resp.isSuccess(), resp.error_message);
        if (!effect.endpoint_error.empty())
        {
            // A redirect the mode refused: fail the request rather than use or drop it silently.
            resp.curl_code = CURLE_WEIRD_SERVER_REPLY;
            resp.error_message = effect.endpoint_error;
        }
        else
            resp.new_endpoint = effect.new_endpoint;
    }
}

void HttpClient::sendWithAuthRetry(HttpResponse & resp, const char * content_type)
{
    // The headers are rebuilt per attempt: a retry after re-authentication must
    // carry the new token.
    for (int attempt = 0;; ++attempt)
    {
        resp.body.clear();
        curl_slist * headers = buildAuthHeader();
        if (content_type)
            headers = curl_slist_append(headers, content_type);
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
        resp.curl_code = curl_easy_perform(handle);
        curl_slist_free_all(headers);
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, nullptr);
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &resp.http_code);

        // FB2 SaaS (Legacy) mode hook: one re-authentication and retry on 401.
        // Safe for any statement: a 401 is decided before the statement runs.
        if (attempt == 0 && resp.curl_code == CURLE_OK && resp.http_code == 401 && fb_conn.fb2 && fb_conn.fb2->reauthenticate())
            continue;
        break;
    }

    if (resp.curl_code != CURLE_OK)
        resp.error_message = std::string("curl error: ") + curl_easy_strerror(resp.curl_code);
    else if (!resp.isSuccess())
        resp.error_message = std::string(resp.body.begin(), resp.body.end());

    parseResponseHeaders(resp);
}

HttpResponse HttpClient::executeQuery(const std::string & sql, const std::unordered_map<std::string, std::string> & session_params)
{
    HttpResponse resp;

    std::string url = buildUrl(session_params);
    // Append output_format for SELECT queries
    url += (url.find('?') == std::string::npos ? '?' : '&');
    url += "output_format=ArrowStream";

    curl_easy_reset(handle);
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_POST, 1L);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, sql.c_str());
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(sql.size()));
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeBodyCallback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, fb_conn.db->timeout_sec);
    applyTlsOptions();

    // Content-Type: text/plain so the server knows body is raw SQL
    sendWithAuthRetry(resp, "Content-Type: text/plain; charset=utf-8");
    return resp;
}

HttpResponse HttpClient::executeInsert(
    const std::string & sql,
    const std::vector<uint8_t> & arrow_ipc_bytes,
    const std::unordered_map<std::string, std::string> & session_params)
{
    HttpResponse resp;

    std::string url = buildUrl(session_params);

    curl_easy_reset(handle);
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeBodyCallback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &resp.body);
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
    curl_mime_data(arrow_part, reinterpret_cast<const char *>(arrow_ipc_bytes.data()), arrow_ipc_bytes.size());

    curl_easy_setopt(handle, CURLOPT_MIMEPOST, mime);

    sendWithAuthRetry(resp, nullptr);
    curl_easy_setopt(handle, CURLOPT_MIMEPOST, nullptr);
    curl_mime_free(mime);
    return resp;
}

} // namespace firebolt::adbc
