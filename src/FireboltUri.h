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

#include "adbc.h"

#include <optional>
#include <string>
#include <variant>

namespace firebolt::adbc
{

// A firebolt:// connection URI, translated into what the driver sends requests to.
//
//     firebolt://<host>[:<port>]/[<database>][?ssl_mode=<mode>]
//
// The shape follows Firebolt's SDK connection-string spec: one scheme, with the
// transport chosen by ssl_mode (verify-full, the default, means https; disable
// means plaintext http) rather than by a second scheme.
struct FireboltUri
{
    std::string endpoint; // http(s)://<host>[:<port>]
    std::string database; // percent-decoded path segment; empty when absent
};

// The uri is not firebolt:// at all; the http(s):// checks decide what it is.
struct NotFireboltUri
{
};

// A firebolt:// uri that cannot be used, with the status to report it under.
struct FireboltUriError
{
    AdbcStatusCode code;
    std::string message;
};

// Never throws: running out of memory comes back as a FireboltUriError.
std::variant<NotFireboltUri, FireboltUri, FireboltUriError> parseFireboltUri(const std::string & uri) noexcept;

// An absolute URL as libcurl reads it, for checks on where a request will go.
struct ParsedUrl
{
    std::string scheme; // lower case
    std::string host; // lower case; an IPv6 literal keeps its brackets
    std::string port; // the explicit port, or the scheme's default one
    bool has_userinfo = false; // user[:password]@ before the host
};

// nullopt when libcurl does not accept `url` as an absolute URL with a host, or
// on allocation failure.  Never throws.
std::optional<ParsedUrl> parseUrl(const std::string & url) noexcept;

} // namespace firebolt::adbc
