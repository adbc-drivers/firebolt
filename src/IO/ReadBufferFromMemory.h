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

#include <algorithm>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include "IO/IReadBuffer.h"

namespace firebolt::adbc
{

// Bytes already in memory: owned, such as a response body read whole, or
// borrowed, such as an upload payload.
class ReadBufferFromMemory final : public IReadBuffer
{
public:
    explicit ReadBufferFromMemory(std::vector<uint8_t> owned_bytes) : owned(std::move(owned_bytes)), bytes(owned) { }
    // `borrowed` must outlive this buffer.
    explicit ReadBufferFromMemory(std::span<const uint8_t> borrowed) : bytes(borrowed) { }

    ReadBufferFromMemory(const ReadBufferFromMemory &) = delete;
    ReadBufferFromMemory & operator=(const ReadBufferFromMemory &) = delete;

    // Read from `position` next; false, and nothing changes, if it is past the end.
    bool seek(size_t position)
    {
        if (position > bytes.size())
            return false;
        offset = position;
        return true;
    }

    Status read(uint8_t * buffer, size_t size, size_t & bytes_read) override
    {
        bytes_read = std::min(size, bytes.size() - offset);
        memcpy(buffer, bytes.data() + offset, bytes_read);
        offset += bytes_read;
        return {};
    }

private:
    std::vector<uint8_t> owned;
    std::span<const uint8_t> bytes;
    size_t offset = 0;
};

} // namespace firebolt::adbc
