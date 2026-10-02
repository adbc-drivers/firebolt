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

HttpClient::HttpClient(const FireboltConnection & conn) : fb_conn_(conn)
{
    handle_ = curl_easy_init();
    if (!handle_)
        throw std::runtime_error("Failed to initialize curl handle");
}

HttpClient::~HttpClient()
{
    if (handle_)
        curl_easy_cleanup(handle_);
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
    std::string url = fb_conn_.db->url;

    bool first = (url.find('?') == std::string::npos);
    auto addParam = [&](const std::string & k, const std::string & v) {
        url += (first ? '?' : '&');
        first = false;
        url += k + "=" + urlEncode(handle_, v);
    };

    if (!fb_conn_.db->database.empty())
        addParam("database", fb_conn_.db->database);

    for (const auto & [k, v] : session_params)
        addParam(k, v);

    return url;
}

curl_slist * HttpClient::buildAuthHeader() const
{
    curl_slist * headers = nullptr;
    if (!fb_conn_.token.empty())
    {
        std::string auth = "Authorization: Bearer " + fb_conn_.token;
        headers = curl_slist_append(headers, auth.c_str());
    }
    headers = curl_slist_append(headers, "Firebolt-Protocol-Version: 2.4");
    return headers;
}

void HttpClient::applyTlsOptions() const
{
    // Peer and host verification are libcurl's defaults and stay on; this only
    // says which CA bundle backs them.  Empty for http://.
    if (!fb_conn_.db->ca_bundle_path.empty())
        curl_easy_setopt(handle_, CURLOPT_CAINFO, fb_conn_.db->ca_bundle_path.c_str());
}

void HttpClient::parseResponseHeaders(HttpResponse & resp) const
{
    resp.reset_session = shouldResetSession(handle_);
    resp.update_params = parseUpdateParameters(handle_);
    resp.remove_params = parseRemoveParameters(handle_);
}

HttpResponse HttpClient::executeQuery(const std::string & sql, const std::unordered_map<std::string, std::string> & session_params)
{
    HttpResponse resp;

    std::string url = buildUrl(session_params);
    // Append output_format for SELECT queries
    url += (url.find('?') == std::string::npos ? '?' : '&');
    url += "output_format=ArrowStream";

    curl_easy_reset(handle_);
    curl_easy_setopt(handle_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle_, CURLOPT_POST, 1L);
    curl_easy_setopt(handle_, CURLOPT_POSTFIELDS, sql.c_str());
    curl_easy_setopt(handle_, CURLOPT_POSTFIELDSIZE, static_cast<long>(sql.size()));
    curl_easy_setopt(handle_, CURLOPT_WRITEFUNCTION, writeBodyCallback);
    curl_easy_setopt(handle_, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(handle_, CURLOPT_TIMEOUT, fb_conn_.db->timeout_sec);
    applyTlsOptions();

    curl_slist * auth_headers = buildAuthHeader();
    // Content-Type: text/plain so the server knows body is raw SQL
    auth_headers = curl_slist_append(auth_headers, "Content-Type: text/plain; charset=utf-8");
    curl_easy_setopt(handle_, CURLOPT_HTTPHEADER, auth_headers);

    resp.curl_code = curl_easy_perform(handle_);
    curl_slist_free_all(auth_headers);

    curl_easy_getinfo(handle_, CURLINFO_RESPONSE_CODE, &resp.http_code);

    if (resp.curl_code != CURLE_OK)
        resp.error_message = curl_easy_strerror(resp.curl_code);
    else if (!resp.isSuccess())
        resp.error_message = std::string(resp.body.begin(), resp.body.end());

    parseResponseHeaders(resp);
    return resp;
}

HttpResponse HttpClient::executeInsert(
    const std::string & sql,
    const std::vector<uint8_t> & arrow_ipc_bytes,
    const std::unordered_map<std::string, std::string> & session_params)
{
    HttpResponse resp;

    std::string url = buildUrl(session_params);

    curl_easy_reset(handle_);
    curl_easy_setopt(handle_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle_, CURLOPT_WRITEFUNCTION, writeBodyCallback);
    curl_easy_setopt(handle_, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(handle_, CURLOPT_TIMEOUT, fb_conn_.db->timeout_sec);
    applyTlsOptions();

    // Set auth header
    curl_slist * auth_headers = buildAuthHeader();
    curl_easy_setopt(handle_, CURLOPT_HTTPHEADER, auth_headers);

    // Build multipart/form-data using curl_mime
    curl_mime * mime = curl_mime_init(handle_);

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

    curl_easy_setopt(handle_, CURLOPT_MIMEPOST, mime);

    resp.curl_code = curl_easy_perform(handle_);
    curl_slist_free_all(auth_headers);
    curl_mime_free(mime);

    curl_easy_getinfo(handle_, CURLINFO_RESPONSE_CODE, &resp.http_code);

    if (resp.curl_code != CURLE_OK)
        resp.error_message = curl_easy_strerror(resp.curl_code);
    else if (!resp.isSuccess())
        resp.error_message = std::string(resp.body.begin(), resp.body.end());

    parseResponseHeaders(resp);
    return resp;
}

} // namespace firebolt::adbc
