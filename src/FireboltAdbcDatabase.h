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

#include <memory>
#include <string>

namespace firebolt::adbc
{

namespace fb2
{
    class Fb2LegacyMode;
}

struct FireboltDatabase
{
    std::string url; // "uri" — the HTTP query endpoint, e.g. "http://localhost:3473"
    std::string token; // "firebolt.token" — JWT bearer token (omit for no-auth)
    std::string database; // "firebolt.database" — database name (optional)
    long timeout_sec = 0; // "firebolt.timeout_sec" — 0 means no timeout (libcurl default)
    std::string ssl_certificate_path; // "firebolt.ssl_certificate_path" — CA bundle override
    std::string ca_bundle_path; // resolved at DatabaseInit for https://; see TlsConfig.h
    bool initialized = false;

    // FB2 SaaS (Legacy) mode hook: created by the first FB2 option, null on the Core
    // path.  When set, DatabaseInit resolves `url` instead of reading it from "uri".
    std::shared_ptr<fb2::Fb2LegacyMode> fb2;

    // First rejected option, held until DatabaseInit reports it.  See the note in
    // DatabaseSetOption for why a bad option is not refused on the spot.
    std::string option_error;
    AdbcStatusCode option_error_code = ADBC_STATUS_OK;
};

} // namespace firebolt::adbc
