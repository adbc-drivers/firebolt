#pragma once

#include <string>
#include <vector>

namespace firebolt::adbc
{

struct FireboltConnection;

struct FireboltStatement
{
    FireboltConnection * conn = nullptr; // borrowed
    std::string sql;
    std::vector<uint8_t> bound_ipc_bytes;
    bool has_bind_data = false;
    // For bulk ingestion via ADBC_INGEST_OPTION_TARGET_TABLE:
    std::string ingest_target_table;
    std::vector<std::string> bound_column_names; // column names from bound ArrowSchema
};

} // namespace firebolt::adbc
