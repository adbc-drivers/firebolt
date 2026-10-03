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

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <strings.h>

namespace firebolt::adbc
{

namespace
{

    constexpr const char * kScheme = "firebolt://";

    // One component of a parsed URL, or nullopt when it is absent or, with
    // CURLU_URLDECODE, does not decode.
    std::optional<std::string> urlPart(CURLU * url, CURLUPart part, unsigned int flags = 0)
    {
        char * value = nullptr;
        if (curl_url_get(url, part, &value, flags) != CURLUE_OK)
            return std::nullopt;
        const std::unique_ptr<char, decltype(&curl_free)> owned(value, &curl_free);
        return std::string(owned.get());
    }

} // namespace

std::variant<NotFireboltUri, FireboltUri, Status> parseFireboltUri(const std::string & uri) noexcept
try
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
        return Status{
            ADBC_STATUS_NOT_IMPLEMENTED,
            "Database 'uri' carries credentials (user:password@), which this driver does not support yet; "
            "remove them and pass a bearer token in 'firebolt.token'. A literal '@' (for example in a "
            "database name) is written %40"};
    }

    // libcurl reads firebolt:///db as host "db", which is the shape of the
    // older account-based DSN; here an empty authority means no host.
    if (uri.size() == authority_start || uri[authority_start] == '/' || uri[authority_start] == '?')
    {
        return Status{
            ADBC_STATUS_INVALID_ARGUMENT,
            "Database 'uri' has no host; got '" + uri + "'. Expected firebolt://<host>[:<port>]/[<database>]"};
    }

    const std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), &curl_url_cleanup);
    if (!url)
        return Status{ADBC_STATUS_INTERNAL, "out of memory"};
    const CURLUcode rc = curl_url_set(url.get(), CURLUPART_URL, uri.c_str(), CURLU_NON_SUPPORT_SCHEME);
    if (rc != CURLUE_OK)
    {
        return Status{
            ADBC_STATUS_INVALID_ARGUMENT,
            std::string("Database 'uri' is not a valid firebolt:// URI (") + curl_url_strerror(rc) + "); got '" + uri + "'"};
    }
    const std::optional<std::string> port = urlPart(url.get(), CURLUPART_PORT);
    const std::string authority = urlPart(url.get(), CURLUPART_HOST).value_or("") + (port ? ":" + *port : "");

    // The path names the database: one segment, possibly empty.  Slashes are
    // checked before decoding, so %2F can still put one in a database name.
    const std::string segment = urlPart(url.get(), CURLUPART_PATH).value_or("/").substr(1);
    if (segment.find('/') != std::string::npos)
    {
        return Status{ADBC_STATUS_INVALID_ARGUMENT, "Database 'uri' path must be a single database name; got '" + segment + "'"};
    }
    const std::optional<std::string> decoded_path = urlPart(url.get(), CURLUPART_PATH, CURLU_URLDECODE);
    if (!decoded_path)
    {
        return Status{ADBC_STATUS_INVALID_ARGUMENT, "Database 'uri' has a database name that does not decode; got '" + segment + "'"};
    }
    std::string database = decoded_path->substr(1);

    // Query keys and values are fixed ASCII words, so they are compared as written.
    const std::string query = urlPart(url.get(), CURLUPART_QUERY).value_or("");
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
        const std::string key = pair.substr(0, eq);
        const std::string value = eq == std::string::npos ? "" : pair.substr(eq + 1);
        if (key != "ssl_mode")
        {
            return Status{ADBC_STATUS_NOT_FOUND, "Unknown query parameter '" + key + "' in database 'uri'; the supported one is ssl_mode"};
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
        return Status{
            ADBC_STATUS_NOT_IMPLEMENTED,
            "ssl_mode=" + ssl_mode
                + " is not supported: this driver always verifies the certificate and host name. "
                  "Use ssl_mode=verify-full (the default) or ssl_mode=disable"};
    }
    else
    {
        return Status{ADBC_STATUS_INVALID_ARGUMENT, "ssl_mode must be verify-full or disable; got '" + ssl_mode + "'"};
    }

    return FireboltUri{transport + authority, std::move(database)};
}
catch (...)
{
    // Only allocation can fail.  The message fits in std::string's inline
    // buffer, so building this error does not allocate and cannot throw again.
    return Status{ADBC_STATUS_INTERNAL, "out of memory"};
}

std::optional<ParsedUrl> parseUrl(const std::string & url) noexcept
try
{
    const std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(curl_url(), &curl_url_cleanup);
    if (!parsed || curl_url_set(parsed.get(), CURLUPART_URL, url.c_str(), CURLU_NON_SUPPORT_SCHEME) != CURLUE_OK)
        return std::nullopt;
    const auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    std::optional<std::string> scheme = urlPart(parsed.get(), CURLUPART_SCHEME);
    std::optional<std::string> host = urlPart(parsed.get(), CURLUPART_HOST);
    if (!scheme || !host || host->empty())
        return std::nullopt;
    return ParsedUrl{
        lower(*scheme),
        lower(*host),
        urlPart(parsed.get(), CURLUPART_PORT, CURLU_DEFAULT_PORT).value_or(""),
        urlPart(parsed.get(), CURLUPART_USER).has_value()};
}
catch (...)
{
    return std::nullopt;
}

} // namespace firebolt::adbc
