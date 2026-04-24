#include "ArrowIpcStream.h"
#include "FireboltAdbcConnection.h"
#include "FireboltAdbcDatabase.h"
#include "FireboltAdbcMetadata.h"
#include "FireboltAdbcStatement.h"
#include "HttpClient.h"
#include "adbc.h"

#include <nanoarrow/nanoarrow.hpp>
#include <nanoarrow/nanoarrow_ipc.hpp>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>

#include <curl/curl.h>

namespace firebolt::adbc
{

// ============================================================
// Utility helpers
// ============================================================

// Quote a SQL identifier with double quotes, escaping any embedded double quotes
// by doubling them (standard SQL identifier quoting).  This prevents SQL injection
// when table names or column names originate from user-controlled input.
static std::string quoteIdentifier(const std::string & name)
{
    std::string result;
    result.reserve(name.size() + 2);
    result += '"';
    for (char c : name)
    {
        if (c == '"')
            result += '"'; // double any embedded double-quote
        result += c;
    }
    result += '"';
    return result;
}

static AdbcStatusCode SetError(AdbcError * e, AdbcStatusCode code, const std::string & msg)
{
    if (e)
    {
        e->message = strdup(msg.c_str());
        e->release = [](AdbcError * err) {
            free(err->message);
            err->message = nullptr;
            err->release = nullptr;
        };
        e->vendor_code = 0;
    }
    return code;
}

static AdbcStatusCode HttpRespToStatus(const HttpResponse & resp, AdbcError * error)
{
    if (resp.curl_code != CURLE_OK)
        return SetError(error, ADBC_STATUS_IO, "curl error: " + std::string(curl_easy_strerror(resp.curl_code)));
    if (resp.http_code == 401 || resp.http_code == 403)
        return SetError(error, ADBC_STATUS_UNAUTHORIZED, "HTTP " + std::to_string(resp.http_code) + ": " + resp.error_message);
    if (resp.http_code >= 400 && resp.http_code < 500)
        return SetError(error, ADBC_STATUS_INVALID_ARGUMENT, "HTTP " + std::to_string(resp.http_code) + ": " + resp.error_message);
    if (resp.http_code >= 500)
        return SetError(error, ADBC_STATUS_IO, "HTTP " + std::to_string(resp.http_code) + ": " + resp.error_message);
    return ADBC_STATUS_OK;
}

static void ApplySessionUpdates(FireboltConnection * conn, const HttpResponse & resp)
{
    if (resp.reset_session)
        conn->session_params.clear();
    for (const auto & [k, v] : resp.update_params)
        conn->session_params[k] = v;
    for (const auto & k : resp.remove_params)
        conn->session_params.erase(k);
}

// ============================================================
// curl global init / cleanup (once per process)
// ============================================================

static std::once_flag g_curl_init_flag;

static void initCurl()
{
    std::call_once(g_curl_init_flag, []() { curl_global_init(CURL_GLOBAL_ALL); });
}

__attribute__((destructor)) static void cleanupCurl()
{
    curl_global_cleanup();
}

// Execute a simple SQL statement (no result rows expected).
// Used for BEGIN / COMMIT / ROLLBACK.
static AdbcStatusCode RunSimpleSql(FireboltConnection * conn, const char * sql, AdbcError * error)
{
    auto resp = conn->http->executeQuery(sql, conn->session_params);
    ApplySessionUpdates(conn, resp);
    if (!resp.isSuccess())
        return HttpRespToStatus(resp, error);
    return ADBC_STATUS_OK;
}

// ============================================================
// Arrow IPC serialisation helper for StatementBind / BindStream
// Serialises an ArrowArrayStream into Arrow IPC stream bytes using nanoarrow.
// ============================================================

static std::string SerializeArrayStream(ArrowArrayStream * stream, std::vector<uint8_t> & out)
{
    // ArrowIpcOutputStreamInitBuffer stores a pointer to buf (does NOT take ownership).
    // ArrowIpcWriterInit takes ownership of the output stream via ArrowIpcOutputStreamMove.
    // After ArrowIpcWriterReset, buf is still valid and holds the written IPC bytes.
    ArrowBuffer buf{};
    ArrowBufferInit(&buf);

    ArrowIpcOutputStream out_stream{};
    if (ArrowIpcOutputStreamInitBuffer(&out_stream, &buf) != 0)
    {
        ArrowBufferReset(&buf);
        return "ArrowIpcOutputStreamInitBuffer failed";
    }

    ArrowIpcWriter writer{};
    if (ArrowIpcWriterInit(&writer, &out_stream) != 0)
    {
        if (out_stream.release)
            out_stream.release(&out_stream);
        ArrowBufferReset(&buf);
        return "ArrowIpcWriterInit failed";
    }
    // writer owns out_stream now; out_stream.release should not be called separately

    ArrowError error{};
    int rc = ArrowIpcWriterWriteArrayStream(&writer, stream, &error);
    ArrowIpcWriterReset(&writer); // releases writer + out_stream (not buf)

    if (rc != 0)
    {
        ArrowBufferReset(&buf);
        return std::string("ArrowIpcWriterWriteArrayStream: ") + error.message;
    }

    out.assign(buf.data, buf.data + buf.size_bytes);
    ArrowBufferReset(&buf);
    return {};
}

// ============================================================
// Database
// ============================================================

static AdbcStatusCode DatabaseNew(AdbcDatabase * db, AdbcError * error)
{
    if (!db)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "AdbcDatabase is null");
    try
    {
        db->private_data = new FireboltDatabase();
        return ADBC_STATUS_OK;
    }
    catch (std::exception & ex)
    {
        return SetError(error, ADBC_STATUS_INTERNAL, ex.what());
    }
}

