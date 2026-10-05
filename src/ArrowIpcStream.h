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
#include <string>
#include <vector>

#include "adbc.h" // for ArrowArrayStream

namespace firebolt::adbc
{

// Given raw Arrow IPC stream bytes returned by Firebolt's HTTP API, export
// them as an ArrowArrayStream (Arrow C Data Interface).
// On empty body (DDL / zero-row result), produces an empty stream.
// Returns a non-empty error string on failure.
std::string ExportIpcBytesAsArrowStream(std::vector<uint8_t> ipc_bytes, ArrowArrayStream * out);

// Decode several independent Arrow IPC responses and expose their record batches
// as one stream, in response order. Returns an error if their schemas differ.
std::string ExportIpcResponsesAsArrowStream(std::vector<std::vector<uint8_t>> ipc_responses, ArrowArrayStream * out);

} // namespace firebolt::adbc
