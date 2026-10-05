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

#include "FireboltAdbcMetadata.h"
#include "ArrowIpcStream.h"
#include "HttpClient.h"
#include "IngestSqlBuilder.h"
#include "StringUtils.h"
#include "Version.h" // generated from src/Version.h.in

#include <nanoarrow/nanoarrow.hpp>
#include <nanoarrow/nanoarrow_ipc.hpp>

#include <cstring>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace firebolt::adbc
{

// ============================================================
// Shared helpers (local to this TU)
// ============================================================

static AdbcStatusCode SetError(AdbcError * e, AdbcStatusCode code, const std::string & msg)
{
    if (e)
    {
        e->message = duplicateString(msg);
        e->release = [](AdbcError * err) {
            free(err->message);
            err->message = nullptr;
            err->release = nullptr;
        };
        e->vendor_code = 0;
    }
    return code;
}

// Build an ArrowStringView from a C string.
static inline ArrowStringView sv(const char * s)
{
    return {s, static_cast<int64_t>(strlen(s))};
}
static inline ArrowStringView sv(const std::string & s)
{
    return {s.data(), static_cast<int64_t>(s.size())};
}

// Export a single Arrow batch as a one-batch ArrowArrayStream.
// Takes ownership of the schema and batch (via ArrowSchemaMove/ArrowArrayMove).
static void ExportBatchAsStream(ArrowSchema * schema, ArrowArray * batch, ArrowArrayStream * out)
{
    ArrowBasicArrayStreamInit(out, schema, 1);
    ArrowBasicArrayStreamSetArray(out, 0, batch);
}

// ============================================================
// Flat query result reader
// ============================================================
// Reads all rows from an Arrow IPC stream into a vector of rows,
// each row being a vector of optional<string> (one per column).
// Column names are returned in col_names.

struct FlatResult
{
    std::vector<std::string> col_names;
    std::vector<std::vector<std::optional<std::string>>> rows;
};

static std::optional<std::string> viewGetStr(const ArrowArrayView * av, int64_t i)
{
    if (ArrowArrayViewIsNull(av, i))
        return std::nullopt;
    switch (av->storage_type)
    {
        case NANOARROW_TYPE_STRING:
        case NANOARROW_TYPE_LARGE_STRING: {
            ArrowStringView s = ArrowArrayViewGetStringUnsafe(av, i);
            return std::string(s.data, s.size_bytes);
        }
        case NANOARROW_TYPE_INT8:
        case NANOARROW_TYPE_INT16:
        case NANOARROW_TYPE_INT32:
        case NANOARROW_TYPE_INT64:
        case NANOARROW_TYPE_UINT8:
        case NANOARROW_TYPE_UINT16:
        case NANOARROW_TYPE_UINT32:
        case NANOARROW_TYPE_UINT64:
            return std::to_string(ArrowArrayViewGetIntUnsafe(av, i));
        default:
            return std::nullopt;
    }
}

static std::optional<int32_t> viewGetInt32(const ArrowArrayView * av, int64_t i)
{
    if (ArrowArrayViewIsNull(av, i))
        return std::nullopt;
    return static_cast<int32_t>(ArrowArrayViewGetIntUnsafe(av, i));
}

// Execute SQL and read all result rows into a FlatResult.
static std::string executeAndRead(FireboltConnection * conn, const std::string & sql, FlatResult & result)
{
    auto resp = conn->http->executeQuery(sql, conn->session_params);
    // Honour server-advertised session updates on success — keeps metadata-path
    // session-state behaviour symmetric with the SQL-execution path so a server
    // reset after a GetObjects call is not silently dropped.
    applySessionUpdatesIfSuccess(conn->session_params, resp);
    if (!resp.isSuccess())
        return resp.error_message.empty() ? "HTTP error " + std::to_string(resp.http_code) : resp.error_message;
    if (resp.body.empty())
        return {};

    ArrowArrayStream stream{};
    std::string err = ExportIpcBytesAsArrowStream(std::move(resp.body), &stream);
    if (!err.empty())
        return err;

    // Get schema.
    ArrowSchema schema{};
    if (stream.get_schema(&stream, &schema) != 0)
    {
        if (stream.release)
            stream.release(&stream);
        return "get_schema failed";
    }

    int n_cols = static_cast<int>(schema.n_children);
    result.col_names.clear();
    for (int c = 0; c < n_cols; c++)
        result.col_names.push_back(schema.children[c]->name ? schema.children[c]->name : "");

    // Read batches.
    ArrowArray batch{};
    while (stream.get_next(&stream, &batch) == 0 && batch.release != nullptr)
    {
        // Build per-column views.
        std::vector<ArrowArrayView> views(n_cols);
        for (int c = 0; c < n_cols; c++)
        {
            memset(&views[c], 0, sizeof(ArrowArrayView));
            ArrowArrayViewInitFromSchema(&views[c], schema.children[c], nullptr);
            ArrowArrayViewSetArray(&views[c], batch.children[c], nullptr);
        }

        for (int64_t r = 0; r < batch.length; r++)
        {
            std::vector<std::optional<std::string>> row(n_cols);
            for (int c = 0; c < n_cols; c++)
                row[c] = viewGetStr(&views[c], r);
            result.rows.push_back(std::move(row));
        }

        for (int c = 0; c < n_cols; c++)
            ArrowArrayViewReset(&views[c]);

        batch.release(&batch);
    }

    if (stream.release)
        stream.release(&stream);
    if (schema.release)
        schema.release(&schema);

    return {};
}

// Find the index of a named column in a FlatResult (-1 if not found).
static int colIdx(const FlatResult & r, const std::string & name)
{
    for (int i = 0; i < static_cast<int>(r.col_names.size()); i++)
        if (r.col_names[i] == name)
            return i;
    return -1;
}

static std::optional<std::string>
getCol(const FlatResult & r, const std::vector<std::optional<std::string>> & row, const std::string & name)
{
    int i = colIdx(r, name);
    if (i < 0 || i >= static_cast<int>(row.size()))
        return std::nullopt;
    return row[i];
}

static AdbcStatusCode queryVendorVersion(FireboltConnection * conn, std::string & version, AdbcError * error)
{
    if (!conn || !conn->http)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "GetInfo: connection not initialized");

    FlatResult result;
    std::string query_error = executeAndRead(conn, "SELECT version()", result);
    if (!query_error.empty())
        return SetError(error, ADBC_STATUS_IO, "GetInfo: failed to query vendor version: " + query_error);

    if (result.col_names.size() != 1 || result.rows.size() != 1 || result.rows.front().size() != 1 || !result.rows.front().front()
        || result.rows.front().front()->empty())
        return SetError(error, ADBC_STATUS_INVALID_DATA, "GetInfo: SELECT version() did not return one non-empty value");

    version = *result.rows.front().front();
    return ADBC_STATUS_OK;
}

