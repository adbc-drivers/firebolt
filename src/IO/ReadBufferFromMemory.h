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
#include <utility>
#include <vector>

#include "IO/IReadBuffer.h"

namespace firebolt::adbc
{

// Bytes already in memory, such as a response body read whole.
class ReadBufferFromMemory final : public IReadBuffer
{
public:
    explicit ReadBufferFromMemory(std::vector<uint8_t> bytes) : bytes(std::move(bytes)) { }

    Status read(uint8_t * buffer, size_t size, size_t & bytes_read) override
    {
        bytes_read = (std::min)(size, bytes.size() - offset);
        memcpy(buffer, bytes.data() + offset, bytes_read);
        offset += bytes_read;
        return {};
    }

private:
    std::vector<uint8_t> bytes;
    size_t offset = 0;
};

} // namespace firebolt::adbc
