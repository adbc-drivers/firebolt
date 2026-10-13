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
    constexpr std::chrono::milliseconds MAX_POLL_WAIT{1000};

    // A longer timeout is no limit in practice; the cap keeps the deadline from overflowing.
    constexpr long MAX_IDLE_TIMEOUT_SEC = 365L * 24 * 3600;

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
    : handles(std::move(handles))
    , request_headers(request_headers)
    , idle_timeout_sec((std::min)(idle_timeout_sec, MAX_IDLE_TIMEOUT_SEC))
    , response_id(++this->handles->last_response_id)
{
    CURL * easy = this->handles->easy;
    this->handles->holder = this;
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, request_headers);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, onBodyBytes);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, this);
    if (CURLMcode code = curl_multi_add_handle(this->handles->multi, easy); code != CURLM_OK)
        abandon(curlError(code));
}

ReadBufferFromHttpResponse::~ReadBufferFromHttpResponse()
{
    releaseHandles();
}

void ReadBufferFromHttpResponse::releaseHandles()
{
    if (handles->holder != this)
        return;
    // An unfinished transfer is aborted, and its HTTP connection closed.
    curl_multi_remove_handle(handles->multi, handles->easy);
    curl_easy_setopt(handles->easy, CURLOPT_HTTPHEADER, nullptr);
    curl_slist_free_all(request_headers);
    request_headers = nullptr;
    handles->holder = nullptr;
}

void ReadBufferFromHttpResponse::abandon(Status reason)
{
    abandoned = std::move(reason);
    releaseHandles();
}

size_t ReadBufferFromHttpResponse::onBodyBytes(char * ptr, size_t size, size_t nmemb, void * userdata)
{
    auto * stream = static_cast<ReadBufferFromHttpResponse *>(userdata);
    const size_t total = size * nmemb;
    try
    {
        const size_t to_caller = (std::min)(total, stream->caller_buffer_size - stream->caller_bytes);
        if (to_caller > 0)
        {
            memcpy(stream->caller_buffer + stream->caller_bytes, ptr, to_caller);
            stream->caller_bytes += to_caller;
        }
        stream->received.insert(stream->received.end(), ptr + to_caller, ptr + total);
    }
    catch (std::exception &)
    {
        return 0; // no exception may unwind through curl: fail the transfer instead
    }
    return total;
}

// receiveMoreBody() loops until new body bytes arrive or the transfer ends.  Curl
// has no background thread: all network work happens inside these calls, on the
// caller's thread.
//   - curl_multi_perform() does whatever network work is ready without blocking:
//     connecting, sending the request, reading the socket.
//   - onBodyBytes() is called by curl from inside curl_multi_perform() with each
//     piece of the body, and copies it into read()'s buffer, the rest that does
//     not fit into `received`.
//   - curl_multi_info_read() reports when the transfer has finished;
//     releaseHandles() then gives the curl handles back to the connection.
//   - curl_multi_poll() sleeps until the socket is ready again, then the loop
//     calls curl_multi_perform() once more.
// Since the socket is only read while a reader waits here, a slow consumer slows
// the server down instead of filling memory.  Any failure releases the handles,
// so a failed response never keeps the connection busy.
Status ReadBufferFromHttpResponse::receiveMoreBody()
{
    if (!abandoned.ok())
        return abandoned;
    if (consumed == received.size())
    {
        received.clear();
        consumed = 0;
    }
    const size_t already_received = received.size() + caller_bytes;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(idle_timeout_sec);
    while (!finished && received.size() + caller_bytes == already_received)
    {
        int running = 0;
        if (CURLMcode code = curl_multi_perform(handles->multi, &running); code != CURLM_OK)
        {
            abandon(curlError(code));
            return abandoned;
        }
        int queued = 0;
        while (CURLMsg * message = curl_multi_info_read(handles->multi, &queued))
        {
            if (message->msg == CURLMSG_DONE)
            {
                finished = true;
                transfer_result = message->data.result;
            }
        }
        // The rest of the body has been received: the connection is free again,
        // though the caller may not have read all of it yet.
        if (finished)
            releaseHandles();
        if (finished || received.size() + caller_bytes != already_received)
            break;

        std::chrono::milliseconds wait = MAX_POLL_WAIT;
        if (idle_timeout_sec > 0)
        {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0)
            {
                abandon(
                    {ADBC_STATUS_TIMEOUT, "No data from the server for " + std::to_string(idle_timeout_sec) + " s (firebolt.timeout_sec)"});
                return abandoned;
            }
            wait = (std::min)(wait, remaining);
        }
        if (CURLMcode code = curl_multi_poll(handles->multi, nullptr, 0, static_cast<int>(wait.count()), nullptr); code != CURLM_OK)
        {
            abandon(curlError(code));
            return abandoned;
        }
    }
    // Bytes that arrived with the end of a failed transfer are read first; the
    // failure is reported once they are.
    if (finished && transfer_result != CURLE_OK && received.size() == consumed && caller_bytes == 0)
        return curlError(transfer_result);
    return {};
}

Status ReadBufferFromHttpResponse::read(uint8_t * buffer, size_t size, size_t & bytes_read)
{
    bytes_read = 0;
    if (!abandoned.ok())
        return abandoned;
    if (consumed == received.size())
    {
        caller_buffer = buffer;
        caller_buffer_size = size;
        caller_bytes = 0;
        Status status = receiveMoreBody();
        if (status.ok())
            bytes_read = caller_bytes;
        caller_buffer = nullptr;
        caller_buffer_size = 0;
        caller_bytes = 0;
        if (!status.ok() || bytes_read > 0)
            return status;
    }
    bytes_read = (std::min)(size, received.size() - consumed);
    memcpy(buffer, received.data() + consumed, bytes_read);
    consumed += bytes_read;
    return {};
}

} // namespace firebolt::adbc