// ============================================================
// ConnectionGetInfo
// ============================================================
//
// Schema: struct<info_name: uint32, info_value: dense_union<
//   string_value(0): utf8,
//   bool_value(1): bool,
//   int64_value(2): int64,
//   int32_bitmask(3): int32,
//   string_list(4): list<utf8>,
//   int32_to_int32_list_map(5): map<int32, list<int32>>
// >>

static int buildGetInfoSchema(ArrowSchema * schema)
{
    if (ArrowSchemaInitFromType(schema, NANOARROW_TYPE_STRUCT) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaAllocateChildren(schema, 2) != NANOARROW_OK)
        return -1;
    ArrowSchemaInit(schema->children[0]);
    ArrowSchemaInit(schema->children[1]);

    // children[0]: info_name (uint32)
    if (ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_UINT32) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetName(schema->children[0], "info_name") != NANOARROW_OK)
        return -1;

    // children[1]: info_value (dense union with 6 members, type IDs 0-5)
    if (ArrowSchemaSetTypeUnion(schema->children[1], NANOARROW_TYPE_DENSE_UNION, 6) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetName(schema->children[1], "info_value") != NANOARROW_OK)
        return -1;

    // Union child 0: string_value (utf8)
    if (ArrowSchemaSetType(schema->children[1]->children[0], NANOARROW_TYPE_STRING) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetName(schema->children[1]->children[0], "string_value") != NANOARROW_OK)
        return -1;

    // Union child 1: bool_value (bool)
    if (ArrowSchemaSetType(schema->children[1]->children[1], NANOARROW_TYPE_BOOL) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetName(schema->children[1]->children[1], "bool_value") != NANOARROW_OK)
        return -1;

    // Union child 2: int64_value (int64)
    if (ArrowSchemaSetType(schema->children[1]->children[2], NANOARROW_TYPE_INT64) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetName(schema->children[1]->children[2], "int64_value") != NANOARROW_OK)
        return -1;

    // Union child 3: int32_bitmask (int32)
    if (ArrowSchemaSetType(schema->children[1]->children[3], NANOARROW_TYPE_INT32) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetName(schema->children[1]->children[3], "int32_bitmask") != NANOARROW_OK)
        return -1;

    // Union child 4: string_list (list<utf8>)
    if (ArrowSchemaSetType(schema->children[1]->children[4], NANOARROW_TYPE_LIST) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetName(schema->children[1]->children[4], "string_list") != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetType(schema->children[1]->children[4]->children[0], NANOARROW_TYPE_STRING) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetName(schema->children[1]->children[4]->children[0], "item") != NANOARROW_OK)
        return -1;

    // Union child 5: int32_to_int32_list_map (map<int32, list<int32>>)
    // ArrowSchemaSetType(MAP) auto-creates:
    //   children[0] = "entries" struct (not nullable) with children[0]="key", children[1]="value"
    // We only need to set the types of key and value; names/structure already set.
    if (ArrowSchemaSetType(schema->children[1]->children[5], NANOARROW_TYPE_MAP) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetName(schema->children[1]->children[5], "int32_to_int32_list_map") != NANOARROW_OK)
        return -1;
    ArrowSchema * entries = schema->children[1]->children[5]->children[0];
    // entries is already a struct named "entries" with key/value children — just set their types.
    if (ArrowSchemaSetType(entries->children[0], NANOARROW_TYPE_INT32) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetType(entries->children[1], NANOARROW_TYPE_LIST) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetType(entries->children[1]->children[0], NANOARROW_TYPE_INT32) != NANOARROW_OK)
        return -1;

    return 0;
}

// Append one info row.  type_id selects the union branch (0=string,1=bool,2=int64).
static int appendInfoRow(ArrowArray * batch, uint32_t code, int8_t type_id, const std::string & str_val, bool bool_val, int64_t int_val)
{
    if (ArrowArrayAppendInt(batch->children[0], code) != NANOARROW_OK)
        return -1;

    ArrowArray * union_arr = batch->children[1];
    if (type_id == 0)
    {
        if (ArrowArrayAppendString(union_arr->children[0], sv(str_val)) != NANOARROW_OK)
            return -1;
    }
    else if (type_id == 1)
    {
        if (ArrowArrayAppendInt(union_arr->children[1], bool_val ? 1 : 0) != NANOARROW_OK)
            return -1;
    }
    else if (type_id == 2)
    {
        if (ArrowArrayAppendInt(union_arr->children[2], int_val) != NANOARROW_OK)
            return -1;
    }
    else
    {
        return -1;
    }

    if (ArrowArrayFinishUnionElement(union_arr, type_id) != NANOARROW_OK)
        return -1;
    if (ArrowArrayFinishElement(batch) != NANOARROW_OK)
        return -1;
    return 0;
}

