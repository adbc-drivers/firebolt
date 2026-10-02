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

#include <cstring>
#include <utility>
#include <strings.h>

namespace firebolt::adbc
{

namespace
{

    constexpr const char * kScheme = "firebolt://";

    int hexValue(char c)
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    }

    bool percentDecode(const std::string & in, std::string & out)
    {
        out.clear();
        for (size_t i = 0; i < in.size(); ++i)
        {
            if (in[i] != '%')
            {
                out += in[i];
                continue;
            }
            if (i + 2 >= in.size())
                return false;
            const int hi = hexValue(in[i + 1]);
            const int lo = hexValue(in[i + 2]);
            if (hi < 0 || lo < 0)
                return false;
            out += static_cast<char>(hi * 16 + lo);
            i += 2;
        }
        return true;
    }

} // namespace

bool isFireboltUri(const std::string & uri)
{
    const size_t n = std::strlen(kScheme);
    return uri.size() >= n && strncasecmp(uri.c_str(), kScheme, n) == 0;
}

AdbcStatusCode parseFireboltUri(const std::string & uri, FireboltUri & out, std::string & message)
{
    const std::string rest = uri.substr(std::strlen(kScheme));
    const size_t authority_end = rest.find_first_of("/?");
    const std::string authority = rest.substr(0, authority_end);
    std::string path;
    std::string query;
    if (authority_end != std::string::npos)
    {
        const std::string tail = rest.substr(authority_end);
        const size_t q = tail.find('?');
        path = tail.substr(0, q);
        if (q != std::string::npos)
            query = tail.substr(q + 1);
    }

    // Credentials in the URI are OAuth client_credentials in Firebolt's SDK
    // spec, which this driver does not implement yet.  The authority is not
    // echoed back, since it holds a secret.
    if (authority.find('@') != std::string::npos)
    {
        message = "Database 'uri' carries credentials (user:password@), which this driver does not support yet; "
                  "remove them and pass a bearer token in 'firebolt.token'";
        return ADBC_STATUS_NOT_IMPLEMENTED;
    }
    if (authority.empty())
    {
        message = "Database 'uri' has no host; got '" + uri + "'. Expected firebolt://<host>[:<port>]/[<database>]";
        return ADBC_STATUS_INVALID_ARGUMENT;
    }

    // The path names the database: one segment, possibly empty.  Slashes are
    // checked before decoding, so %2F can still put one in a database name.
    const std::string segment = path.empty() ? path : path.substr(1);
    if (segment.find('/') != std::string::npos)
    {
        message = "Database 'uri' path must be a single database name; got '" + segment + "'";
        return ADBC_STATUS_INVALID_ARGUMENT;
    }
    std::string database;
    if (!percentDecode(segment, database))
    {
        message = "Database 'uri' has a malformed percent-escape in the database name '" + segment + "'";
        return ADBC_STATUS_INVALID_ARGUMENT;
    }

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
        std::string key;
        std::string value;
        if (!percentDecode(pair.substr(0, eq), key) || !percentDecode(eq == std::string::npos ? "" : pair.substr(eq + 1), value))
        {
            message = "Database 'uri' has a malformed percent-escape in query parameter '" + pair + "'";
            return ADBC_STATUS_INVALID_ARGUMENT;
        }
        if (key != "ssl_mode")
        {
            message = "Unknown query parameter '" + key + "' in database 'uri'; the supported one is ssl_mode";
            return ADBC_STATUS_NOT_FOUND;
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
        message = "ssl_mode=" + ssl_mode
            + " is not supported: this driver always verifies the certificate and host name. "
              "Use ssl_mode=verify-full (the default) or ssl_mode=disable";
        return ADBC_STATUS_NOT_IMPLEMENTED;
    }
    else
    {
        message = "ssl_mode must be verify-full or disable; got '" + ssl_mode + "'";
        return ADBC_STATUS_INVALID_ARGUMENT;
    }

    out.endpoint = transport + authority;
    out.database = std::move(database);
    return ADBC_STATUS_OK;
}

} // namespace firebolt::adbc
