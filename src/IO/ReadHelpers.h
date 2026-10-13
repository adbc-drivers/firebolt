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
#include <string>
#include <vector>

#include "IO/IReadBuffer.h"
#include "Status.h"

namespace firebolt::adbc
{

// Fill `buffer` with `size` bytes from `in`, fewer only at its end.
Status readFully(IReadBuffer & in, uint8_t * buffer, size_t size, size_t & bytes_read);

// Append the rest of `in` to `out`.
Status readToEnd(IReadBuffer & in, std::vector<uint8_t> & out);

// Append the rest of `in` to `out`, stopping once `out` holds `max_size` bytes;
// what is left past that stays unread.
Status readToEnd(IReadBuffer & in, std::string & out, size_t max_size);

// Read and drop the rest of `in`.
Status skipToEnd(IReadBuffer & in);

} // namespace firebolt::adbc