AdbcStatusCode
ConnectionGetInfo(FireboltConnection * conn, const uint32_t * info_codes, size_t info_codes_len, ArrowArrayStream * out, AdbcError * error)
{
    // Build the set of requested codes (empty = return all).
    std::unordered_set<uint32_t> requested;
    if (info_codes && info_codes_len > 0)
        for (size_t i = 0; i < info_codes_len; i++)
            requested.insert(info_codes[i]);
    auto want = [&](uint32_t code) { return requested.empty() || requested.count(code); };

    std::string vendor_version;
    if (want(ADBC_INFO_VENDOR_VERSION))
    {
        AdbcStatusCode status = queryVendorVersion(conn, vendor_version, error);
        if (status != ADBC_STATUS_OK)
            return status;
    }

    // Build schema.
    nanoarrow::UniqueSchema schema;
    if (buildGetInfoSchema(schema.get()) != 0)
        return SetError(error, ADBC_STATUS_INTERNAL, "GetInfo: failed to build schema");

    // Build batch.
    nanoarrow::UniqueArray batch;
    if (ArrowArrayInitFromSchema(batch.get(), schema.get(), nullptr) != NANOARROW_OK)
        return SetError(error, ADBC_STATUS_INTERNAL, "GetInfo: ArrowArrayInitFromSchema failed");
    if (ArrowArrayStartAppending(batch.get()) != NANOARROW_OK)
        return SetError(error, ADBC_STATUS_INTERNAL, "GetInfo: ArrowArrayStartAppending failed");

    // Static info entries.
    // type_id: 0=string, 1=bool, 2=int64
    struct Entry
    {
        uint32_t code;
        int8_t type_id;
        std::string str_val;
        bool bool_val;
        int64_t int_val;
    };
    std::vector<Entry> entries = {
        {ADBC_INFO_VENDOR_NAME, 0, "Firebolt", false, 0},
        {ADBC_INFO_VENDOR_VERSION, 0, vendor_version, false, 0},
        {ADBC_INFO_VENDOR_SQL, 1, "", true, 0},
        {ADBC_INFO_VENDOR_SUBSTRAIT, 1, "", false, 0},
        {ADBC_INFO_DRIVER_NAME, 0, "ADBC Driver for Firebolt", false, 0},
        {ADBC_INFO_DRIVER_VERSION, 0, FIREBOLT_ADBC_VERSION, false, 0},
        {ADBC_INFO_DRIVER_ARROW_VERSION, 0, "v" + std::string(ArrowNanoarrowVersion()), false, 0},
        {ADBC_INFO_DRIVER_ADBC_VERSION, 2, "", false, ADBC_VERSION_1_1_0},
    };

    for (const auto & e : entries)
    {
        if (!want(e.code))
            continue;
        if (appendInfoRow(batch.get(), e.code, e.type_id, e.str_val, e.bool_val, e.int_val) != 0)
            return SetError(error, ADBC_STATUS_INTERNAL, "GetInfo: failed to append row for code " + std::to_string(e.code));
    }

    ArrowError na_err{};
    if (ArrowArrayFinishBuilding(batch.get(), NANOARROW_VALIDATION_LEVEL_DEFAULT, &na_err) != NANOARROW_OK)
        return SetError(error, ADBC_STATUS_INTERNAL, std::string("GetInfo: ArrowArrayFinishBuilding: ") + na_err.message);

    ExportBatchAsStream(schema.get(), batch.get(), out);
    return ADBC_STATUS_OK;
}

// ============================================================
// ConnectionGetTableTypes
// ============================================================
// Schema: struct<table_type: utf8 not null>

AdbcStatusCode ConnectionGetTableTypes(FireboltConnection * /*conn*/, ArrowArrayStream * out, AdbcError * error)
{
    nanoarrow::UniqueSchema schema;
    if (ArrowSchemaInitFromType(schema.get(), NANOARROW_TYPE_STRUCT) != NANOARROW_OK
        || ArrowSchemaAllocateChildren(schema.get(), 1) != NANOARROW_OK)
        return SetError(error, ADBC_STATUS_INTERNAL, "GetTableTypes: failed to build schema");
    ArrowSchemaInit(schema->children[0]);
    if (ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_STRING) != NANOARROW_OK
        || ArrowSchemaSetName(schema->children[0], "table_type") != NANOARROW_OK)
        return SetError(error, ADBC_STATUS_INTERNAL, "GetTableTypes: failed to build schema");

    nanoarrow::UniqueArray batch;
    if (ArrowArrayInitFromSchema(batch.get(), schema.get(), nullptr) != NANOARROW_OK
        || ArrowArrayStartAppending(batch.get()) != NANOARROW_OK)
        return SetError(error, ADBC_STATUS_INTERNAL, "GetTableTypes: failed to init array");

    for (const char * tt : {"BASE TABLE", "VIEW", "LOCAL TEMPORARY"})
    {
        if (ArrowArrayAppendString(batch->children[0], sv(tt)) != NANOARROW_OK || ArrowArrayFinishElement(batch.get()) != NANOARROW_OK)
            return SetError(error, ADBC_STATUS_INTERNAL, "GetTableTypes: failed to append");
    }

    if (ArrowArrayFinishBuilding(batch.get(), NANOARROW_VALIDATION_LEVEL_DEFAULT, nullptr) != NANOARROW_OK)
        return SetError(error, ADBC_STATUS_INTERNAL, "GetTableTypes: ArrowArrayFinishBuilding failed");

    ExportBatchAsStream(schema.get(), batch.get(), out);
    return ADBC_STATUS_OK;
}

// ============================================================
// ConnectionGetTableSchema
// ============================================================

