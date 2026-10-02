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

#include <string>
#include <unordered_map>
#include <vector>

#include <curl/curl.h>

namespace firebolt::adbc
{

// Parse all `Firebolt-Update-Parameters: k=v[, k=v]*` headers from the final
// response on the given curl handle into a flat map.  The last occurrence of
// a key wins.  `handle` must be a curl easy handle whose last
// curl_easy_perform() has completed; libcurl owns the parsed-header strings
// until the next curl call on this handle, so callers should consume the
// result before reusing the handle.
//
// Header-name matching, case-insensitivity, value trimming, and the
// CURLH_HEADER origin filter (which excludes 1xx prelude / CONNECT / trailer
// headers) are all delegated to libcurl's curl_easy_header API.
std::unordered_map<std::string, std::string> parseUpdateParameters(CURL * handle);

// Parse all `Firebolt-Remove-Parameters: k[, k]*` headers into a flat list.
std::vector<std::string> parseRemoveParameters(CURL * handle);

// True if at least one `Firebolt-Reset-Session` header is present on the
// final response.
bool shouldResetSession(CURL * handle);

} // namespace firebolt::adbc
