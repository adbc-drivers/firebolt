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

namespace firebolt::adbc
{

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

Status skipToEnd(IReadBuffer & in)
{
    uint8_t chunk[16 * 1024];
    for (;;)
    {
        size_t received = 0;
        if (Status status = in.read(chunk, sizeof(chunk), received); !status.ok() || received == 0)
            return status;
    }
}

} // namespace firebolt::adbc
