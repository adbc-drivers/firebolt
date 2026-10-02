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

#include "HttpHeaderParse.h"

#include <sstream>
#include <string_view>

#include <curl/curl.h>
#include <curl/header.h>

namespace firebolt::adbc
{

namespace
{

    // Iterate every value of `name` on the final response of `handle`, calling
    // `cb` with each non-empty trimmed value.  CURLH_HEADER restricts the
    // walk to the plain server response (libcurl puts 1xx prelude, CONNECT,
    // and trailer headers under separate origin flags), so reflection /
    // 1xx-smuggling vectors are ruled out by libcurl's parser before we see
    // anything.
    template <class Fn>
    void forEachHeaderValue(CURL * handle, const char * name, Fn && cb)
    {
        for (size_t index = 0;; ++index)
        {
            struct curl_header * h = nullptr;
            if (curl_easy_header(handle, name, index, CURLH_HEADER, -1, &h) != CURLHE_OK)
                return;
            if (!h || !h->value || !*h->value)
                continue;
            cb(std::string_view(h->value));
        }
    }

    void trim(std::string & s)
    {
        const auto first = s.find_first_not_of(" \t");
        if (first == std::string::npos)
        {
            s.clear();
            return;
        }
        s.erase(0, first);
        s.erase(s.find_last_not_of(" \t") + 1);
    }

} // namespace

std::unordered_map<std::string, std::string> parseUpdateParameters(CURL * handle)
{
    std::unordered_map<std::string, std::string> params;
    forEachHeaderValue(handle, "Firebolt-Update-Parameters", [&](std::string_view hval) {
        std::stringstream ss{std::string(hval)};
        std::string pair;
        while (std::getline(ss, pair, ','))
        {
            const auto eq = pair.find('=');
            if (eq == std::string::npos)
                continue;
            std::string key = pair.substr(0, eq);
            std::string val = pair.substr(eq + 1);
            trim(key);
            trim(val);
            if (!key.empty())
                params[key] = val;
        }
    });
    return params;
}

std::vector<std::string> parseRemoveParameters(CURL * handle)
{
    std::vector<std::string> params;
    forEachHeaderValue(handle, "Firebolt-Remove-Parameters", [&](std::string_view hval) {
        std::stringstream ss{std::string(hval)};
        std::string name;
        while (std::getline(ss, name, ','))
        {
            trim(name);
            if (!name.empty())
                params.push_back(std::move(name));
        }
    });
    return params;
}

bool shouldResetSession(CURL * handle)
{
    struct curl_header * h = nullptr;
    return curl_easy_header(handle, "Firebolt-Reset-Session", 0, CURLH_HEADER, -1, &h) == CURLHE_OK;
}

} // namespace firebolt::adbc