static AdbcStatusCode DatabaseSetOption(AdbcDatabase * db, const char * key, const char * value, AdbcError * error)
{
    if (!db || !db->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Database not initialized");
    auto * fdb = static_cast<FireboltDatabase *>(db->private_data);
    if (!key)
        return SetError(error, ADBC_STATUS_INVALID_ARGUMENT, "Option key is null");
    const std::string k(key);
    const std::string v(value ? value : "");
    if (k == "uri")
        fdb->url = v;
    else if (k == "adbc.firebolt.token")
        fdb->token = v;
    else if (k == "adbc.firebolt.database")
        fdb->database = v;
    else if (k == "adbc.firebolt.timeout_sec")
        fdb->timeout_sec = std::stol(v);
    // Unknown options are silently ignored.
    return ADBC_STATUS_OK;
}

static AdbcStatusCode DatabaseInit(AdbcDatabase * db, AdbcError * error)
{
    if (!db || !db->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Database not initialized");
    auto * fdb = static_cast<FireboltDatabase *>(db->private_data);
    if (fdb->url.empty())
        return SetError(error, ADBC_STATUS_INVALID_ARGUMENT, "Database 'uri' option is required");
    initCurl();
    fdb->initialized = true;
    return ADBC_STATUS_OK;
}

static AdbcStatusCode DatabaseRelease(AdbcDatabase * db, AdbcError * /*error*/)
{
    if (!db || !db->private_data)
        return ADBC_STATUS_OK;
    delete static_cast<FireboltDatabase *>(db->private_data);
    db->private_data = nullptr;
    return ADBC_STATUS_OK;
}

// ============================================================
// Connection
// ============================================================

static AdbcStatusCode ConnectionNew(AdbcConnection * conn, AdbcError * error)
{
    if (!conn)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "AdbcConnection is null");
    try
    {
        conn->private_data = new FireboltConnection();
        return ADBC_STATUS_OK;
    }
    catch (std::exception & ex)
    {
        return SetError(error, ADBC_STATUS_INTERNAL, ex.what());
    }
}

static AdbcStatusCode ConnectionSetOption(AdbcConnection * conn, const char * key, const char * value, AdbcError * error)
{
    if (!conn || !conn->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Connection not initialized");
    auto * fc = static_cast<FireboltConnection *>(conn->private_data);
    if (!key)
        return SetError(error, ADBC_STATUS_INVALID_ARGUMENT, "Option key is null");
    const std::string k(key);
    const std::string v(value ? value : "");

    if (k == ADBC_CONNECTION_OPTION_AUTOCOMMIT)
    {
        bool want_autocommit = (v != ADBC_OPTION_VALUE_DISABLED);
        if (want_autocommit && !fc->autocommit && fc->in_transaction && fc->http)
        {
            // Switching back to autocommit while inside a transaction: commit it.
            AdbcStatusCode rc = RunSimpleSql(fc, "COMMIT", error);
            if (rc != ADBC_STATUS_OK)
                return rc;
            fc->in_transaction = false;
        }
        fc->autocommit = want_autocommit;
        return ADBC_STATUS_OK;
    }
    if (k == "adbc.firebolt.token" && fc->db)
        fc->db->token = v;

    // Everything else stored as a session parameter appended to query URL
    fc->session_params[k] = v;
    return ADBC_STATUS_OK;
}

static AdbcStatusCode ConnectionInit(AdbcConnection * conn, AdbcDatabase * db, AdbcError * error)
{
    if (!conn || !conn->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Connection not initialized");
    if (!db || !db->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Database not initialized");
    auto * fc = static_cast<FireboltConnection *>(conn->private_data);
    auto * fdb = static_cast<FireboltDatabase *>(db->private_data);
    if (!fdb->initialized)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Database not initialized");
    try
    {
        fc->db = fdb;
        fc->http = std::make_unique<HttpClient>(*fdb);
        return ADBC_STATUS_OK;
    }
    catch (std::exception & ex)
    {
        return SetError(error, ADBC_STATUS_INTERNAL, ex.what());
    }
}

static AdbcStatusCode ConnectionRelease(AdbcConnection * conn, AdbcError * /*error*/)
{
    if (!conn || !conn->private_data)
        return ADBC_STATUS_OK;
    delete static_cast<FireboltConnection *>(conn->private_data);
    conn->private_data = nullptr;
    return ADBC_STATUS_OK;
}

static AdbcStatusCode
ConnectionGetInfo(AdbcConnection * conn, const uint32_t * info_codes, size_t info_codes_len, ArrowArrayStream * out, AdbcError * error)
{
    if (!conn || !conn->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Connection not initialized");
    return firebolt::adbc::ConnectionGetInfo(static_cast<FireboltConnection *>(conn->private_data), info_codes, info_codes_len, out, error);
}

static AdbcStatusCode ConnectionGetObjects(
    AdbcConnection * conn,
    int depth,
    const char * catalog,
    const char * db_schema,
    const char * table_name,
    const char ** table_type,
    const char * column_name,
    ArrowArrayStream * out,
    AdbcError * error)
{
    if (!conn || !conn->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Connection not initialized");
    return firebolt::adbc::ConnectionGetObjects(
        static_cast<FireboltConnection *>(conn->private_data), depth, catalog, db_schema, table_name, table_type, column_name, out, error);
}

static AdbcStatusCode ConnectionGetTableSchema(
    AdbcConnection * conn, const char * catalog, const char * db_schema, const char * table_name, ArrowSchema * out, AdbcError * error)
{
    if (!conn || !conn->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Connection not initialized");
    return firebolt::adbc::ConnectionGetTableSchema(
        static_cast<FireboltConnection *>(conn->private_data), catalog, db_schema, table_name, out, error);
}

static AdbcStatusCode ConnectionGetTableTypes(AdbcConnection * conn, ArrowArrayStream * out, AdbcError * error)
{
    if (!conn || !conn->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Connection not initialized");
    return firebolt::adbc::ConnectionGetTableTypes(static_cast<FireboltConnection *>(conn->private_data), out, error);
}

static AdbcStatusCode ConnectionReadPartition(
    AdbcConnection * /*conn*/,
    const uint8_t * /*serialized_partition*/,
    size_t /*serialized_length*/,
    ArrowArrayStream * /*out*/,
    AdbcError * error)
{
    return SetError(error, ADBC_STATUS_NOT_IMPLEMENTED, "ConnectionReadPartition not implemented");
}

static AdbcStatusCode ConnectionCommit(AdbcConnection * conn, AdbcError * error)
{
    if (!conn || !conn->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Connection not initialized");
    auto * fc = static_cast<FireboltConnection *>(conn->private_data);
    if (fc->autocommit)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Cannot commit: connection is in autocommit mode");
    if (!fc->in_transaction)
        return ADBC_STATUS_OK; // nothing to commit
    AdbcStatusCode rc = RunSimpleSql(fc, "COMMIT", error);
    if (rc == ADBC_STATUS_OK)
        fc->in_transaction = false;
    return rc;
}

static AdbcStatusCode ConnectionRollback(AdbcConnection * conn, AdbcError * error)
{
    if (!conn || !conn->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Connection not initialized");
    auto * fc = static_cast<FireboltConnection *>(conn->private_data);
    if (fc->autocommit)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Cannot rollback: connection is in autocommit mode");
    if (!fc->in_transaction)
        return ADBC_STATUS_OK; // nothing to roll back
    AdbcStatusCode rc = RunSimpleSql(fc, "ROLLBACK", error);
    if (rc == ADBC_STATUS_OK)
        fc->in_transaction = false;
    return rc;
}

// ============================================================
// Statement
// ============================================================

static AdbcStatusCode StatementNew(AdbcConnection * conn, AdbcStatement * stmt, AdbcError * error)
{
    if (!conn || !conn->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Connection not initialized");
    if (!stmt)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "AdbcStatement is null");
    try
    {
        auto * fs = new FireboltStatement();
        fs->conn = static_cast<FireboltConnection *>(conn->private_data);
        stmt->private_data = fs;
        return ADBC_STATUS_OK;
    }
    catch (std::exception & ex)
    {
        return SetError(error, ADBC_STATUS_INTERNAL, ex.what());
    }
}

static AdbcStatusCode StatementRelease(AdbcStatement * stmt, AdbcError * /*error*/)
{
    if (!stmt || !stmt->private_data)
        return ADBC_STATUS_OK;
    delete static_cast<FireboltStatement *>(stmt->private_data);
    stmt->private_data = nullptr;
    return ADBC_STATUS_OK;
}

static AdbcStatusCode StatementSetSqlQuery(AdbcStatement * stmt, const char * query, AdbcError * error)
{
    if (!stmt || !stmt->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Statement not initialized");
    if (!query)
        return SetError(error, ADBC_STATUS_INVALID_ARGUMENT, "Query is null");
    static_cast<FireboltStatement *>(stmt->private_data)->sql = query;
    return ADBC_STATUS_OK;
}

static AdbcStatusCode StatementSetSubstraitPlan(AdbcStatement * /*stmt*/, const uint8_t * /*plan*/, size_t /*length*/, AdbcError * error)
{
    return SetError(error, ADBC_STATUS_NOT_IMPLEMENTED, "Substrait plans not supported");
}

static AdbcStatusCode StatementPrepare(AdbcStatement * stmt, AdbcError * error)
{
    if (!stmt || !stmt->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Statement not initialized");
    // No-op: Firebolt HTTP has no server-side prepare
    return ADBC_STATUS_OK;
}

static AdbcStatusCode StatementBind(AdbcStatement * stmt, ArrowArray * values, ArrowSchema * schema, AdbcError * error)
{
    if (!stmt || !stmt->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Statement not initialized");
    auto * fs = static_cast<FireboltStatement *>(stmt->private_data);

    // Capture column names before the stream takes ownership of the schema.
    fs->bound_column_names.clear();
    if (schema)
        for (int64_t i = 0; i < schema->n_children; ++i)
            if (schema->children[i] && schema->children[i]->name)
                fs->bound_column_names.emplace_back(schema->children[i]->name);

    // Wrap the single record batch in a one-shot ArrowArrayStream, then serialise.
    // ArrowBasicArrayStreamInit(stream, schema, n_arrays) takes ownership of schema.
    // ArrowBasicArrayStreamSetArray(stream, i, array) takes ownership of array (returns void).
    nanoarrow::UniqueArrayStream stream;
    int rc = ArrowBasicArrayStreamInit(stream.get(), schema, 1);
    if (rc != 0)
        return SetError(error, ADBC_STATUS_INVALID_ARGUMENT, "ArrowBasicArrayStreamInit failed");

    ArrowBasicArrayStreamSetArray(stream.get(), 0, values);

    std::string err = SerializeArrayStream(stream.get(), fs->bound_ipc_bytes);
    if (!err.empty())
        return SetError(error, ADBC_STATUS_INTERNAL, err);

    fs->has_bind_data = true;
    return ADBC_STATUS_OK;
}

static AdbcStatusCode StatementBindStream(AdbcStatement * stmt, ArrowArrayStream * stream, AdbcError * error)
{
    if (!stmt || !stmt->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Statement not initialized");
    auto * fs = static_cast<FireboltStatement *>(stmt->private_data);

    // Capture column names from the stream schema before consuming the stream.
    fs->bound_column_names.clear();
    if (stream && stream->get_schema)
    {
        ArrowSchema schema{};
        if (stream->get_schema(stream, &schema) == 0)
        {
            for (int64_t i = 0; i < schema.n_children; ++i)
                if (schema.children[i] && schema.children[i]->name)
                    fs->bound_column_names.emplace_back(schema.children[i]->name);
            if (schema.release)
                schema.release(&schema);
        }
    }

    std::string err = SerializeArrayStream(stream, fs->bound_ipc_bytes);
    if (!err.empty())
        return SetError(error, ADBC_STATUS_INTERNAL, err);

    fs->has_bind_data = true;
    return ADBC_STATUS_OK;
}

static AdbcStatusCode StatementGetParameterSchema(AdbcStatement * /*stmt*/, ArrowSchema * /*schema*/, AdbcError * error)
{
    return SetError(error, ADBC_STATUS_NOT_IMPLEMENTED, "StatementGetParameterSchema not implemented");
}

static AdbcStatusCode StatementSetOption(AdbcStatement * stmt, const char * key, const char * value, AdbcError * error)
{
    if (!stmt || !stmt->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Statement not initialized");
    auto * fs = static_cast<FireboltStatement *>(stmt->private_data);
    if (key && std::string_view{key} == ADBC_INGEST_OPTION_TARGET_TABLE)
        fs->ingest_target_table = value ? value : "";
    return ADBC_STATUS_OK;
}

static AdbcStatusCode StatementExecuteQuery(AdbcStatement * stmt, ArrowArrayStream * out, int64_t * rows_affected, AdbcError * error)
{
    if (!stmt || !stmt->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Statement not initialized");
    auto * fs = static_cast<FireboltStatement *>(stmt->private_data);
    auto * conn = fs->conn;
    if (!conn || !conn->http)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Connection not initialized");

    if (rows_affected)
        *rows_affected = -1;

    try
    {
        // Lazy BEGIN: start a transaction on the first statement when autocommit is off.
        if (!conn->autocommit && !conn->in_transaction)
        {
            AdbcStatusCode rc = RunSimpleSql(conn, "BEGIN", error);
            if (rc != ADBC_STATUS_OK)
                return rc;
            conn->in_transaction = true;
        }

        // For bulk ingestion: auto-generate INSERT SQL from the target table and bound column names.
        // Always regenerate when ingest_target_table is set, to handle statement reuse correctly
        // (the DBAPI cursor reuses the same AdbcStatement across multiple adbc_ingest() calls).
        if (fs->has_bind_data && !fs->ingest_target_table.empty())
        {
            std::string cols;
            for (size_t i = 0; i < fs->bound_column_names.size(); ++i)
            {
                if (i > 0)
                    cols += ", ";
                cols += quoteIdentifier(fs->bound_column_names[i]);
            }
            fs->sql = "INSERT INTO " + quoteIdentifier(fs->ingest_target_table);
            if (!cols.empty())
                fs->sql += " (" + cols + ")";
            fs->sql += " SELECT * FROM read_arrow('upload://data.arrow')";
        }

        HttpResponse resp;
        if (fs->has_bind_data)
            resp = conn->http->executeInsert(fs->sql, fs->bound_ipc_bytes, conn->session_params);
        else
            resp = conn->http->executeQuery(fs->sql, conn->session_params);

        ApplySessionUpdates(conn, resp);

        if (!resp.isSuccess())
            return HttpRespToStatus(resp, error);

        if (out)
        {
            std::string err = ExportIpcBytesAsArrowStream(std::move(resp.body), out);
            if (!err.empty())
                return SetError(error, ADBC_STATUS_INTERNAL, err);
        }

        return ADBC_STATUS_OK;
    }
    catch (std::exception & ex)
    {
        return SetError(error, ADBC_STATUS_INTERNAL, ex.what());
    }
}

static AdbcStatusCode StatementExecutePartitions(
    AdbcStatement * /*stmt*/, ArrowSchema * /*schema*/, AdbcPartitions * /*partitions*/, int64_t * /*rows_affected*/, AdbcError * error)
{
    return SetError(error, ADBC_STATUS_NOT_IMPLEMENTED, "StatementExecutePartitions not implemented");
}

// ============================================================
// Driver init
// ============================================================

static AdbcStatusCode DriverRelease(AdbcDriver * /*driver*/, AdbcError * /*error*/)
{
    return ADBC_STATUS_OK;
}

static AdbcStatusCode PopulateDriver(AdbcDriver * driver, AdbcError * error)
{
    if (!driver)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "AdbcDriver is null");

    driver->release = DriverRelease;

    driver->DatabaseNew = DatabaseNew;
    driver->DatabaseSetOption = DatabaseSetOption;
    driver->DatabaseInit = DatabaseInit;
    driver->DatabaseRelease = DatabaseRelease;

    driver->ConnectionNew = ConnectionNew;
    driver->ConnectionSetOption = ConnectionSetOption;
    driver->ConnectionInit = ConnectionInit;
    driver->ConnectionRelease = ConnectionRelease;
    driver->ConnectionGetInfo = ConnectionGetInfo;
    driver->ConnectionGetObjects = ConnectionGetObjects;
    driver->ConnectionGetTableSchema = ConnectionGetTableSchema;
    driver->ConnectionGetTableTypes = ConnectionGetTableTypes;
    driver->ConnectionReadPartition = ConnectionReadPartition;
    driver->ConnectionCommit = ConnectionCommit;
    driver->ConnectionRollback = ConnectionRollback;

    driver->StatementNew = StatementNew;
    driver->StatementRelease = StatementRelease;
    driver->StatementExecuteQuery = StatementExecuteQuery;
    driver->StatementPrepare = StatementPrepare;
    driver->StatementSetSqlQuery = StatementSetSqlQuery;
    driver->StatementSetSubstraitPlan = StatementSetSubstraitPlan;
    driver->StatementBind = StatementBind;
    driver->StatementBindStream = StatementBindStream;
    driver->StatementGetParameterSchema = StatementGetParameterSchema;
    driver->StatementSetOption = StatementSetOption;
    driver->StatementExecutePartitions = StatementExecutePartitions;

    return ADBC_STATUS_OK;
}

} // namespace firebolt::adbc

// ============================================================
// Public entry points (C linkage, exported by version script)
// ============================================================

extern "C" __attribute__((visibility("default"))) AdbcStatusCode AdbcDriverInit(int version, void * raw_driver, AdbcError * error)
{
    if (version != ADBC_VERSION_1_0_0 && version != ADBC_VERSION_1_1_0)
    {
        if (error)
        {
            error->vendor_code = 0;
            error->message = strdup("Unsupported ADBC version");
            error->release = [](AdbcError * e) {
                free(e->message);
                e->message = nullptr;
                e->release = nullptr;
            };
        }
        return ADBC_STATUS_NOT_IMPLEMENTED;
    }
    return firebolt::adbc::PopulateDriver(static_cast<AdbcDriver *>(raw_driver), error);
}

extern "C" __attribute__((visibility("default"))) AdbcStatusCode FireboltAdbcDriverInit(int version, void * raw_driver, AdbcError * error)
{
    return AdbcDriverInit(version, raw_driver, error);
}
