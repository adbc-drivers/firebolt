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
#include <cstddef>
#include <cstdint>
#include <limits>

#include "IO/IReadBuffer.h"
#include "Status.h"

namespace firebolt::adbc
{

// Fill `buffer` with `size` bytes from `in`, fewer only at its end.
Status readFully(IReadBuffer & in, uint8_t * buffer, size_t size, size_t & bytes_read);

// Append the rest of `in` to `out` (a std::string or std::vector<uint8_t>),
// stopping once `out` holds `max_size` bytes; what is left past that stays unread.
template <typename Bytes>
Status readToEnd(IReadBuffer & in, Bytes & out, size_t max_size = std::numeric_limits<size_t>::max())
{
    constexpr size_t chunk_size = 64 * 1024;
    while (out.size() < max_size)
    {
        const size_t old_size = out.size();
        const size_t wanted = (std::min)(chunk_size, max_size - old_size);
        out.resize(old_size + wanted);
        size_t received = 0;
        Status status = in.read(reinterpret_cast<uint8_t *>(out.data()) + old_size, wanted, received);
        out.resize(old_size + received);
        if (!status.ok() || received == 0)
            return status;
    }
    return {};
}

// Read and drop the rest of `in`.
Status skipToEnd(IReadBuffer & in);

} // namespace firebolt::adbc
