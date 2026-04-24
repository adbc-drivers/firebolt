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

} // namespace firebolt::adbc
