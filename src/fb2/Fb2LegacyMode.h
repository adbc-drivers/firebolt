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

// FB2 SaaS (Legacy) mode. Keep isolated; see CLAUDE.md.
//
// Lets the driver reach engines (v5+) deployed in Firebolt 2.0 SaaS:
// service-account credentials plus account and engine names, exchanged for a
// token and resolved to an engine URL through the 2.0 control plane.  The main
// code knows only this header: an optional Fb2LegacyMode on the database (null
// unless FB2 options were set) and the hook calls marked
// "FB2 SaaS (Legacy) mode hook" at its call sites.
#pragma once

#include "Status.h"
#include "adbc.h"

#include <curl/curl.h>

#include <array>
#include <memory>
#include <string>
#include <string_view>

namespace firebolt::adbc::fb2
{

// Every database option FB2 mode owns.  `username`/`password` are ADBC's standard
// credential keys; the firebolt.client_* spellings are Firebolt's own names
// for the same values; either is accepted.  environment, api_endpoint
// and auth_endpoint are internal: undocumented, for staging and tests.
inline constexpr std::array<std::string_view, 10> kOptionKeys = {
    "username",
    "password",
    "firebolt.client_id",
    "firebolt.client_secret",
    "firebolt.account",
    "firebolt.engine",
    "firebolt.cache_connection",
    "firebolt.environment",
    "firebolt.api_endpoint",
    "firebolt.auth_endpoint",
};

inline bool isOption(std::string_view key)
{
    for (auto k : kOptionKeys)
        if (k == key)
            return true;
    return false;
}

// What DatabaseInit hands FB2 mode: the main options it must check or reuse, and
// the transport settings its own control-plane requests follow.
struct InitInputs
{
    std::string uri; // must be empty: FB2 mode resolves the endpoint itself
    std::string token; // firebolt.token: a pre-acquired token, never refreshed
    std::string database; // firebolt.database, validated with USE DATABASE
    std::string ca_bundle_path;
    long timeout_sec = 0;
};

// What a response did, as far as FB2 mode is concerned.
struct ResponseEffect
{
    std::string new_endpoint; // Firebolt-Update-Endpoint, validated; empty if none
    std::string endpoint_error; // a Firebolt-Update-Endpoint that was refused
};

class Fb2LegacyMode
{
public:
    Fb2LegacyMode();
    ~Fb2LegacyMode();
    Fb2LegacyMode(const Fb2LegacyMode &) = delete;
    Fb2LegacyMode & operator=(const Fb2LegacyMode &) = delete;

    // DatabaseSetOption for a key isOption() claims.  Values are checked at init.
    Status setOption(const std::string & key, const std::string & value);

    // True once any FB2 option was set, so DatabaseInit takes this path.
    bool requested() const;

    // DatabaseInit: validate, authenticate, resolve the engine.  On success
    // `endpoint` is the user engine's URL, query string included.
    Status init(const InitInputs & inputs, std::string & endpoint);

    // The bearer token for the next request.
    std::string bearerToken();

    // A request was refused with 401.  Drops the cached token and fetches a new
    // one; true when the caller should retry the request once.
    bool reauthenticate();

    // Called for every completed response on a connection in FB2 mode.  On
    // success reads Firebolt-Update-Endpoint; on failure may replace
    // `error_message` with one that says what to do (start the engine, …).
    ResponseEffect onResponse(CURL * handle, long http_code, bool success, std::string & error_message);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace firebolt::adbc::fb2
