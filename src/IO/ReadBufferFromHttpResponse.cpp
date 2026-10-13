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


#include "ReadBufferFromHttpResponse.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>

namespace firebolt::adbc
{

namespace
{

    // The longest single wait in curl_multi_poll, so a deadline is checked often.
    constexpr int MAX_POLL_MS = 1000;

    Status curlError(CURLcode code)
    {
        const AdbcStatusCode status_code = code == CURLE_OPERATION_TIMEDOUT ? ADBC_STATUS_TIMEOUT : ADBC_STATUS_IO;
        return {status_code, std::string("curl error: ") + curl_easy_strerror(code)};
    }

    Status curlError(CURLMcode code)
    {
        return {ADBC_STATUS_IO, std::string("curl error: ") + curl_multi_strerror(code)};
    }

} // namespace

ReadBufferFromHttpResponse::ReadBufferFromHttpResponse(
    std::shared_ptr<CurlHandles> handles, curl_slist * request_headers, long idle_timeout_sec)
    : handles(std::move(handles)), request_headers(request_headers), idle_timeout_sec(idle_timeout_sec)
{
    CURL * easy = this->handles->easy;
    this->handles->in_use = true;
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, request_headers);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, onBodyBytes);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, this);
    if (curl_multi_add_handle(this->handles->multi, easy) != CURLM_OK)
    {
        this->handles->in_use = false;
        curl_slist_free_all(request_headers);
        throw std::runtime_error("Failed to start an HTTP request");
    }
}

ReadBufferFromHttpResponse::~ReadBufferFromHttpResponse()
{
    releaseHandles();
}

void ReadBufferFromHttpResponse::releaseHandles()
{
    if (!holds_handles)
        return;
    holds_handles = false;
    // An unfinished transfer is aborted, and its HTTP connection closed.
    curl_multi_remove_handle(handles->multi, handles->easy);
    curl_easy_setopt(handles->easy, CURLOPT_HTTPHEADER, nullptr);
    curl_slist_free_all(request_headers);
    request_headers = nullptr;
    handles->in_use = false;
}

size_t ReadBufferFromHttpResponse::onBodyBytes(char * ptr, size_t size, size_t nmemb, void * userdata)
{
    auto * stream = static_cast<ReadBufferFromHttpResponse *>(userdata);
    const size_t total = size * nmemb;
    stream->received.insert(stream->received.end(), ptr, ptr + total);
    return total;
}

// receive() loops until new body bytes arrive or the transfer ends.  Curl has no
// background thread: all network work happens inside these calls, on the caller's
// thread.
//   - curl_multi_perform() does whatever network work is ready without blocking:
//     connecting, sending the request, reading the socket.
//   - onBodyBytes() is called by curl from inside curl_multi_perform() with each
//     piece of the body, and appends it to `received`.
//   - curl_multi_info_read() reports when the transfer has finished;
//     releaseHandles() then gives the curl handles back to the connection.
//   - curl_multi_poll() sleeps until the socket is ready again, then the loop
//     calls curl_multi_perform() once more.
// Since the socket is only read while a reader waits here, a slow consumer slows
// the server down instead of filling memory.
Status ReadBufferFromHttpResponse::receive()
{
    if (consumed == received.size())
    {
        received.clear();
        consumed = 0;
    }
    const size_t already_received = received.size();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(idle_timeout_sec);
    while (!finished && received.size() == already_received)
    {
        int running = 0;
        if (CURLMcode code = curl_multi_perform(handles->multi, &running); code != CURLM_OK)
            return curlError(code);
        int queued = 0;
        while (CURLMsg * message = curl_multi_info_read(handles->multi, &queued))
        {
            if (message->msg == CURLMSG_DONE)
            {
                finished = true;
                transfer_result = message->data.result;
            }
        }
        // The rest of the body is in `received`: the connection is free again,
        // though the caller may not have read it yet.
        if (finished)
            releaseHandles();
        if (finished || received.size() != already_received)
            break;

        int wait_ms = MAX_POLL_MS;
        if (idle_timeout_sec > 0)
        {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0)
                return {
                    ADBC_STATUS_TIMEOUT, "No data from the server for " + std::to_string(idle_timeout_sec) + " s (firebolt.timeout_sec)"};
            wait_ms = static_cast<int>(std::min<long long>(remaining.count(), MAX_POLL_MS));
        }
        if (CURLMcode code = curl_multi_poll(handles->multi, nullptr, 0, wait_ms, nullptr); code != CURLM_OK)
            return curlError(code);
    }
    if (finished && transfer_result != CURLE_OK)
        return curlError(transfer_result);
    return {};
}

Status ReadBufferFromHttpResponse::waitForResponse()
{
    return receive();
}

Status ReadBufferFromHttpResponse::readToEnd(std::vector<uint8_t> & body)
{
    for (;;)
    {
        body.insert(body.end(), received.begin() + static_cast<std::ptrdiff_t>(consumed), received.end());
        consumed = received.size();
        if (Status status = receive(); !status.ok())
            return status;
        if (finished && consumed == received.size())
            return {};
    }
}

Status ReadBufferFromHttpResponse::read(uint8_t * buffer, size_t size, size_t & bytes_read)
{
    bytes_read = 0;
    if (consumed == received.size())
    {
        if (Status status = receive(); !status.ok())
            return status;
    }
    bytes_read = (std::min)(size, received.size() - consumed);
    memcpy(buffer, received.data() + consumed, bytes_read);
    consumed += bytes_read;
    return {};
}

} // namespace firebolt::adbc
