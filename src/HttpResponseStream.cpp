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


#include "HttpResponseStream.h"

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

    std::string curlError(CURLcode code)
    {
        return std::string("curl error: ") + curl_easy_strerror(code);
    }

} // namespace

HttpResponseStream::HttpResponseStream(PooledCurl handles, curl_slist * request_headers, long idle_timeout_sec)
    : handles(std::move(handles)), request_headers(request_headers), idle_timeout_sec(idle_timeout_sec)
{
    CURL * easy = this->handles->easy;
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, request_headers);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, onBodyBytes);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, this);
    if (curl_multi_add_handle(this->handles->multi, easy) != CURLM_OK)
    {
        curl_slist_free_all(request_headers);
        throw std::runtime_error("Failed to start an HTTP request");
    }
}

HttpResponseStream::~HttpResponseStream()
{
    detach();
}

PooledCurl HttpResponseStream::detach()
{
    if (handles)
    {
        // An unfinished transfer is aborted, and its connection closed.
        curl_multi_remove_handle(handles->multi, handles->easy);
        curl_easy_setopt(handles->easy, CURLOPT_HTTPHEADER, nullptr);
    }
    curl_slist_free_all(request_headers);
    request_headers = nullptr;
    return std::move(handles);
}

size_t HttpResponseStream::onBodyBytes(char * ptr, size_t size, size_t nmemb, void * userdata)
{
    auto * stream = static_cast<HttpResponseStream *>(userdata);
    const size_t total = size * nmemb;
    stream->received.insert(stream->received.end(), ptr, ptr + total);
    return total;
}

CURLcode HttpResponseStream::receive(std::string & error_message)
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
        {
            error_message = std::string("curl error: ") + curl_multi_strerror(code);
            return CURLE_RECV_ERROR;
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
        if (finished || received.size() != already_received)
            break;

        int wait_ms = MAX_POLL_MS;
        if (idle_timeout_sec > 0)
        {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0)
            {
                error_message = "No data from the server for " + std::to_string(idle_timeout_sec) + " s (firebolt.timeout_sec)";
                return CURLE_OPERATION_TIMEDOUT;
            }
            wait_ms = static_cast<int>(std::min<long long>(remaining.count(), MAX_POLL_MS));
        }
        if (CURLMcode code = curl_multi_poll(handles->multi, nullptr, 0, wait_ms, nullptr); code != CURLM_OK)
        {
            error_message = std::string("curl error: ") + curl_multi_strerror(code);
            return CURLE_RECV_ERROR;
        }
    }
    if (finished && transfer_result != CURLE_OK)
    {
        error_message = curlError(transfer_result);
        return transfer_result;
    }
    return CURLE_OK;
}

CURLcode HttpResponseStream::waitForResponse(std::string & error_message)
{
    return receive(error_message);
}

CURLcode HttpResponseStream::readToEnd(std::vector<uint8_t> & body, std::string & error_message)
{
    for (;;)
    {
        body.insert(body.end(), received.begin() + static_cast<std::ptrdiff_t>(consumed), received.end());
        consumed = received.size();
        if (CURLcode code = receive(error_message); code != CURLE_OK)
            return code;
        if (finished && consumed == received.size())
            return CURLE_OK;
    }
}

Status HttpResponseStream::read(uint8_t * buffer, size_t size, size_t & bytes_read)
{
    bytes_read = 0;
    if (consumed == received.size())
    {
        std::string error_message;
        if (receive(error_message) != CURLE_OK)
            return {ADBC_STATUS_IO, error_message};
    }
    bytes_read = std::min(size, received.size() - consumed);
    memcpy(buffer, received.data() + consumed, bytes_read);
    consumed += bytes_read;
    return {};
}

} // namespace firebolt::adbc
