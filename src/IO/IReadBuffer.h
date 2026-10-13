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

#include <cstddef>
#include <cstdint>

#include "Status.h"

namespace firebolt::adbc
{

// A byte stream read front to back, such as the body of a query response.
// Implementations: ReadBufferFromMemory, ReadBufferFromHttpResponse.
class IReadBuffer
{
public:
    virtual ~IReadBuffer() = default;

    // Copy up to `size` bytes into `buffer`, waiting for at least one if none is
    // available yet.  `bytes_read` is 0 only at the end of the stream.
    virtual Status read(uint8_t * buffer, size_t size, size_t & bytes_read) = 0;
};

} // namespace firebolt::adbc