AdbcStatusCode ConnectionGetTableSchema(
    FireboltConnection * conn, const char * catalog, const char * db_schema, const char * table_name, ArrowSchema * out, AdbcError * error)
{
    if (!table_name || !*table_name)
        return SetError(error, ADBC_STATUS_INVALID_ARGUMENT, "GetTableSchema: table_name is required");

    auto resp = conn->http->executeQuery(
        buildTableSchemaSql(catalog ? catalog : "", db_schema ? db_schema : "", table_name), conn->session_params);
    applySessionUpdatesIfSuccess(conn->session_params, resp);
    if (!resp.isSuccess())
    {
        const bool relation_not_found = resp.curl_code == CURLE_OK && resp.http_code >= 400 && resp.http_code < 500
            && resp.error_message.find("relation") != std::string::npos && resp.error_message.find("does not exist") != std::string::npos;
        const AdbcStatusCode status = relation_not_found
            ? ADBC_STATUS_NOT_FOUND
            : (resp.http_code >= 400 && resp.http_code < 500 ? ADBC_STATUS_INVALID_ARGUMENT : ADBC_STATUS_IO);
        return SetError(error, status, "GetTableSchema: " + resp.error_message);
    }

    ArrowArrayStream stream{};
    std::string err = ExportIpcBytesAsArrowStream(std::move(resp.body), &stream);
    if (!err.empty())
        return SetError(error, ADBC_STATUS_INTERNAL, "GetTableSchema: " + err);

    int rc = stream.get_schema(&stream, out);
    if (stream.release)
        stream.release(&stream);
    if (rc != 0)
        return SetError(error, ADBC_STATUS_INTERNAL, "GetTableSchema: get_schema failed");

    return ADBC_STATUS_OK;
}

// ============================================================
// ConnectionGetObjects
// ============================================================
//
// Output schema (see ADBC spec):
//   struct<
//     catalog_name: utf8,
//     catalog_db_schemas: list<struct<
//       db_schema_name: utf8,
//       db_schema_tables: list<struct<
//         table_name: utf8 not null,
//         table_type: utf8 not null,
//         table_columns: list<COLUMN_SCHEMA>,
//         table_constraints: list<CONSTRAINT_SCHEMA>   (always null here)
//       >>
//     >>
//   >
//
// COLUMN_SCHEMA has 19 fields; we populate the essential ones.

static int buildGetObjectsSchema(ArrowSchema * top)
{
    // Top-level struct<catalog_name, catalog_db_schemas>
    if (ArrowSchemaInitFromType(top, NANOARROW_TYPE_STRUCT) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaAllocateChildren(top, 2) != NANOARROW_OK)
        return -1;

    ArrowSchemaInit(top->children[0]);
    ArrowSchemaInit(top->children[1]);

    // Field 0: catalog_name utf8
    if (ArrowSchemaSetType(top->children[0], NANOARROW_TYPE_STRING) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetName(top->children[0], "catalog_name") != NANOARROW_OK)
        return -1;

    // Field 1: catalog_db_schemas list<struct<db_schema_name, db_schema_tables>>
    if (ArrowSchemaSetType(top->children[1], NANOARROW_TYPE_LIST) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetName(top->children[1], "catalog_db_schemas") != NANOARROW_OK)
        return -1;

    // List item: struct<db_schema_name, db_schema_tables>
    ArrowSchema * schema_struct = top->children[1]->children[0];
    if (ArrowSchemaSetTypeStruct(schema_struct, 2) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetName(schema_struct, "item") != NANOARROW_OK)
        return -1;

    // schema_struct->children[0]: db_schema_name utf8
    if (ArrowSchemaSetType(schema_struct->children[0], NANOARROW_TYPE_STRING) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetName(schema_struct->children[0], "db_schema_name") != NANOARROW_OK)
        return -1;

    // schema_struct->children[1]: db_schema_tables list<struct<...>>
    if (ArrowSchemaSetType(schema_struct->children[1], NANOARROW_TYPE_LIST) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetName(schema_struct->children[1], "db_schema_tables") != NANOARROW_OK)
        return -1;

    // Tables list item: struct<table_name, table_type, table_columns, table_constraints>
    ArrowSchema * table_struct = schema_struct->children[1]->children[0];
    if (ArrowSchemaSetTypeStruct(table_struct, 4) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetName(table_struct, "item") != NANOARROW_OK)
        return -1;

    // table_struct->children[0]: table_name utf8
    if (ArrowSchemaSetType(table_struct->children[0], NANOARROW_TYPE_STRING) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetName(table_struct->children[0], "table_name") != NANOARROW_OK)
        return -1;

    // table_struct->children[1]: table_type utf8
    if (ArrowSchemaSetType(table_struct->children[1], NANOARROW_TYPE_STRING) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetName(table_struct->children[1], "table_type") != NANOARROW_OK)
        return -1;

    // table_struct->children[2]: table_columns list<COLUMN_SCHEMA (19 fields)>
    if (ArrowSchemaSetType(table_struct->children[2], NANOARROW_TYPE_LIST) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetName(table_struct->children[2], "table_columns") != NANOARROW_OK)
        return -1;

    ArrowSchema * col_struct = table_struct->children[2]->children[0];
    if (ArrowSchemaSetTypeStruct(col_struct, 19) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetName(col_struct, "item") != NANOARROW_OK)
        return -1;

    // Column schema fields (ADBC spec order):
    const struct
    {
        const char * name;
        ArrowType type;
    } col_fields[19] = {
        {"column_name", NANOARROW_TYPE_STRING},
        {"ordinal_position", NANOARROW_TYPE_INT32},
        {"remarks", NANOARROW_TYPE_STRING},
        {"xdbc_data_type", NANOARROW_TYPE_INT16},
        {"xdbc_type_name", NANOARROW_TYPE_STRING},
        {"xdbc_column_size", NANOARROW_TYPE_INT32},
        {"xdbc_decimal_digits", NANOARROW_TYPE_INT16},
        {"xdbc_num_prec_radix", NANOARROW_TYPE_INT16},
        {"xdbc_nullable", NANOARROW_TYPE_INT16},
        {"xdbc_column_def", NANOARROW_TYPE_STRING},
        {"xdbc_sql_data_type", NANOARROW_TYPE_INT16},
        {"xdbc_datetime_sub", NANOARROW_TYPE_INT16},
        {"xdbc_char_octet_length", NANOARROW_TYPE_INT32},
        {"xdbc_is_nullable", NANOARROW_TYPE_STRING},
        {"xdbc_scope_catalog", NANOARROW_TYPE_STRING},
        {"xdbc_scope_schema", NANOARROW_TYPE_STRING},
        {"xdbc_scope_table", NANOARROW_TYPE_STRING},
        {"xdbc_is_autoincrement", NANOARROW_TYPE_BOOL},
        {"xdbc_is_generatedcolumn", NANOARROW_TYPE_BOOL},
    };
    for (int i = 0; i < 19; i++)
    {
        if (ArrowSchemaSetType(col_struct->children[i], col_fields[i].type) != NANOARROW_OK)
            return -1;
        if (ArrowSchemaSetName(col_struct->children[i], col_fields[i].name) != NANOARROW_OK)
            return -1;
    }

    // table_struct->children[3]: table_constraints list<CONSTRAINT_SCHEMA (4 fields)>
    if (ArrowSchemaSetType(table_struct->children[3], NANOARROW_TYPE_LIST) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetName(table_struct->children[3], "table_constraints") != NANOARROW_OK)
        return -1;

    ArrowSchema * constraint_struct = table_struct->children[3]->children[0];
    if (ArrowSchemaSetTypeStruct(constraint_struct, 4) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetName(constraint_struct, "item") != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetType(constraint_struct->children[0], NANOARROW_TYPE_STRING) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetName(constraint_struct->children[0], "constraint_name") != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetType(constraint_struct->children[1], NANOARROW_TYPE_STRING) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetName(constraint_struct->children[1], "constraint_type") != NANOARROW_OK)
        return -1;
    // constraint_column_names: list<utf8>
    if (ArrowSchemaSetType(constraint_struct->children[2], NANOARROW_TYPE_LIST) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetName(constraint_struct->children[2], "constraint_column_names") != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetType(constraint_struct->children[2]->children[0], NANOARROW_TYPE_STRING) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetName(constraint_struct->children[2]->children[0], "item") != NANOARROW_OK)
        return -1;
    // constraint_column_usage: list<USAGE_SCHEMA> — minimal schema
    if (ArrowSchemaSetType(constraint_struct->children[3], NANOARROW_TYPE_LIST) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetName(constraint_struct->children[3], "constraint_column_usage") != NANOARROW_OK)
        return -1;
    ArrowSchema * usage_struct = constraint_struct->children[3]->children[0];
    if (ArrowSchemaSetTypeStruct(usage_struct, 4) != NANOARROW_OK)
        return -1;
    if (ArrowSchemaSetName(usage_struct, "item") != NANOARROW_OK)
        return -1;
    for (int i = 0; i < 4; i++)
    {
        const char * names[4] = {"fk_catalog", "fk_db_schema", "fk_table", "fk_column_name"};
        if (ArrowSchemaSetType(usage_struct->children[i], NANOARROW_TYPE_STRING) != NANOARROW_OK)
            return -1;
        if (ArrowSchemaSetName(usage_struct->children[i], names[i]) != NANOARROW_OK)
            return -1;
    }

    return 0;
}

