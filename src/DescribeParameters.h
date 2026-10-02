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
#include <string_view>
#include <utility>
#include <vector>

namespace firebolt::adbc
{

// Reading of Firebolt's `execution_mode=describe_parameters` response: the types of a
// statement's `$N` placeholders, without executing it.  One row, one string column,
// holding JSON:
//
//   {"result_columns":[{"name":"n","type":"integer"}],"parameter_types":{"$1":"integer"}}
//
// `result_columns` is populated only for SELECT, `parameter_types` only when the
// statement references placeholders.

// The parameter types of a describe response, in the order they were found.
using ParameterTypeList = std::vector<std::pair<std::string, std::string>>;

// Extract the `parameter_types` member of a describe response.  Parsed structurally,
// not searched for by key: a result column may itself be named `parameter_types`.
//
// No `parameter_types` member yields an empty list, as a statement without
// placeholders produces.  Order is nlohmann/json's — sorted by name, so `$10` before
// `$2`; buildParameterSchema restores ordinal order.
AdbcStatusCode extractParameterTypes(std::string_view describe_json, ParameterTypeList & out, std::string & out_error);

// Map a Firebolt type name to an Arrow type, initialising `out` (an uninitialised
// ArrowSchema owned by the caller).  Case-insensitive; handles an optional ` null`
// suffix, `decimal(p, s)` / `numeric(p, s)`, and `array(...)` to any depth.
//
// An unrecognised name yields Arrow's null type, which ADBC prescribes for a
// parameter whose type cannot be determined.
AdbcStatusCode fireboltTypeNameToArrowSchema(std::string_view type_name, ArrowSchema * out, std::string & out_error);

// Build the ADBC parameter schema — a struct schema, one field per parameter — from a
// describe response.
//
// Ordered by the *numeric* index in each `$N` name, so ten or more parameters keep
// ordinal order (as text, `$10` sorts before `$2`).  A name that is not `$<digits>`
// sorts last, keeping `param('name')` parameters present.
AdbcStatusCode buildParameterSchema(std::string_view describe_json, ArrowSchema * out, std::string & out_error);

} // namespace firebolt::adbc
