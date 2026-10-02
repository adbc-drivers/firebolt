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

#include "adbc.h"

#include <nanoarrow/nanoarrow.h>

#include <string>

namespace firebolt::adbc
{

// Rendering of bound Arrow data into Firebolt's `query_parameters` setting: how the
// server substitutes `$1`, `$2`, … and feeds the `param('name')` function.
//
// The setting is a JSON array of `{"name": …, "value": …}`.  A parameter's SQL type
// is the JSON type of its value — `null` an untyped SQL NULL, a boolean a BOOLEAN, an
// integer a BIGINT, any other number a DOUBLE, a string TEXT.  There is no type key,
// which is why DATE / TIMESTAMP / NUMERIC travel as strings and coerce at the use
// site.
//
// The server's query validator turns `$N` into a literal node of the validated AST
// rather than splicing text into SQL, so the only escaping that matters is JSON
// escaping, which nlohmann/json does.

// Build the `query_parameters` value for the parameter set at `row` of `batch_view`,
// a struct view over one record batch of the bound data.
//
// Every parameter is named by position — `$1`, `$2`, … in bound-column order — which
// is what the statement's placeholders resolve against.  `bind_by_name` carries each
// under its column's name *as well*, for `param('name')`; both namings go out,
// because an unreferenced parameter is ignored by the server while a stale
// `bind_by_name` emitting names alone would leave `$N` unbound.
//
// Example: one int64 column named "who" holding 41
//   positionally      [{"name":"$1","value":41}]
//   also by name      [{"name":"$1","value":41},{"name":"who","value":41}]
//
// On failure `out_error` holds a user-facing message, and the status separates a type
// this mechanism cannot express (ADBC_STATUS_NOT_IMPLEMENTED) from a value that does
// not fit (ADBC_STATUS_INVALID_ARGUMENT).
AdbcStatusCode buildQueryParametersJson(
    const ArrowSchema * schema,
    const ArrowArrayView * batch_view,
    int64_t row,
    bool bind_by_name,
    std::string & out_json,
    std::string & out_error);

} // namespace firebolt::adbc
