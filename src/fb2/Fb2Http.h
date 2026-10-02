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
// The mode's own control-plane requests (token exchange, engineUrl), on a
// short-lived curl handle so they never disturb a connection's handle.
#pragma once

#include <string>
#include <vector>

namespace firebolt::adbc::fb2
{

struct Transport
{
    std::string ca_bundle_path; // CURLOPT_CAINFO for https://; empty for http://
    long timeout_sec = 0;
};

struct Fb2HttpResult
{
    bool transport_ok = false; // the request completed at the HTTP level
    std::string transport_error; // curl's reason when it did not
    long status = 0;
    std::string body;
    std::string retry_after; // the Retry-After header, if any
    std::string update_endpoint; // the Firebolt-Update-Endpoint header, if any
};

// POST an application/x-www-form-urlencoded body.
Fb2HttpResult postForm(const std::string & url, const std::string & form_body, const Transport & transport);

// GET with an optional bearer token.
Fb2HttpResult getJson(const std::string & url, const std::string & bearer_token, const Transport & transport);

// POST a SQL statement to an engine, as the driver's data plane does.
Fb2HttpResult postSql(const std::string & url, const std::string & sql, const std::string & bearer_token, const Transport & transport);

// application/x-www-form-urlencoded encoding of one value.
std::string formEncode(const std::string & value);

} // namespace firebolt::adbc::fb2
