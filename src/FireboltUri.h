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

#include <string>

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

bool isFireboltUri(const std::string & uri);

// ADBC_STATUS_OK and `out` filled, or an error status with `message` set.
AdbcStatusCode parseFireboltUri(const std::string & uri, FireboltUri & out, std::string & message);

} // namespace firebolt::adbc
