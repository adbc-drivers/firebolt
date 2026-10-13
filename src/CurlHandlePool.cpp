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


#include "CurlHandlePool.h"

#include <stdexcept>

namespace firebolt::adbc
{

CurlHandles::CurlHandles() : multi(curl_multi_init()), easy(curl_easy_init())
{
    if (!multi || !easy)
    {
        curl_multi_cleanup(multi);
        curl_easy_cleanup(easy);
        throw std::runtime_error("Failed to initialize curl handles");
    }
}

CurlHandles::~CurlHandles()
{
    curl_multi_cleanup(multi);
    curl_easy_cleanup(easy);
}

void PooledCurlDeleter::operator()(CurlHandles * handles) const
{
    std::unique_ptr<CurlHandles> owned(handles);
    if (auto alive = pool.lock())
        alive->giveBack(std::move(owned));
}

PooledCurl CurlHandlePool::acquire()
{
    std::unique_ptr<CurlHandles> handles;
    {
        std::lock_guard lock(mutex);
        handles = std::move(idle);
    }
    if (handles)
        curl_easy_reset(handles->easy);
    else
        handles = std::make_unique<CurlHandles>();
    return PooledCurl(handles.release(), PooledCurlDeleter{weak_from_this()});
}

void CurlHandlePool::giveBack(std::unique_ptr<CurlHandles> handles)
{
    std::lock_guard lock(mutex);
    if (!idle)
        idle = std::move(handles);
}

} // namespace firebolt::adbc
