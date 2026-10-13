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


#include "IO/ReadHelpers.h"

#include <algorithm>

namespace firebolt::adbc
{

namespace
{

    // How much each read step asks for.
    constexpr size_t CHUNK_SIZE = 64 * 1024;

} // namespace

Status readFully(IReadBuffer & in, uint8_t * buffer, size_t size, size_t & bytes_read)
{
    bytes_read = 0;
    while (bytes_read < size)
    {
        size_t received = 0;
        if (Status status = in.read(buffer + bytes_read, size - bytes_read, received); !status.ok())
            return status;
        if (received == 0)
            break;
        bytes_read += received;
    }
    return {};
}

Status readToEnd(IReadBuffer & in, std::vector<uint8_t> & out)
{
    for (;;)
    {
        const size_t old_size = out.size();
        out.resize(old_size + CHUNK_SIZE);
        size_t received = 0;
        Status status = in.read(out.data() + old_size, CHUNK_SIZE, received);
        out.resize(old_size + received);
        if (!status.ok() || received == 0)
            return status;
    }
}

Status readToEnd(IReadBuffer & in, std::string & out, size_t max_size)
{
    while (out.size() < max_size)
    {
        const size_t old_size = out.size();
        const size_t wanted = (std::min)(CHUNK_SIZE, max_size - old_size);
        out.resize(old_size + wanted);
        size_t received = 0;
        Status status = in.read(reinterpret_cast<uint8_t *>(out.data()) + old_size, wanted, received);
        out.resize(old_size + received);
        if (!status.ok() || received == 0)
            return status;
    }
    return {};
}

Status skipToEnd(IReadBuffer & in)
{
    uint8_t chunk[16 * 1024]; // on the stack, so smaller than CHUNK_SIZE
    for (;;)
    {
        size_t received = 0;
        if (Status status = in.read(chunk, sizeof(chunk), received); !status.ok() || received == 0)
            return status;
    }
}

} // namespace firebolt::adbc
