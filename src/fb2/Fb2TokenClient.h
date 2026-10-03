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
//
// Service-account credentials → bearer token, through the Firebolt 2.0 token
// endpoint (https://id.<env>.firebolt.io/oauth/token, OAuth 2.0
// client_credentials with the fixed audience https://api.firebolt.io), with a
// process-wide cache.  dbapi.connect() makes a new AdbcDatabase per call, so a
// cache scoped to one database would exchange credentials on every connect.
#pragma once

#include "fb2/Fb2Http.h"
#include "fb2/Fb2LegacyMode.h"

#include <chrono>
#include <string>

namespace firebolt::adbc::fb2
{

inline constexpr const char * kAudience = "https://api.firebolt.io";

// A token is refreshed this long before the server says it expires, so a
// request never leaves with a token that dies in flight.
inline constexpr std::chrono::seconds kExpiryMargin{60};

struct Credentials
{
    std::string client_id;
    std::string client_secret;
};

// https://id.<environment>.firebolt.io/oauth/token
std::string tokenEndpointFor(const std::string & environment);

// The request body, every value form-encoded.
std::string tokenRequestBody(const Credentials & credentials);

struct TokenGrant
{
    std::string access_token;
    std::chrono::seconds expires_in{0};
};

// Turn a token-endpoint response into a grant, or a status saying what went
// wrong.  The client ID may appear in a message; the secret never does.
Status parseTokenResponse(const Fb2HttpResult & result, const std::string & endpoint, const std::string & client_id, TokenGrant & grant);

// The process-wide cache.  Keyed by endpoint and both credentials, so a
// rotated secret never reuses the old one's token.  Thread-safe.
class TokenCache
{
public:
    // A cached token still valid at `now` (with kExpiryMargin to spare), or empty.
    static std::string lookup(const std::string & endpoint, const Credentials & credentials, std::chrono::steady_clock::time_point now);
    static void store(
        const std::string & endpoint, const Credentials & credentials, const TokenGrant & grant, std::chrono::steady_clock::time_point now);
    static void invalidate(const std::string & endpoint, const Credentials & credentials);
    static void clearForTesting();
};

// Exchange the credentials, through the cache unless `use_cache` is false.
Status acquireToken(
    const std::string & endpoint, const Credentials & credentials, const Transport & transport, bool use_cache, std::string & token);

} // namespace firebolt::adbc::fb2
