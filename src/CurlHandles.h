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

#include <cstdint>

#include <curl/curl.h>

namespace firebolt::adbc
{

class ReadBufferFromHttpResponse;

// A connection's curl handles: an easy handle and the multi handle that runs its
// transfers, which keeps the HTTP connection open from one request to the next.
// A response is read through them until it ends, so the connection sends one
// request at a time; `holder` is the response holding them, if any.
struct CurlHandles
{
    CURLM * multi = nullptr;
    CURL * easy = nullptr;
    ReadBufferFromHttpResponse * holder = nullptr;
    // The id of the last response started on these handles.
    uint64_t last_response_id = 0;

    CurlHandles();
    ~CurlHandles();
    CurlHandles(const CurlHandles &) = delete;
    CurlHandles & operator=(const CurlHandles &) = delete;
};

} // namespace firebolt::adbc
