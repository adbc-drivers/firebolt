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

#include <memory>
#include <mutex>

#include <curl/curl.h>

namespace firebolt::adbc
{

// An easy handle and the multi handle that runs its transfers.  The multi handle
// holds the HTTP connection open from one request to the next.
struct CurlHandles
{
    CURLM * multi = nullptr;
    CURL * easy = nullptr;

    CurlHandles();
    ~CurlHandles();
    CurlHandles(const CurlHandles &) = delete;
    CurlHandles & operator=(const CurlHandles &) = delete;
};

class CurlHandlePool;

// Gives handles back to their pool, or frees them if the pool is gone.
struct PooledCurlDeleter
{
    std::weak_ptr<CurlHandlePool> pool;
    void operator()(CurlHandles * handles) const;
};

using PooledCurl = std::unique_ptr<CurlHandles, PooledCurlDeleter>;

// The curl handles of one connection.  A request reuses the idle handles, and
// with them the open HTTP connection; a request made while a result stream still
// holds them gets fresh ones.
class CurlHandlePool : public std::enable_shared_from_this<CurlHandlePool>
{
public:
    // Handles with every easy option at its default.  Throws if curl cannot make them.
    PooledCurl acquire();

private:
    friend struct PooledCurlDeleter;
    void giveBack(std::unique_ptr<CurlHandles> handles);

    std::mutex mutex;
    // One is kept: a connection has one request in flight but for open streams.
    std::unique_ptr<CurlHandles> idle;
};

} // namespace firebolt::adbc
