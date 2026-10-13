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

#include "IO/IReadBuffer.h"
#include "adbc.h" // for ArrowArrayStream

namespace firebolt::adbc
{

// Export a response body of Arrow IPC stream bytes as an ArrowArrayStream (Arrow C
// Data Interface), decoding each record batch as it is asked for.  The schema is
// read before this returns.  An empty body (DDL, zero-row result) produces an empty
// stream.  A query that fails after its response started ends the body with the
// error text in place of the next message; reading that message fails with the text.
Status ExportIpcReadBufferAsArrowStream(std::unique_ptr<IReadBuffer> body, ArrowArrayStream * out);

// The same, for a body already read into memory.
Status ExportIpcBytesAsArrowStream(std::vector<uint8_t> ipc_bytes, ArrowArrayStream * out);

// Decode several independent Arrow IPC responses and expose their record batches
// as one stream, in response order. Fails if their schemas differ.
Status ExportIpcResponsesAsArrowStream(std::vector<std::vector<uint8_t>> ipc_responses, ArrowArrayStream * out);

} // namespace firebolt::adbc
