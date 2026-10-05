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
#include <mutex>
#include <string>
#include <unordered_map>

namespace firebolt::adbc
{

struct FireboltDatabase;
class HttpClient;

namespace fb2
{
    class Fb2LegacyMode;
}

struct FireboltConnection
{
    FireboltDatabase * db = nullptr; // borrowed
    // The endpoint this connection sends to.  Starts as the database's URL; a
    // server may move it (Firebolt-Update-Endpoint, honoured in FB2 mode only).
    std::string url;
    // FB2 SaaS (Legacy) mode hook: the database's mode, null on the Core path.
    std::shared_ptr<fb2::Fb2LegacyMode> fb2;
    std::unique_ptr<HttpClient> http;
    std::unordered_map<std::string, std::string> session_params;
    // ADBC requires ConnectionGetOption to be thread-safe with itself. Other
    // connection operations are not thread-safe, so this mutex only serializes
    // concurrent option reads that share the connection's curl handle.
    std::mutex get_option_mutex;
    // Per-connection bearer token.  Initialised from FireboltDatabase::token at
    // ConnectionInit time, then mutated only by ConnectionSetOption on this
    // connection — keeping connections that share a FireboltDatabase isolated.
    std::string token;
    bool autocommit = true; // ADBC_CONNECTION_OPTION_AUTOCOMMIT
    bool in_transaction = false; // true between BEGIN and COMMIT/ROLLBACK
};

} // namespace firebolt::adbc
