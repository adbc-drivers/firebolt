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
