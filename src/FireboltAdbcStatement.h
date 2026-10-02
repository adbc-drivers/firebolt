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

// Per-execution state from Bind / BindStream and the ADBC_INGEST_OPTION_* options,
// consumed and reset by ExecuteQuery.  Held in a std::optional whose presence means
// "data or an ingest target was supplied".
//
// Two destinations, told apart by whether an ingest target table is set:
//   * with a target — a bulk-ingest payload, uploaded as multipart Arrow IPC;
//   * without one — query parameters in the `query_parameters` setting, one
//     execution per bound row.
struct BoundData
{
    // The bound record batches, kept as Arrow data.  Only the ingest path sends IPC,
    // and it encodes them at execution time; the parameter path reads the values
    // straight out of these arrays.  Encoding at bind time would cost a round trip
    // the parameter path never uses, and would refuse any layout nanoarrow's IPC
    // writer cannot encode — Arrow's view layouts among them.
    std::vector<nanoarrow::UniqueArray> batches;

    // Whether Bind/BindStream supplied data.  Distinct from a non-empty `batches`: a
    // stream yielding no batches is still bound data, and ingesting it uploads a
    // schema-only payload — a create-mode ingest of it makes an empty table.
    bool data_bound = false;

    // Deep copy of the bound schema, captured by Bind/BindStream.  Supplies column
    // names and types for the generated INSERT / CREATE TABLE, and the logical types
    // the bound values are read with.
    nanoarrow::UniqueSchema schema;

    std::string target_table;
    std::string target_catalog;
    std::string target_db_schema;
    // ADBC's default: "Whether to create (the default) or append" (adbc.h,
    // ADBC_INGEST_OPTION_MODE).  Only a low-level caller can leave it unset —
    // dbapi's Cursor.adbc_ingest() always sends a mode.
    IngestMode mode = IngestMode::Create;

    // Whether any of mode / target_catalog / target_db_schema was set.  Recorded
    // rather than inferred, since the mode's value cannot say whether it was set
    // explicitly.  Without a target table these options mean nothing, and
    // ExecuteQuery refuses that combination rather than discarding them.
    bool ingest_options_set = false;
};

struct FireboltStatement
{
    FireboltConnection * conn = nullptr; // borrowed
    std::string sql;
    std::optional<BoundData> bound;

    // "adbc.statement.bind_by_name": carry each parameter under its bound column's
    // name as well as its positional `$N`, so `param('name')` resolves.
    //
    // Outside BoundData, and untouched by SetSqlQuery, because it must outlive the
    // payload it applies to: a driver manager sends it only when its own idea of the
    // setting changes (dbapi tracks it per cursor), so resetting it here would
    // silently revert to positional naming.  It can therefore be stale, which is why
    // it only ever *adds* a naming — see buildQueryParametersJson.
    bool bind_by_name = false;

    BoundData & initAndGetBoundData()
    {
        if (!bound)
            bound.emplace();
        return bound.value();
    }
};

} // namespace firebolt::adbc