// Data structures for in-memory grouping.
struct ColumnRow
{
    std::string column_name;
    int32_t ordinal_position{0};
    std::string data_type;
    std::string is_nullable; // "YES" or "NO"
    std::optional<std::string> column_default;
    std::optional<int32_t> char_max_length;
    std::optional<int32_t> char_octet_length;
    std::optional<int32_t> num_precision;
    std::optional<int32_t> num_scale;
    std::optional<int32_t> num_prec_radix;
};

struct TableRow
{
    std::string table_name;
    std::string table_type;
    std::vector<ColumnRow> columns;
};

struct SchemaRow
{
    std::string schema_name;
    std::vector<TableRow> tables;
};

struct CatalogRow
{
    std::string catalog_name;
    std::vector<SchemaRow> schemas;
};

// Append one COLUMN_SCHEMA struct row to the column_struct array.
// col_arr must be column_struct (19 children).
static int appendColumn(ArrowArray * col_struct, const ColumnRow & col)
{
    // 0: column_name
    if (ArrowArrayAppendString(col_struct->children[0], sv(col.column_name)) != NANOARROW_OK)
        return -1;
    // 1: ordinal_position
    if (ArrowArrayAppendInt(col_struct->children[1], col.ordinal_position) != NANOARROW_OK)
        return -1;
    // 2: remarks (NULL)
    if (ArrowArrayAppendNull(col_struct->children[2], 1) != NANOARROW_OK)
        return -1;
    // 3: xdbc_data_type (NULL)
    if (ArrowArrayAppendNull(col_struct->children[3], 1) != NANOARROW_OK)
        return -1;
    // 4: xdbc_type_name
    if (!col.data_type.empty())
    {
        if (ArrowArrayAppendString(col_struct->children[4], sv(col.data_type)) != NANOARROW_OK)
            return -1;
    }
    else
    {
        if (ArrowArrayAppendNull(col_struct->children[4], 1) != NANOARROW_OK)
            return -1;
    }
    // 5: xdbc_column_size (char_max_length or num_precision)
    {
        auto sz = col.char_max_length.has_value() ? col.char_max_length : col.num_precision;
        if (sz.has_value())
        {
            if (ArrowArrayAppendInt(col_struct->children[5], *sz) != NANOARROW_OK)
                return -1;
        }
        else
        {
            if (ArrowArrayAppendNull(col_struct->children[5], 1) != NANOARROW_OK)
                return -1;
        }
    }
    // 6: xdbc_decimal_digits (num_scale as int16)
    if (col.num_scale.has_value())
    {
        if (ArrowArrayAppendInt(col_struct->children[6], *col.num_scale) != NANOARROW_OK)
            return -1;
    }
    else
    {
        if (ArrowArrayAppendNull(col_struct->children[6], 1) != NANOARROW_OK)
            return -1;
    }
    // 7: xdbc_num_prec_radix
    if (col.num_prec_radix.has_value())
    {
        if (ArrowArrayAppendInt(col_struct->children[7], *col.num_prec_radix) != NANOARROW_OK)
            return -1;
    }
    else
    {
        if (ArrowArrayAppendNull(col_struct->children[7], 1) != NANOARROW_OK)
            return -1;
    }
    // 8: xdbc_nullable (1=nullable, 0=not nullable)
    {
        int16_t nullable = (col.is_nullable == "YES") ? 1 : 0;
        if (ArrowArrayAppendInt(col_struct->children[8], nullable) != NANOARROW_OK)
            return -1;
    }
    // 9: xdbc_column_def
    if (col.column_default.has_value())
    {
        if (ArrowArrayAppendString(col_struct->children[9], sv(*col.column_default)) != NANOARROW_OK)
            return -1;
    }
    else
    {
        if (ArrowArrayAppendNull(col_struct->children[9], 1) != NANOARROW_OK)
            return -1;
    }
    // 10-12: xdbc_sql_data_type, xdbc_datetime_sub (NULL); xdbc_char_octet_length
    if (ArrowArrayAppendNull(col_struct->children[10], 1) != NANOARROW_OK)
        return -1;
    if (ArrowArrayAppendNull(col_struct->children[11], 1) != NANOARROW_OK)
        return -1;
    if (col.char_octet_length.has_value())
    {
        if (ArrowArrayAppendInt(col_struct->children[12], *col.char_octet_length) != NANOARROW_OK)
            return -1;
    }
    else
    {
        if (ArrowArrayAppendNull(col_struct->children[12], 1) != NANOARROW_OK)
            return -1;
    }
    // 13: xdbc_is_nullable ("YES"/"NO")
    if (!col.is_nullable.empty())
    {
        if (ArrowArrayAppendString(col_struct->children[13], sv(col.is_nullable)) != NANOARROW_OK)
            return -1;
    }
    else
    {
        if (ArrowArrayAppendNull(col_struct->children[13], 1) != NANOARROW_OK)
            return -1;
    }
    // 14-16: xdbc_scope_* (NULL)
    for (int i = 14; i <= 16; i++)
        if (ArrowArrayAppendNull(col_struct->children[i], 1) != NANOARROW_OK)
            return -1;
    // 17-18: xdbc_is_autoincrement, xdbc_is_generatedcolumn (NULL)
    if (ArrowArrayAppendNull(col_struct->children[17], 1) != NANOARROW_OK)
        return -1;
    if (ArrowArrayAppendNull(col_struct->children[18], 1) != NANOARROW_OK)
        return -1;

    if (ArrowArrayFinishElement(col_struct) != NANOARROW_OK)
        return -1;
    return 0;
}

