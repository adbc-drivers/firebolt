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

#include "FireboltAdbcConnection.h"
#include "adbc.h"

namespace firebolt::adbc
{

AdbcStatusCode
ConnectionGetInfo(FireboltConnection * conn, const uint32_t * info_codes, size_t info_codes_len, ArrowArrayStream * out, AdbcError * error);

AdbcStatusCode ConnectionGetTableTypes(FireboltConnection * conn, ArrowArrayStream * out, AdbcError * error);

AdbcStatusCode ConnectionGetTableSchema(
    FireboltConnection * conn, const char * catalog, const char * db_schema, const char * table_name, ArrowSchema * out, AdbcError * error);

AdbcStatusCode ConnectionGetObjects(
    FireboltConnection * conn,
    int depth,
    const char * catalog,
    const char * db_schema,
    const char * table_name,
    const char ** table_type,
    const char * column_name,
    ArrowArrayStream * out,
    AdbcError * error);

} // namespace firebolt::adbc
