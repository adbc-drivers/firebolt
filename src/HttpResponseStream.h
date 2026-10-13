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
#include <string>
#include <vector>

#include <curl/curl.h>

#include "ArrowIpcStream.h"
#include "CurlHandlePool.h"

namespace firebolt::adbc
{

// The response to one request, received as it is read.  Its transfer runs on the
// request's own multi handle and only while a reader waits for bytes, so a body
// is never received faster than it is consumed.
class HttpResponseStream final : public ResponseBodySource
{
public:
    // Starts the request configured on `handles->easy`, sending `request_headers`,
    // which the stream frees.  `idle_timeout_sec` bounds each wait for the server;
    // 0 means no bound.
    HttpResponseStream(PooledCurl handles, curl_slist * request_headers, long idle_timeout_sec);
    ~HttpResponseStream() override;

    HttpResponseStream(const HttpResponseStream &) = delete;
    HttpResponseStream & operator=(const HttpResponseStream &) = delete;

    // Wait until the body starts or the transfer ends; the status and the response
    // headers are known after it succeeds.
    CURLcode waitForResponse(std::string & error_message);

    // Read the rest of the body onto the end of `body`.
    CURLcode readToEnd(std::vector<uint8_t> & body, std::string & error_message);

    // Stop the transfer and take back the handles, their options kept, to send the
    // request again.
    PooledCurl detach();

    CURL * handle() const { return handles->easy; }

    Status read(uint8_t * buffer, size_t size, size_t & bytes_read) override;

private:
    static size_t onBodyBytes(char * ptr, size_t size, size_t nmemb, void * userdata);

    // Wait for body bytes beyond those received, or for the end of the transfer.
    CURLcode receive(std::string & error_message);

    PooledCurl handles;
    curl_slist * request_headers;
    long idle_timeout_sec;
    std::vector<uint8_t> received;
    size_t consumed = 0;
    bool finished = false;
    CURLcode transfer_result = CURLE_OK;
};

} // namespace firebolt::adbc