// Append one TABLE_SCHEMA struct row (with its columns list) to the table_struct array.
static int appendTable(ArrowArray * table_struct_arr, const TableRow & tbl, bool include_columns)
{
    // children[0]: table_name
    if (ArrowArrayAppendString(table_struct_arr->children[0], sv(tbl.table_name)) != NANOARROW_OK)
        return -1;
    // children[1]: table_type
    if (ArrowArrayAppendString(table_struct_arr->children[1], sv(tbl.table_type)) != NANOARROW_OK)
        return -1;

    // children[2]: table_columns (list<COLUMN_SCHEMA>)
    ArrowArray * cols_list = table_struct_arr->children[2];
    ArrowArray * col_struct = cols_list->children[0];
    if (include_columns && !tbl.columns.empty())
    {
        for (const auto & col : tbl.columns)
            if (appendColumn(col_struct, col) != 0)
                return -1;
    }
    if (ArrowArrayFinishElement(cols_list) != NANOARROW_OK) // finish the list
        return -1;

    // children[3]: table_constraints — always null list
    if (ArrowArrayAppendNull(table_struct_arr->children[3], 1) != NANOARROW_OK)
        return -1;

    if (ArrowArrayFinishElement(table_struct_arr) != NANOARROW_OK)
        return -1;
    return 0;
}

// Add optional WHERE clause filter for an exact-match string column.
static void addFilter(std::string & sql, const char * col, const char * val, bool & first_filter)
{
    if (!val)
        return;
    sql += first_filter ? " WHERE " : " AND ";
    first_filter = false;
    // Use LIKE to support % wildcards; quote single quotes in value.
    std::string escaped;
    for (char c : std::string(val))
    {
        if (c == '\'')
            escaped += "''";
        else
            escaped += c;
    }
    sql += std::string(col) + " LIKE '" + escaped + "'";
}

