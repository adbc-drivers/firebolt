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

// FB2 SaaS (Legacy) mode. Keep isolated; see AGENTS.md.
#include "fb2/Fb2Http.h"

#include <curl/curl.h>

#include <memory>

namespace firebolt::adbc::fb2
{

namespace
{

    size_t appendBody(char * ptr, size_t size, size_t nmemb, void * userdata)
    {
        static_cast<std::string *>(userdata)->append(ptr, size * nmemb);
        return size * nmemb;
    }

    using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
    using HeaderList = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;

    Fb2HttpResult sendControlPlaneRequest(CURL * handle, const std::string & url, curl_slist * headers, const Transport & transport)
    {
        Fb2HttpResult result;
        curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, appendBody);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, &result.body);
        curl_easy_setopt(handle, CURLOPT_TIMEOUT, transport.timeout_sec);
        // Redirects are not followed: a control-plane answer that points
        // elsewhere would carry the credentials or token to a host nobody chose.
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
        if (!transport.ca_bundle_path.empty())
            curl_easy_setopt(handle, CURLOPT_CAINFO, transport.ca_bundle_path.c_str());

        CURLcode code = curl_easy_perform(handle);
        if (code != CURLE_OK)
        {
            result.transport_error = curl_easy_strerror(code);
            return result;
        }
        result.transport_ok = true;
        curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &result.status);
        struct curl_header * header = nullptr;
        if (curl_easy_header(handle, "Retry-After", 0, CURLH_HEADER, -1, &header) == CURLHE_OK && header && header->value)
            result.retry_after = header->value;
        header = nullptr;
        if (curl_easy_header(handle, "Firebolt-Update-Endpoint", 0, CURLH_HEADER, -1, &header) == CURLHE_OK && header && header->value)
            result.update_endpoint = header->value;
        return result;
    }

} // namespace

std::string formEncode(const std::string & value)
{
    char * encoded = curl_easy_escape(nullptr, value.c_str(), static_cast<int>(value.size()));
    std::string out(encoded ? encoded : "");
    curl_free(encoded);
    return out;
}

Fb2HttpResult postForm(const std::string & url, const std::string & form_body, const Transport & transport)
{
    CurlHandle handle(curl_easy_init(), curl_easy_cleanup);
    if (!handle)
        return {false, "curl_easy_init failed"};
    HeaderList headers(curl_slist_append(nullptr, "Content-Type: application/x-www-form-urlencoded"), curl_slist_free_all);
    curl_easy_setopt(handle.get(), CURLOPT_POST, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, form_body.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(form_body.size()));
    return sendControlPlaneRequest(handle.get(), url, headers.get(), transport);
}

Fb2HttpResult getJson(const std::string & url, const std::string & bearer_token, const Transport & transport)
{
    CurlHandle handle(curl_easy_init(), curl_easy_cleanup);
    if (!handle)
        return {false, "curl_easy_init failed"};
    HeaderList headers(curl_slist_append(nullptr, "Accept: application/json"), curl_slist_free_all);
    if (!bearer_token.empty())
        headers.reset(curl_slist_append(headers.release(), ("Authorization: Bearer " + bearer_token).c_str()));
    curl_easy_setopt(handle.get(), CURLOPT_HTTPGET, 1L);
    return sendControlPlaneRequest(handle.get(), url, headers.get(), transport);
}

Fb2HttpResult postSql(const std::string & url, const std::string & sql, const std::string & bearer_token, const Transport & transport)
{
    CurlHandle handle(curl_easy_init(), curl_easy_cleanup);
    if (!handle)
        return {false, "curl_easy_init failed"};
    HeaderList headers(curl_slist_append(nullptr, "Content-Type: text/plain; charset=utf-8"), curl_slist_free_all);
    headers.reset(curl_slist_append(headers.release(), "Firebolt-Protocol-Version: 2.4"));
    if (!bearer_token.empty())
        headers.reset(curl_slist_append(headers.release(), ("Authorization: Bearer " + bearer_token).c_str()));
    curl_easy_setopt(handle.get(), CURLOPT_POST, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, sql.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(sql.size()));
    return sendControlPlaneRequest(handle.get(), url, headers.get(), transport);
}

} // namespace firebolt::adbc::fb2
