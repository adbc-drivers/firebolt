#pragma once

#include <nanoarrow/nanoarrow.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace firebolt::adbc
{

struct FireboltConnection;

enum class IngestMode
{
    Append,
    Create,
    Replace,
    CreateAppend,
};

// Per-execution transient state populated by Bind / BindStream and the
// ADBC_INGEST_OPTION_* statement options, then consumed by ExecuteQuery on the
// bulk-ingest path.  Lives inside std::optional on FireboltStatement so the
// presence of the optional itself signals "ingest setup in progress"; success
// in ExecuteQuery resets it.
struct BulkIngestState
{
    std::vector<uint8_t> ipc_bytes;
    // Deep copy of the bound stream's schema captured during Bind/BindStream.
    // Used to extract column names and types for auto-generated INSERT /
    // CREATE TABLE statements.
    nanoarrow::UniqueSchema schema;

    std::string target_table;
    std::string target_catalog;
    std::string target_db_schema;
    IngestMode mode = IngestMode::Append;
};

struct FireboltStatement
{
    FireboltConnection * conn = nullptr; // borrowed
    std::string sql;
    std::optional<BulkIngestState> ingest;
};

} // namespace firebolt::adbc
