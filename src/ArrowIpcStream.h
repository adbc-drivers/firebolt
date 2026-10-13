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

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Status.h"
#include "adbc.h" // for ArrowArrayStream

namespace firebolt::adbc
{

// The body of a query response, read front to back.  Implemented over a body
// already in memory and over one still arriving (QueryResponseStream).
class ResponseBodySource
{
public:
    virtual ~ResponseBodySource() = default;

    // Copy up to `size` bytes of the body into `buffer`, waiting for at least one if
    // none has arrived yet.  `bytes_read` is 0 only at the end of the body.
    virtual Status read(uint8_t * buffer, size_t size, size_t & bytes_read) = 0;
};

// Export a response body of Arrow IPC stream bytes as an ArrowArrayStream (Arrow C
// Data Interface), decoding each record batch as it is asked for.  The schema is
// read before this returns.  An empty body (DDL, zero-row result) produces an empty
// stream.  A query that fails after its response started ends the body with the
// error text in place of the next message; reading that message fails with the text.
// Returns a non-empty error string on failure.
std::string ExportIpcBodyAsArrowStream(std::unique_ptr<ResponseBodySource> body, ArrowArrayStream * out);

// The same, for a body already read into memory.
std::string ExportIpcBytesAsArrowStream(std::vector<uint8_t> ipc_bytes, ArrowArrayStream * out);

// Decode several independent Arrow IPC responses and expose their record batches
// as one stream, in response order. Returns an error if their schemas differ.
std::string ExportIpcResponsesAsArrowStream(std::vector<std::vector<uint8_t>> ipc_responses, ArrowArrayStream * out);

} // namespace firebolt::adbc
