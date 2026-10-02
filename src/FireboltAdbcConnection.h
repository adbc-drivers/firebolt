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

#include <memory>
#include <string>
#include <unordered_map>

namespace firebolt::adbc
{

struct FireboltDatabase;
class HttpClient;

struct FireboltConnection
{
    FireboltDatabase * db = nullptr; // borrowed
    std::unique_ptr<HttpClient> http;
    std::unordered_map<std::string, std::string> session_params;
    // Per-connection bearer token.  Initialised from FireboltDatabase::token at
    // ConnectionInit time, then mutated only by ConnectionSetOption on this
    // connection — keeping connections that share a FireboltDatabase isolated.
    std::string token;
    bool autocommit = true; // ADBC_CONNECTION_OPTION_AUTOCOMMIT
    bool in_transaction = false; // true between BEGIN and COMMIT/ROLLBACK
};

} // namespace firebolt::adbc
