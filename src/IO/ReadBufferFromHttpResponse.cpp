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
#include <mutex>
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
    : handles(std::move(handles))
    , request_headers(request_headers)
    , idle_timeout_sec(idle_timeout_sec)
    , response_id(++this->handles->last_response_id)
{
    CURL * easy = this->handles->easy;
    {
        std::lock_guard handles_lock(this->handles->mutex);
        this->handles->holder = this;
    }
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, request_headers);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, onBodyBytes);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, this);
    if (CURLMcode code = curl_multi_add_handle(this->handles->multi, easy); code != CURLM_OK)
    {
        std::lock_guard reading_lock(reading);
        abandonLocked(curlError(code));
    }
}

ReadBufferFromHttpResponse::~ReadBufferFromHttpResponse()
{
    releaseHandles();
}

void ReadBufferFromHttpResponse::releaseHandles()
{
    std::lock_guard handles_lock(handles->mutex);
    releaseHandlesWhileLocked();
}

void ReadBufferFromHttpResponse::releaseHandlesWhileLocked()
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

void ReadBufferFromHttpResponse::abandonLocked(Status reason)
{
    abandoned = std::move(reason);
    releaseHandles();
}

bool ReadBufferFromHttpResponse::tryAbandonWhileLocked(Status reason)
{
    std::unique_lock reading_lock(reading, std::try_to_lock);
    if (!reading_lock.owns_lock())
        return false;
    abandoned = std::move(reason);
    releaseHandlesWhileLocked();
    return true;
}

size_t ReadBufferFromHttpResponse::onBodyBytes(char * ptr, size_t size, size_t nmemb, void * userdata)
{
    auto * stream = static_cast<ReadBufferFromHttpResponse *>(userdata);
    const size_t total = size * nmemb;
    try
    {
        std::vector<uint8_t> & target = stream->sink ? *stream->sink : stream->received;
        target.insert(target.end(), ptr, ptr + total);
        stream->bytes_received += total;
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
//     piece of the body, and appends it to `received` (or readToEnd's `sink`).
//   - curl_multi_info_read() reports when the transfer has finished;
//     releaseHandles() then gives the curl handles back to the connection.
//   - curl_multi_poll() sleeps until the socket is ready again, then the loop
//     calls curl_multi_perform() once more.
// Since the socket is only read while a reader waits here, a slow consumer slows
// the server down instead of filling memory.  Any failure releases the handles,
// so a failed response never keeps the connection busy.
Status ReadBufferFromHttpResponse::receiveMoreBody()
{
    std::lock_guard reading_lock(reading);
    return receiveMoreBodyLocked();
}

Status ReadBufferFromHttpResponse::receiveMoreBodyLocked()
{
    if (!abandoned.ok())
        return abandoned;
    if (consumed == received.size())
    {
        received.clear();
        consumed = 0;
    }
    const size_t already_received = bytes_received;
    // Whole seconds, compared as such: a huge timeout must not overflow.
    const auto started = std::chrono::steady_clock::now();
    while (!finished && bytes_received == already_received)
    {
        int running = 0;
        if (CURLMcode code = curl_multi_perform(handles->multi, &running); code != CURLM_OK)
        {
            abandonLocked(curlError(code));
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
        // The rest of the body is in `received`: the connection is free again,
        // though the caller may not have read it yet.
        if (finished)
            releaseHandles();
        if (finished || bytes_received != already_received)
            break;

        long long wait_ms = MAX_POLL_MS;
        if (idle_timeout_sec > 0)
        {
            const auto waited = std::chrono::steady_clock::now() - started;
            const long long waited_sec = std::chrono::duration_cast<std::chrono::seconds>(waited).count();
            if (waited_sec >= idle_timeout_sec)
            {
                abandonLocked(
                    {ADBC_STATUS_TIMEOUT, "No data from the server for " + std::to_string(idle_timeout_sec) + " s (firebolt.timeout_sec)"});
                return abandoned;
            }
            if (idle_timeout_sec - waited_sec <= 1)
            {
                const long long waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(waited).count();
                wait_ms = (std::min)(wait_ms, idle_timeout_sec * 1000 - waited_ms);
            }
        }
        if (CURLMcode code = curl_multi_poll(handles->multi, nullptr, 0, static_cast<int>(wait_ms), nullptr); code != CURLM_OK)
        {
            abandonLocked(curlError(code));
            return abandoned;
        }
    }
    // Bytes that arrived with the end of a failed transfer are read first; the
    // failure is reported once they are.
    if (finished && transfer_result != CURLE_OK && received.size() == consumed)
        return curlError(transfer_result);
    return {};
}

Status ReadBufferFromHttpResponse::readToEnd(std::vector<uint8_t> & body)
{
    std::lock_guard reading_lock(reading);
    if (!abandoned.ok())
        return abandoned;
    body.insert(body.end(), received.begin() + static_cast<std::ptrdiff_t>(consumed), received.end());
    consumed = received.size();
    sink = &body;
    Status status;
    do
        status = receiveMoreBodyLocked();
    while (status.ok() && !finished);
    sink = nullptr;
    return status;
}

Status ReadBufferFromHttpResponse::read(uint8_t * buffer, size_t size, size_t & bytes_read)
{
    std::lock_guard reading_lock(reading);
    bytes_read = 0;
    if (!abandoned.ok())
        return abandoned;
    if (consumed == received.size())
    {
        if (Status status = receiveMoreBodyLocked(); !status.ok())
            return status;
    }
    bytes_read = (std::min)(size, received.size() - consumed);
    memcpy(buffer, received.data() + consumed, bytes_read);
    consumed += bytes_read;
    return {};
}

} // namespace firebolt::adbc