AdbcStatusCode ConnectionGetObjects(
    FireboltConnection * conn,
    int depth,
    const char * catalog_filter,
    const char * db_schema_filter,
    const char * table_name_filter,
    const char ** table_type_filter,
    const char * column_name_filter,
    ArrowArrayStream * out,
    AdbcError * error)
{
    const bool include_tables = (depth == ADBC_OBJECT_DEPTH_ALL || depth >= ADBC_OBJECT_DEPTH_TABLES);
    const bool include_columns = (depth == ADBC_OBJECT_DEPTH_ALL || depth >= ADBC_OBJECT_DEPTH_COLUMNS);

    // ---- Query information_schema.tables ----
    std::string tables_sql = "SELECT table_catalog, table_schema, table_name, table_type FROM information_schema.tables";
    bool first = true;
    addFilter(tables_sql, "table_catalog", catalog_filter, first);
    addFilter(tables_sql, "table_schema", db_schema_filter, first);
    if (include_tables)
        addFilter(tables_sql, "table_name", table_name_filter, first);

    // table_type filter (multi-value OR).
    if (table_type_filter && table_type_filter[0])
    {
        tables_sql += first ? " WHERE " : " AND ";
        tables_sql += "table_type IN (";
        for (int i = 0; table_type_filter[i]; i++)
        {
            if (i > 0)
                tables_sql += ", ";
            tables_sql += "'";
            for (char c : std::string(table_type_filter[i]))
            {
                if (c == '\'')
                    tables_sql += "''";
                else
                    tables_sql += c;
            }
            tables_sql += "'";
        }
        tables_sql += ")";
    }
    tables_sql += " ORDER BY table_catalog, table_schema, table_name";

    // For depth=CATALOGS, build a simpler query that returns only catalog names.
    // For all other depths, query tables (and possibly schemas).
    FlatResult tables_result;
    if (depth == ADBC_OBJECT_DEPTH_CATALOGS)
    {
        std::string cat_sql = "SELECT DISTINCT table_catalog FROM information_schema.tables";
        bool first_cat = true;
        addFilter(cat_sql, "table_catalog", catalog_filter, first_cat);
        cat_sql += " ORDER BY table_catalog";
        std::string err = executeAndRead(conn, cat_sql, tables_result);
        if (!err.empty())
            return SetError(error, ADBC_STATUS_IO, "GetObjects (catalogs): " + err);
    }
    else if (include_tables || depth == ADBC_OBJECT_DEPTH_DB_SCHEMAS)
    {
        std::string err = executeAndRead(conn, tables_sql, tables_result);
        if (!err.empty())
            return SetError(error, ADBC_STATUS_IO, "GetObjects (tables): " + err);
    }

    // ---- Query information_schema.columns (depth=ALL) ----
    FlatResult cols_result;
    if (include_columns)
    {
        std::string cols_sql = "SELECT table_catalog, table_schema, table_name, column_name, "
                               "ordinal_position, data_type, is_nullable, column_default, "
                               "character_maximum_length, character_octet_length, "
                               "numeric_precision, numeric_scale, numeric_precision_radix "
                               "FROM information_schema.columns";
        bool first2 = true;
        addFilter(cols_sql, "table_catalog", catalog_filter, first2);
        addFilter(cols_sql, "table_schema", db_schema_filter, first2);
        addFilter(cols_sql, "table_name", table_name_filter, first2);
        addFilter(cols_sql, "column_name", column_name_filter, first2);
        cols_sql += " ORDER BY table_catalog, table_schema, table_name, ordinal_position";

        std::string err = executeAndRead(conn, cols_sql, cols_result);
        if (!err.empty())
            return SetError(error, ADBC_STATUS_IO, "GetObjects (columns): " + err);
    }

    // ---- Group into CatalogRow hierarchy ----
    std::vector<CatalogRow> catalogs;

    // For depth < ADBC_OBJECT_DEPTH_TABLES we still need catalog/schema from
    // information_schema.tables (queried above with the same filters but no table_name filter).
    // Build the hierarchy from the flat tables result.

    auto find_or_add_catalog = [&](const std::string & cat_name) -> CatalogRow & {
        for (auto & c : catalogs)
            if (c.catalog_name == cat_name)
                return c;
        catalogs.push_back({cat_name, {}});
        return catalogs.back();
    };
    auto find_or_add_schema = [](CatalogRow & cat, const std::string & sch_name) -> SchemaRow & {
        for (auto & s : cat.schemas)
            if (s.schema_name == sch_name)
                return s;
        cat.schemas.push_back({sch_name, {}});
        return cat.schemas.back();
    };

    for (const auto & row : tables_result.rows)
    {
        auto cat = getCol(tables_result, row, "table_catalog").value_or("");
        auto & cat_row = find_or_add_catalog(cat);

        if (depth == ADBC_OBJECT_DEPTH_CATALOGS)
            continue; // only catalog names needed; schemas/tables come from the other query path

        auto sch = getCol(tables_result, row, "table_schema").value_or("");
        auto tbl = getCol(tables_result, row, "table_name").value_or("");
        auto typ = getCol(tables_result, row, "table_type").value_or("");

        auto & sch_row = find_or_add_schema(cat_row, sch);

        if (include_tables)
            sch_row.tables.push_back({tbl, typ, {}});
    }

    // Populate columns into the matching table.
    if (include_columns)
    {
        for (const auto & row : cols_result.rows)
        {
            auto cat = getCol(cols_result, row, "table_catalog").value_or("");
            auto sch = getCol(cols_result, row, "table_schema").value_or("");
            auto tbl = getCol(cols_result, row, "table_name").value_or("");

            for (auto & cat_row : catalogs)
            {
                if (cat_row.catalog_name != cat)
                    continue;
                for (auto & sch_row : cat_row.schemas)
                {
                    if (sch_row.schema_name != sch)
                        continue;
                    for (auto & tbl_row : sch_row.tables)
                    {
                        if (tbl_row.table_name != tbl)
                            continue;
                        ColumnRow col;
                        col.column_name = getCol(cols_result, row, "column_name").value_or("");
                        {
                            auto op = getCol(cols_result, row, "ordinal_position");
                            col.ordinal_position = op.has_value() ? std::stoi(*op) : 0;
                        }
                        col.data_type = getCol(cols_result, row, "data_type").value_or("");
                        col.is_nullable = getCol(cols_result, row, "is_nullable").value_or("");
                        col.column_default = getCol(cols_result, row, "column_default");
                        {
                            auto v = getCol(cols_result, row, "character_maximum_length");
                            if (v)
                                col.char_max_length = std::stoi(*v);
                        }
                        {
                            auto v = getCol(cols_result, row, "character_octet_length");
                            if (v)
                                col.char_octet_length = std::stoi(*v);
                        }
                        {
                            auto v = getCol(cols_result, row, "numeric_precision");
                            if (v)
                                col.num_precision = std::stoi(*v);
                        }
                        {
                            auto v = getCol(cols_result, row, "numeric_scale");
                            if (v)
                                col.num_scale = std::stoi(*v);
                        }
                        {
                            auto v = getCol(cols_result, row, "numeric_precision_radix");
                            if (v)
                                col.num_prec_radix = std::stoi(*v);
                        }
                        tbl_row.columns.push_back(std::move(col));
                        break;
                    }
                    break;
                }
                break;
            }
        }
    }

    // ---- Build output Arrow batch ----
    nanoarrow::UniqueSchema schema;
    if (buildGetObjectsSchema(schema.get()) != 0)
        return SetError(error, ADBC_STATUS_INTERNAL, "GetObjects: failed to build schema");

    nanoarrow::UniqueArray batch;
    if (ArrowArrayInitFromSchema(batch.get(), schema.get(), nullptr) != NANOARROW_OK
        || ArrowArrayStartAppending(batch.get()) != NANOARROW_OK)
        return SetError(error, ADBC_STATUS_INTERNAL, "GetObjects: failed to init array");

    // Shorthand pointers into the nested builder.
    ArrowArray * catalog_name_arr = batch->children[0];
    ArrowArray * schemas_list = batch->children[1];
    ArrowArray * schema_struct = schemas_list->children[0];
    ArrowArray * schema_name_arr = schema_struct->children[0];
    ArrowArray * tables_list = schema_struct->children[1];
    ArrowArray * table_struct = tables_list->children[0];
    ArrowArray * table_name_arr = table_struct->children[0];
    ArrowArray * table_type_arr = table_struct->children[1];
    ArrowArray * cols_list = table_struct->children[2];
    ArrowArray * constraints_list = table_struct->children[3];
    (void)table_name_arr;
    (void)table_type_arr;

    for (const auto & cat : catalogs)
    {
        // catalog_name
        if (!cat.catalog_name.empty())
        {
            if (ArrowArrayAppendString(catalog_name_arr, sv(cat.catalog_name)) != NANOARROW_OK)
                return SetError(error, ADBC_STATUS_INTERNAL, "GetObjects: catalog_name append failed");
        }
        else
        {
            if (ArrowArrayAppendNull(catalog_name_arr, 1) != NANOARROW_OK)
                return SetError(error, ADBC_STATUS_INTERNAL, "GetObjects: catalog_name null append failed");
        }

        if (depth == ADBC_OBJECT_DEPTH_CATALOGS)
        {
            // catalog_db_schemas: null
            if (ArrowArrayAppendNull(schemas_list, 1) != NANOARROW_OK)
                return SetError(error, ADBC_STATUS_INTERNAL, "GetObjects: schemas_list null failed");
        }
        else
        {
            // Build schemas list for this catalog.
            for (const auto & sch : cat.schemas)
            {
                // db_schema_name
                if (ArrowArrayAppendString(schema_name_arr, sv(sch.schema_name)) != NANOARROW_OK)
                    return SetError(error, ADBC_STATUS_INTERNAL, "GetObjects: schema_name append failed");

                if (!include_tables || depth == ADBC_OBJECT_DEPTH_DB_SCHEMAS)
                {
                    // db_schema_tables: null
                    if (ArrowArrayAppendNull(tables_list, 1) != NANOARROW_OK)
                        return SetError(error, ADBC_STATUS_INTERNAL, "GetObjects: tables_list null failed");
                }
                else
                {
                    // Build tables list for this schema.
                    for (const auto & tbl : sch.tables)
                    {
                        // table_name, table_type
                        if (ArrowArrayAppendString(table_struct->children[0], sv(tbl.table_name)) != NANOARROW_OK
                            || ArrowArrayAppendString(table_struct->children[1], sv(tbl.table_type)) != NANOARROW_OK)
                            return SetError(error, ADBC_STATUS_INTERNAL, "GetObjects: table fields append failed");

                        // table_columns list
                        ArrowArray * col_struct_arr = cols_list->children[0];
                        if (include_columns)
                        {
                            for (const auto & col : tbl.columns)
                                if (appendColumn(col_struct_arr, col) != 0)
                                    return SetError(error, ADBC_STATUS_INTERNAL, "GetObjects: column append failed");
                        }
                        if (ArrowArrayFinishElement(cols_list) != NANOARROW_OK)
                            return SetError(error, ADBC_STATUS_INTERNAL, "GetObjects: cols_list finish failed");

                        // table_constraints: null list
                        if (ArrowArrayAppendNull(constraints_list, 1) != NANOARROW_OK)
                            return SetError(error, ADBC_STATUS_INTERNAL, "GetObjects: constraints null failed");

                        if (ArrowArrayFinishElement(table_struct) != NANOARROW_OK)
                            return SetError(error, ADBC_STATUS_INTERNAL, "GetObjects: table_struct finish failed");
                    }
                    if (ArrowArrayFinishElement(tables_list) != NANOARROW_OK)
                        return SetError(error, ADBC_STATUS_INTERNAL, "GetObjects: tables_list finish failed");
                }

                if (ArrowArrayFinishElement(schema_struct) != NANOARROW_OK)
                    return SetError(error, ADBC_STATUS_INTERNAL, "GetObjects: schema_struct finish failed");
            }
            if (ArrowArrayFinishElement(schemas_list) != NANOARROW_OK)
                return SetError(error, ADBC_STATUS_INTERNAL, "GetObjects: schemas_list finish failed");
        }

        if (ArrowArrayFinishElement(batch.get()) != NANOARROW_OK)
            return SetError(error, ADBC_STATUS_INTERNAL, "GetObjects: batch finish failed");
    }

    ArrowError na_err{};
    if (ArrowArrayFinishBuilding(batch.get(), NANOARROW_VALIDATION_LEVEL_DEFAULT, &na_err) != NANOARROW_OK)
        return SetError(error, ADBC_STATUS_INTERNAL, std::string("GetObjects: ArrowArrayFinishBuilding: ") + na_err.message);

    ExportBatchAsStream(schema.get(), batch.get(), out);
    return ADBC_STATUS_OK;
}

} // namespace firebolt::adbc
