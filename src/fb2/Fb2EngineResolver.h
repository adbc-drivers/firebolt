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
// Account and engine names → the user engine's URL, the way the 2.0 control
// plane hands it out:
//
//   1. GET <api>/web/v3/account/<account>/engineUrl  → the system engine's host
//   2. USE DATABASE "<db>" on the system engine       → validates the database
//   3. USE ENGINE "<engine>" on the system engine     → Firebolt-Update-Endpoint
//
// Hosts come back without a scheme and may carry a query string
// (`host?engine=name`), which is kept: it is how a 2.0 gateway routes a request
// to its engine.
#pragma once

#include "fb2/Fb2Http.h"
#include "fb2/Fb2LegacyMode.h"

#include <string>

namespace firebolt::adbc::fb2
{

// https://api.<environment>.firebolt.io
std::string apiEndpointFor(const std::string & environment);

// <api>/web/v3/account/<account, percent-encoded>/engineUrl
std::string engineUrlRequestUrl(const std::string & api_endpoint, const std::string & account);

// The system engine's URL from an engineUrl response, prefixed with `scheme`
// ("https", or the scheme of an api_endpoint override).
Status parseEngineUrlResponse(const Fb2HttpResult & result, const std::string & account, const std::string & scheme, std::string & url);

// `USE ENGINE "<name>"`, the identifier double-quoted with embedded quotes doubled.
std::string useStatement(const char * object, const std::string & name);

// Interpret a USE DATABASE / USE ENGINE response.  `kind` and `name` describe the
// object for the message ("engine", "my_engine").
Status parseUseResponse(const Fb2HttpResult & result, const char * kind, const std::string & name);

// A Firebolt-Update-Endpoint value (`host[:port][/path][?query]`, optionally
// with a scheme) as a URL the connection can use — but only if it stays on the
// system engine's scheme and either its host or a sibling under the same parent
// domain (`a.api.us-east-1.app.firebolt.io` → `*.api.us-east-1.app.firebolt.io`).
// Otherwise a forged header could send the bearer token anywhere.
Status resolveUpdateEndpoint(const std::string & header_value, const std::string & system_engine_url, std::string & url);

// A failed query's message, reworded when it is one of the 2.0 gateway's
// known answers.  `stopped` is set when the engine is not running.
std::string explainEngineError(long http_code, const std::string & message, bool & stopped);

} // namespace firebolt::adbc::fb2
