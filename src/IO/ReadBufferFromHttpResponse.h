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
#include <memory>
#include <stdexcept>
#include <vector>

#include <curl/curl.h>

#include "IO/IReadBuffer.h"

namespace firebolt::adbc
{

class ReadBufferFromHttpResponse;

// A connection's curl handles: an easy handle and the multi handle that runs its
// transfers, which keeps the HTTP connection open from one request to the next.
// A response is read through them until it ends, so the connection sends one
// request at a time; `holder` is the response holding them, if any.
struct CurlHandles
{
    CURLM * multi = curl_multi_init();
    CURL * easy = curl_easy_init();
    ReadBufferFromHttpResponse * holder = nullptr;
    // The id of the last response started on these handles.
    uint64_t last_response_id = 0;

    CurlHandles()
    {
        if (!multi || !easy)
        {
            curl_multi_cleanup(multi);
            curl_easy_cleanup(easy);
            throw std::runtime_error("Failed to initialize curl handles");
        }
    }
    ~CurlHandles()
    {
        curl_multi_cleanup(multi);
        curl_easy_cleanup(easy);
    }
    CurlHandles(const CurlHandles &) = delete;
    CurlHandles & operator=(const CurlHandles &) = delete;
};

// The response to one request, received as it is read.  Its transfer runs on the
// connection's multi handle and only while a reader waits for bytes, so a body is
// never received faster than it is consumed.  The handles are the response's
// until its transfer ends or it is destroyed; until then the connection can send
// nothing else.
class ReadBufferFromHttpResponse final : public IReadBuffer
{
public:
    // Starts the request configured on `handles->easy`, sending `request_headers`,
    // which it frees.  `idle_timeout_sec` bounds each wait for the server;
    // 0 means no bound.
    ReadBufferFromHttpResponse(std::shared_ptr<CurlHandles> handles, curl_slist * request_headers, long idle_timeout_sec);
    ~ReadBufferFromHttpResponse() override;

    ReadBufferFromHttpResponse(const ReadBufferFromHttpResponse &) = delete;
    ReadBufferFromHttpResponse & operator=(const ReadBufferFromHttpResponse &) = delete;

    // Wait for body bytes beyond those received, or for the end of the transfer.
    // Called once before anything is read, it waits for the response to start:
    // its status and headers are then known.
    Status receiveMoreBody();

    // Stop the transfer, if unfinished, and give the handles back to the
    // connection with their options kept, so the request can be sent again.
    void releaseHandles();

    // Release the handles and fail every later read with `reason`.
    void abandon(Status reason);

    // Tells this response apart from others on the same handles.
    uint64_t id() const { return response_id; }

    Status read(uint8_t * buffer, size_t size, size_t & bytes_read) override;

private:
    static size_t onBodyBytes(char * ptr, size_t size, size_t nmemb, void * userdata);

    std::shared_ptr<CurlHandles> handles;
    curl_slist * request_headers;
    long idle_timeout_sec;
    uint64_t response_id;
    // Why reads fail from now on, once the response was abandoned.
    Status abandoned;
    // During read(), the caller's buffer: curl's body pieces are copied straight
    // into it, `caller_bytes` so far.
    uint8_t * caller_buffer = nullptr;
    size_t caller_buffer_size = 0;
    size_t caller_bytes = 0;
    // Body bytes received but not yet read, from `consumed` on: those that did not
    // fit the caller's buffer.  Read before anything new is received, and refilled
    // only once empty, by one curl_multi_perform: curl reads the socket at most ~11
    // times its 16 KiB buffer per call, so this holds ~176 KiB at most, however
    // large the body.
    std::vector<uint8_t> received;
    size_t consumed = 0;
    bool finished = false;
    CURLcode transfer_result = CURLE_OK;
};

} // namespace firebolt::adbc
