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

#include "FireboltUri.h"

#include <curl/curl.h>

#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <strings.h>

namespace firebolt::adbc
{

namespace
{

    constexpr const char * kScheme = "firebolt://";

    struct CurlUrlCleanup
    {
        void operator()(CURLU * url) const { curl_url_cleanup(url); }
    };

    struct CurlFree
    {
        void operator()(char * text) const { curl_free(text); }
    };

    using CurlString = std::unique_ptr<char, CurlFree>;

    // One component of a parsed URL, still percent-encoded; empty when absent.
    std::string urlPart(CURLU * url, CURLUPart part)
    {
        char * value = nullptr;
        if (curl_url_get(url, part, &value, 0) != CURLUE_OK)
            return {};
        return CurlString(value).get();
    }

    // A '%' that does not start a valid escape is kept as it is.
    std::string percentDecode(const std::string & text)
    {
        int length = 0;
        CurlString decoded(curl_easy_unescape(nullptr, text.c_str(), static_cast<int>(text.size()), &length));
        if (!decoded)
            throw std::bad_alloc();
        return std::string(decoded.get(), static_cast<size_t>(length));
    }

} // namespace

std::variant<NotFireboltUri, FireboltUri, FireboltUriError> parseFireboltUri(const std::string & uri)
{
    const size_t authority_start = std::strlen(kScheme);
    if (uri.size() < authority_start || strncasecmp(uri.c_str(), kScheme, authority_start) != 0)
        return NotFireboltUri{};

    // Credentials in the URI are OAuth client_credentials in Firebolt's SDK
    // spec, which this driver does not implement yet.  The whole URI is
    // checked, not just the authority: an unencoded '/' or '?' in a password
    // would otherwise move part of it into the path or the query.  No part of
    // the URI is echoed back, since it holds a secret.
    if (uri.find('@') != std::string::npos)
    {
        return FireboltUriError{
            ADBC_STATUS_NOT_IMPLEMENTED,
            "Database 'uri' carries credentials (user:password@), which this driver does not support yet; "
            "remove them and pass a bearer token in 'firebolt.token'. A literal '@' (for example in a "
            "database name) is written %40"};
    }

    // libcurl reads firebolt:///db as host "db", which is the shape of the
    // older account-based DSN; here an empty authority means no host.
    if (uri.size() == authority_start || uri[authority_start] == '/' || uri[authority_start] == '?')
    {
        return FireboltUriError{
            ADBC_STATUS_INVALID_ARGUMENT,
            "Database 'uri' has no host; got '" + uri + "'. Expected firebolt://<host>[:<port>]/[<database>]"};
    }

    const std::unique_ptr<CURLU, CurlUrlCleanup> url(curl_url());
    if (!url)
        throw std::bad_alloc();
    const CURLUcode rc = curl_url_set(url.get(), CURLUPART_URL, uri.c_str(), CURLU_NON_SUPPORT_SCHEME);
    if (rc != CURLUE_OK)
    {
        return FireboltUriError{
            ADBC_STATUS_INVALID_ARGUMENT,
            std::string("Database 'uri' is not a valid firebolt:// URI (") + curl_url_strerror(rc) + "); got '" + uri + "'"};
    }
    const std::string port = urlPart(url.get(), CURLUPART_PORT);
    const std::string authority = urlPart(url.get(), CURLUPART_HOST) + (port.empty() ? "" : ":" + port);

    // The path names the database: one segment, possibly empty.  Slashes are
    // checked before decoding, so %2F can still put one in a database name.
    const std::string path = urlPart(url.get(), CURLUPART_PATH);
    const std::string segment = path.empty() ? path : path.substr(1);
    if (segment.find('/') != std::string::npos)
    {
        return FireboltUriError{ADBC_STATUS_INVALID_ARGUMENT, "Database 'uri' path must be a single database name; got '" + segment + "'"};
    }
    std::string database = percentDecode(segment);

    const std::string query = urlPart(url.get(), CURLUPART_QUERY);
    std::string ssl_mode = "verify-full";
    size_t pos = 0;
    while (pos < query.size())
    {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos)
            amp = query.size();
        const std::string pair = query.substr(pos, amp - pos);
        pos = amp + 1;
        if (pair.empty())
            continue;
        const size_t eq = pair.find('=');
        const std::string key = percentDecode(pair.substr(0, eq));
        const std::string value = eq == std::string::npos ? "" : percentDecode(pair.substr(eq + 1));
        if (key != "ssl_mode")
        {
            return FireboltUriError{
                ADBC_STATUS_NOT_FOUND, "Unknown query parameter '" + key + "' in database 'uri'; the supported one is ssl_mode"};
        }
        ssl_mode = value;
    }

    const char * transport = nullptr;
    if (ssl_mode == "verify-full")
        transport = "https://";
    else if (ssl_mode == "disable")
        transport = "http://";
    else if (ssl_mode == "verify-ca" || ssl_mode == "require")
    {
        // Both skip part of certificate verification, which has no off switch here.
        return FireboltUriError{
            ADBC_STATUS_NOT_IMPLEMENTED,
            "ssl_mode=" + ssl_mode
                + " is not supported: this driver always verifies the certificate and host name. "
                  "Use ssl_mode=verify-full (the default) or ssl_mode=disable"};
    }
    else
    {
        return FireboltUriError{ADBC_STATUS_INVALID_ARGUMENT, "ssl_mode must be verify-full or disable; got '" + ssl_mode + "'"};
    }

    return FireboltUri{transport + authority, std::move(database)};
}

} // namespace firebolt::adbc
