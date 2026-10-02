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

#include "ArrowIpcStream.h"
#include "DescribeParameters.h"
#include "FireboltAdbcConnection.h"
#include "FireboltAdbcDatabase.h"
#include "FireboltAdbcMetadata.h"
#include "FireboltAdbcStatement.h"
#include "HttpClient.h"
#include "IngestSqlBuilder.h"
#include "QueryParameters.h"
#include "ScopeGuard.h"
#include "TlsConfig.h"
#include "adbc.h"

#include <nanoarrow/nanoarrow.hpp>
#include <nanoarrow/nanoarrow_ipc.hpp>

#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <strings.h>

#include <curl/curl.h>

namespace firebolt::adbc
{

// ============================================================
// Utility helpers
// ============================================================

// SQL identifier quoting, table qualification, and Arrow→Firebolt type mapping
// helpers live in IngestSqlBuilder.{h,cpp}.

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

// Case-insensitive check that `url` begins with `scheme` (URI schemes are
// case-insensitive per RFC 3986 §3.1).
static bool hasSchemePrefix(const std::string & url, const char * scheme)
{
    const size_t n = strlen(scheme);
    return url.size() >= n && strncasecmp(url.c_str(), scheme, n) == 0;
}

// Whether the libcurl we are linked against can speak TLS.  Asked of libcurl
// rather than tracked as a build-time define so the answer always matches the
// library actually loaded.
static bool curlSupportsTls()
{
    const curl_version_info_data * v = curl_version_info(CURLVERSION_NOW);
    return v != nullptr && (v->features & CURL_VERSION_SSL) != 0;
}

// Apply server-advertised session updates only when the response succeeded.
// See applySessionUpdatesIfSuccess in HttpClient.h for the rationale — a 5xx
// or 4xx response can be from a MITM or a generic-error proxy, so its
// session-state hints are untrusted.
static void ApplySessionUpdates(FireboltConnection * conn, const HttpResponse & resp)
{
    applySessionUpdatesIfSuccess(conn->session_params, resp);
}

// ADBC 1.1.0 predates this option, so the vendored adbc.h does not declare it, but
// driver managers send it (adbc_driver_manager 1.8.0,
// StatementOptions.BIND_BY_NAME).
#define FIREBOLT_ADBC_STATEMENT_OPTION_BIND_BY_NAME "adbc.statement.bind_by_name"

// Firebolt query settings the driver sets per request.  They travel as URL query
// parameters, the same channel session parameters use.
static constexpr const char * QUERY_PARAMETERS_SETTING = "query_parameters";
static constexpr const char * EXECUTION_MODE_SETTING = "execution_mode";
static constexpr const char * DESCRIBE_PARAMETERS_MODE = "describe_parameters";

// Session parameters the engine uses to keep a statement inside an open
// transaction.  Set by the server through Firebolt-Update-Parameters after BEGIN.
static constexpr const char * TRANSACTION_ID_PARAM = "transaction_id";
static constexpr const char * TRANSACTION_SEQUENCE_PARAM = "transaction_sequence_id";

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

// Encode the bound batches as the Arrow IPC stream the multipart ingest uploads as
// data.arrow.  The ingest path is the only caller; query parameters are read
// straight out of the arrays.
//
// Consumes the batches — ArrowBasicArrayStreamSetArray takes ownership of each, and
// the payload is single-use.  The stream takes ownership of its schema too, hence
// the copy: buildIngestSql still needs the statement's own.
static AdbcStatusCode SerializeBoundData(BoundData & bound, std::vector<uint8_t> & out, AdbcError * error)
{
    // Reachable in append mode, the one mode buildIngestSql needs no schema for.
    if (!bound.schema->release)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Bound data has no schema; nothing can be uploaded");

    nanoarrow::UniqueSchema schema_copy;
    if (ArrowSchemaDeepCopy(bound.schema.get(), schema_copy.get()) != 0)
        return SetError(error, ADBC_STATUS_INTERNAL, "ArrowSchemaDeepCopy failed");

    nanoarrow::UniqueArrayStream stream;
    if (ArrowBasicArrayStreamInit(stream.get(), schema_copy.get(), static_cast<int64_t>(bound.batches.size())) != 0)
        return SetError(error, ADBC_STATUS_INVALID_ARGUMENT, "ArrowBasicArrayStreamInit failed");

    for (size_t i = 0; i < bound.batches.size(); ++i)
        ArrowBasicArrayStreamSetArray(stream.get(), static_cast<int64_t>(i), bound.batches[i].get());
    bound.batches.clear();

    std::string err = SerializeArrayStream(stream.get(), out);
    if (!err.empty())
        return SetError(error, ADBC_STATUS_INTERNAL, err);
    return ADBC_STATUS_OK;
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

// Refuse an option, but not necessarily right now.
//
// A driver manager collects options set before the driver is even loaded and
// replays them from inside AdbcDatabaseInit.  The failure path of that replay in
// adbc-driver-manager (through at least 1.8.0) copies the driver's error with
//
//     error->message = new char[strlen(src)];  error->message[strlen(src)] = '\0';
//
// — a one-byte heap overflow that aborts the host process.  No driver had
// exercised it, because drivers conventionally accept and discard unknown
// options.  Reporting from DatabaseInit instead keeps the diagnostic and avoids
// that path: the manager forwards Init's error struct through untouched.
//
// Once the database is initialised there is no replay involved, so an option set
// after that point is refused immediately.
static AdbcStatusCode RejectOption(FireboltDatabase * fdb, AdbcError * error, AdbcStatusCode code, std::string message)
{
    if (fdb->initialized)
        return SetError(error, code, message);
    if (fdb->option_error.empty())
    {
        fdb->option_error = std::move(message);
        fdb->option_error_code = code;
    }
    return ADBC_STATUS_OK;
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
    // DatabaseInit validates the endpoint and picks the CA bundle from these two.
    // A change after it would bypass both — an http:// database switched to
    // https:// would have no CA bundle at all — so refuse rather than half-apply.
    if (fdb->initialized && (k == "uri" || k == "firebolt.ssl_certificate_path"))
        return SetError(
            error,
            ADBC_STATUS_INVALID_STATE,
            "Option '" + k + "' must be set before AdbcDatabaseInit; open a new database to use another value");
    if (k == "uri")
        fdb->url = v;
    else if (k == "firebolt.token")
        fdb->token = v;
    else if (k == "firebolt.database")
        fdb->database = v;
    else if (k == "firebolt.ssl_certificate_path")
        fdb->ssl_certificate_path = v;
    else if (k == "firebolt.timeout_sec")
    {
        // std::stol throws on non-numeric and out-of-range input.  Letting that
        // propagate would unwind through the C ABI into a driver manager with no
        // handler, aborting the host process (a Python interpreter, an R session)
        // over a mistyped option.
        long parsed = 0;
        try
        {
            size_t consumed = 0;
            parsed = std::stol(v, &consumed);
            if (consumed != v.size())
                throw std::invalid_argument("trailing characters");
        }
        catch (const std::exception &)
        {
            return RejectOption(
                fdb,
                error,
                ADBC_STATUS_INVALID_ARGUMENT,
                "Option 'firebolt.timeout_sec' must be a whole number of seconds (0 disables the timeout); got '" + v + "'");
        }
        if (parsed < 0)
            return RejectOption(
                fdb,
                error,
                ADBC_STATUS_INVALID_ARGUMENT,
                "Option 'firebolt.timeout_sec' must not be negative (0 disables the timeout); got '" + v + "'");
        fdb->timeout_sec = parsed;
    }
    else if (k.rfind("firebolt.", 0) == 0)
    {
        // A misspelled driver option is a configuration bug.  Accepting it
        // silently produced connections that used the server's defaults with no
        // diagnostic anywhere.
        return RejectOption(fdb, error, ADBC_STATUS_NOT_FOUND, "Unknown Firebolt database option '" + k + "'");
    }
    // Keys outside the firebolt.* namespace are accepted and ignored: the
    // driver manager sets some of them itself, and callers pass connection
    // parameters this driver does not consume yet.
    return ADBC_STATUS_OK;
}

static AdbcStatusCode DatabaseInit(AdbcDatabase * db, AdbcError * error)
{
    if (!db || !db->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Database not initialized");
    auto * fdb = static_cast<FireboltDatabase *>(db->private_data);

    // An option rejected before Init is reported here — see RejectOption.
    if (!fdb->option_error.empty())
        return SetError(error, fdb->option_error_code, fdb->option_error);

    if (fdb->url.empty())
        return SetError(error, ADBC_STATUS_INVALID_ARGUMENT, "Database 'uri' option is required");

    // Validate the endpoint here rather than letting libcurl reject it on the
    // first query, where the failure reads as a network error ("Unsupported
    // protocol") with nothing pointing back at the option that caused it.
    const bool is_http = hasSchemePrefix(fdb->url, "http://");
    const bool is_https = hasSchemePrefix(fdb->url, "https://");
    if (!is_http && !is_https)
        return SetError(
            error,
            ADBC_STATUS_INVALID_ARGUMENT,
            "Database 'uri' must start with http:// or https://; got '" + fdb->url
                + "'. Pass the engine's HTTP endpoint, for example http://localhost:3473");
    if (is_https && !curlSupportsTls())
        return SetError(
            error,
            ADBC_STATUS_INVALID_ARGUMENT,
            "Database 'uri' is https:// but this driver was built without TLS support, so it can only reach plaintext http:// "
            "endpoints. Use an http:// endpoint, or rebuild the driver with -DWITH_SSL=ON.");

    // Pick the CA bundle now: the build disables curl's baked-in path, which
    // names the builder image's layout rather than this host's.  A configured
    // bundle is checked even for http://, so a typo is caught either way.
    if (is_https || !fdb->ssl_certificate_path.empty())
    {
        CaBundleResult ca = resolveCaBundle(fdb->ssl_certificate_path);
        if (!ca.error.empty())
            return SetError(error, ca.configuration_error ? ADBC_STATUS_INVALID_ARGUMENT : ADBC_STATUS_INVALID_STATE, ca.error);
        fdb->ca_bundle_path = ca.path;
    }

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
        // ADBC canonicalises exactly two values for a boolean option.  Treating
        // "anything that is not false" as true made "0", "FALSE" and any typo
        // silently mean autocommit ON — and this is the one option where
        // misreading the value changes durability rather than just behaviour:
        // the caller believes they are inside a transaction while every
        // statement is being committed as it executes.  Refuse instead.
        if (v != ADBC_OPTION_VALUE_ENABLED && v != ADBC_OPTION_VALUE_DISABLED)
            return SetError(
                error,
                ADBC_STATUS_INVALID_ARGUMENT,
                "Option '" + k + "' must be exactly \"" + ADBC_OPTION_VALUE_ENABLED + "\" or \"" + ADBC_OPTION_VALUE_DISABLED + "\"; got '"
                    + v + "'");

        const bool want_autocommit = (v == ADBC_OPTION_VALUE_ENABLED);
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
    if (k == "firebolt.token")
    {
        // Update the bearer token used in the Authorization header.  Stored
        // per-connection (NOT on the shared FireboltDatabase) so two
        // connections sharing the same AdbcDatabase keep independent
        // identities.  HttpClient holds a const reference to this
        // FireboltConnection and reads conn_.token live on each request, so
        // no sync call is needed.  Never written into session_params: those
        // are URL-encoded into the query string on every request and would
        // leak the JWT into proxy and server access logs.
        fc->token = v;
        return ADBC_STATUS_OK;
    }

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
        // Initialise the per-connection token from the database default,
        // unless the caller already set a per-connection token via
        // ConnectionSetOption("firebolt.token") between New and Init —
        // ADBC permits options to be applied before Init, and silently
        // dropping that token would fall back to the database identity.
        // Subsequent ConnectionSetOption("firebolt.token") on this
        // connection only mutates fc->token, never fdb->token.
        if (fc->token.empty())
            fc->token = fdb->token;
        // HttpClient borrows a reference to this FireboltConnection and
        // reads token / url / database / timeout live on every request.
        // No need to seed or replay anything — the connection is the
        // single source of truth.
        fc->http = std::make_unique<HttpClient>(*fc);
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
    auto * fs = static_cast<FireboltStatement *>(stmt->private_data);
    fs->sql = query;
    // Bound payload is tied to the previous SQL and must be dropped when the
    // statement text changes.
    fs->bound.reset();
    return ADBC_STATUS_OK;
}

static AdbcStatusCode StatementSetSubstraitPlan(AdbcStatement * /*stmt*/, const uint8_t * /*plan*/, size_t /*length*/, AdbcError * error)
{
    return SetError(error, ADBC_STATUS_NOT_IMPLEMENTED, "Substrait plans not supported");
}

// Issues no request: Firebolt has no server-side prepare, a parameterised statement
// being sent whole every time with `$N` resolved from the `query_parameters` setting.
// ADBC allows this — Prepare is optional on the way to execution, promises only that
// the statement is reusable, and leaves parameter metadata to GetParameterSchema.  A
// round-trip here would double the request count of every `cursor.execute()`, which
// calls Prepare whenever the query text changes.
//
// Returns OK rather than NOT_IMPLEMENTED because the statement is in fact reusable
// with new parameters, which is what Prepare promises.
static AdbcStatusCode StatementPrepare(AdbcStatement * stmt, AdbcError * error)
{
    if (!stmt || !stmt->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Statement not initialized");
    return ADBC_STATUS_OK;
}

// Snapshot the bound stream's schema into fs->ingest->schema (deep copy).
// Caller must have already populated fs->ingest.  The schema is needed after
// Bind/BindStream returns so that ExecuteQuery can build CREATE TABLE /
// INSERT statements with column names and types.
static AdbcStatusCode CaptureBoundSchema(FireboltStatement * fs, const ArrowSchema * schema, AdbcError * error)
{
    fs->bound->schema.reset();
    if (!schema || !schema->release)
        return ADBC_STATUS_OK;
    if (ArrowSchemaDeepCopy(schema, fs->bound->schema.get()) != 0)
        return SetError(error, ADBC_STATUS_INTERNAL, "ArrowSchemaDeepCopy failed");
    return ADBC_STATUS_OK;
}

// Both Bind entry points take ownership of what they are handed; whatever is not
// retained is released here.
static AdbcStatusCode StatementBind(AdbcStatement * stmt, ArrowArray * values, ArrowSchema * schema, AdbcError * error)
{
    if (!stmt || !stmt->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Statement not initialized");
    auto * fs = static_cast<FireboltStatement *>(stmt->private_data);

    auto & bound = fs->initAndGetBoundData();
    bound.batches.clear();
    bound.data_bound = false;

    // The statement keeps a deep copy of the schema, so the caller's goes either way;
    // the array is moved in.
    AdbcStatusCode capture_rc = CaptureBoundSchema(fs, schema, error);
    if (schema && schema->release)
        schema->release(schema);
    if (capture_rc != ADBC_STATUS_OK)
    {
        if (values && values->release)
            values->release(values);
        return capture_rc;
    }

    if (values && values->release)
    {
        nanoarrow::UniqueArray batch;
        ArrowArrayMove(values, batch.get());
        bound.batches.push_back(std::move(batch));
        bound.data_bound = true;
    }

    return ADBC_STATUS_OK;
}

static AdbcStatusCode StatementBindStream(AdbcStatement * stmt, ArrowArrayStream * stream, AdbcError * error)
{
    if (!stmt || !stmt->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Statement not initialized");
    auto * fs = static_cast<FireboltStatement *>(stmt->private_data);

    auto & bound = fs->initAndGetBoundData();
    bound.batches.clear();
    bound.data_bound = false;

    // The driver owns the stream from here on, including on every failure path.
    nanoarrow::UniqueArrayStream owned_stream;
    if (stream && stream->release)
        ArrowArrayStreamMove(stream, owned_stream.get());

    // Capture the stream schema (deep-copied) before draining the stream.
    bound.schema.reset();
    if (owned_stream->release && owned_stream->get_schema)
    {
        ArrowSchema schema{};
        if (owned_stream->get_schema(owned_stream.get(), &schema) == 0)
        {
            AdbcStatusCode capture_rc = CaptureBoundSchema(fs, &schema, error);
            if (schema.release)
                schema.release(&schema);
            if (capture_rc != ADBC_STATUS_OK)
                return capture_rc;
        }
    }

    // Drained now, not at execution: a stream is single-pass, and the caller may
    // release its side once Bind returns.
    while (owned_stream->release && owned_stream->get_next)
    {
        nanoarrow::UniqueArray batch;
        if (owned_stream->get_next(owned_stream.get(), batch.get()) != 0)
        {
            const char * message = owned_stream->get_last_error ? owned_stream->get_last_error(owned_stream.get()) : nullptr;
            bound.batches.clear();
            return SetError(
                error, ADBC_STATUS_INVALID_ARGUMENT, std::string("Cannot read the bound data: ") + (message ? message : "unknown error"));
        }
        if (!batch->release)
            break;
        bound.batches.push_back(std::move(batch));
    }
    // A stream that yielded nothing is still bound data (a zero-row create ingest
    // uploads a schema-only payload and makes an empty table).  No stream is not.
    bound.data_bound = owned_stream->release != nullptr;

    return ADBC_STATUS_OK;
}

// ============================================================
// Query parameters
// ============================================================

// Render the bound Arrow data as one `query_parameters` setting value per row — one
// parameter set, and so one execution, each.  Reads the retained ArrowArrays
// directly: none of this leaves as Arrow IPC, so none of it is encoded as Arrow IPC.
static AdbcStatusCode BuildParameterSets(const BoundData & bound, bool bind_by_name, std::vector<std::string> & out, AdbcError * error)
{
    out.clear();

    const ArrowSchema * schema = bound.schema.get();
    if (!schema->release || !schema->format || strcmp(schema->format, "+s") != 0)
        return SetError(error, ADBC_STATUS_INVALID_ARGUMENT, "Bound parameters must be a record batch");

    for (const nanoarrow::UniqueArray & batch : bound.batches)
    {
        ArrowArrayView view{};
        memset(&view, 0, sizeof(view));
        FIREBOLT_SCOPE_GUARD(ArrowArrayViewReset(&view));

        ArrowError arrow_error{};
        if (ArrowArrayViewInitFromSchema(&view, schema, &arrow_error) != 0)
            return SetError(
                error, ADBC_STATUS_INVALID_ARGUMENT, std::string("Bound parameter schema is unsupported: ") + arrow_error.message);
        if (ArrowArrayViewSetArray(&view, batch.get(), &arrow_error) != 0)
            return SetError(
                error, ADBC_STATUS_INVALID_ARGUMENT, std::string("Bound parameter data is unsupported: ") + arrow_error.message);

        for (int64_t row = 0; row < batch->length; ++row)
        {
            std::string json;
            std::string message;
            AdbcStatusCode rc = buildQueryParametersJson(schema, &view, row, bind_by_name, json, message);
            if (rc != ADBC_STATUS_OK)
                return SetError(error, rc, message);
            out.push_back(std::move(json));
        }
    }

    return ADBC_STATUS_OK;
}

// ============================================================
// Parameter metadata (execution_mode=describe_parameters)
// ============================================================

// Pull the single string cell out of a describe response.
static AdbcStatusCode ReadDescribePayload(std::vector<uint8_t> body, std::string & out, AdbcError * error)
{
    nanoarrow::UniqueArrayStream stream;
    std::string err = ExportIpcBytesAsArrowStream(std::move(body), stream.get());
    if (!err.empty())
        return SetError(error, ADBC_STATUS_INTERNAL, "Cannot read the describe response: " + err);

    nanoarrow::UniqueSchema schema;
    if (stream->get_schema(stream.get(), schema.get()) != 0 || schema->n_children != 1)
        return SetError(error, ADBC_STATUS_INTERNAL, "Unexpected describe response shape");

    nanoarrow::UniqueArray batch;
    if (stream->get_next(stream.get(), batch.get()) != 0 || !batch->release || batch->length < 1)
        return SetError(error, ADBC_STATUS_INTERNAL, "Empty describe response");

    ArrowArrayView view{};
    memset(&view, 0, sizeof(view));
    FIREBOLT_SCOPE_GUARD(ArrowArrayViewReset(&view));
    if (ArrowArrayViewInitFromSchema(&view, schema->children[0], nullptr) != 0
        || ArrowArrayViewSetArray(&view, batch->children[0], nullptr) != 0)
        return SetError(error, ADBC_STATUS_INTERNAL, "Unexpected describe response type");
    if (ArrowArrayViewIsNull(&view, 0) || (view.storage_type != NANOARROW_TYPE_STRING && view.storage_type != NANOARROW_TYPE_LARGE_STRING))
        return SetError(error, ADBC_STATUS_INTERNAL, "Unexpected describe response type");

    ArrowStringView s = ArrowArrayViewGetStringUnsafe(&view, 0);
    out.assign(s.data, static_cast<size_t>(s.size_bytes));
    return ADBC_STATUS_OK;
}

// Ask the server to validate the statement and report its parameter types without
// executing it.
//
// Not cached: the types come from the objects the statement names, so DDL from any
// connection changes the answer while the text stays the same, and there is no
// invalidation event the driver can see (dbapi re-issues SetSqlQuery only when the
// text changes).  Asking every time costs one request on a rare explicit call —
// GetParameterSchema is reached only from `cursor.adbc_prepare()`.
static AdbcStatusCode Describe(FireboltStatement * fs, std::string & out_payload, AdbcError * error)
{
    auto * conn = fs->conn;

    std::unordered_map<std::string, std::string> params = conn->session_params;
    // Type inference must not join an open transaction, which would spend a
    // transaction step on a statement the caller never ran.  The server's own
    // PostgreSQL handler excludes it the same way.
    params.erase(TRANSACTION_ID_PARAM);
    params.erase(TRANSACTION_SEQUENCE_PARAM);
    params[EXECUTION_MODE_SETTING] = DESCRIBE_PARAMETERS_MODE;

    auto resp = conn->http->executeQuery(fs->sql, params);
    // Session updates from the probe are not applied: it was never part of the
    // session's transaction.
    if (!resp.isSuccess())
        return HttpRespToStatus(resp, error);

    return ReadDescribePayload(std::move(resp.body), out_payload, error);
}

static AdbcStatusCode StatementGetParameterSchema(AdbcStatement * stmt, ArrowSchema * schema, AdbcError * error)
{
    if (!stmt || !stmt->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Statement not initialized");
    if (!schema)
        return SetError(error, ADBC_STATUS_INVALID_ARGUMENT, "Output schema is null");
    auto * fs = static_cast<FireboltStatement *>(stmt->private_data);
    if (!fs->conn || !fs->conn->http)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Connection not initialized");
    if (fs->sql.empty())
        return SetError(error, ADBC_STATUS_INVALID_STATE, "No query has been set");

    try
    {
        std::string payload;
        AdbcStatusCode rc = Describe(fs, payload, error);
        if (rc != ADBC_STATUS_OK)
            return rc;

        std::string message;
        rc = buildParameterSchema(payload, schema, message);
        if (rc != ADBC_STATUS_OK)
            return SetError(error, rc, message);
        return ADBC_STATUS_OK;
    }
    catch (std::exception & ex)
    {
        return SetError(error, ADBC_STATUS_INTERNAL, ex.what());
    }
}

static AdbcStatusCode StatementSetOption(AdbcStatement * stmt, const char * key, const char * value, AdbcError * error)
{
    if (!stmt || !stmt->private_data)
        return SetError(error, ADBC_STATUS_INVALID_STATE, "Statement not initialized");
    auto * fs = static_cast<FireboltStatement *>(stmt->private_data);
    if (!key)
        return SetError(error, ADBC_STATUS_INVALID_ARGUMENT, "Option key is null");

    const std::string_view k{key};
    const std::string v(value ? value : "");

    if (k == FIREBOLT_ADBC_STATEMENT_OPTION_BIND_BY_NAME)
    {
        // Adds a name alias per bound column, for statements reading a parameter
        // through `param('name')`.
        if (v == ADBC_OPTION_VALUE_ENABLED)
            fs->bind_by_name = true;
        else if (v == ADBC_OPTION_VALUE_DISABLED)
            fs->bind_by_name = false;
        else
            return SetError(
                error,
                ADBC_STATUS_INVALID_ARGUMENT,
                std::string("Invalid value for ") + FIREBOLT_ADBC_STATEMENT_OPTION_BIND_BY_NAME + ": expected \""
                    + ADBC_OPTION_VALUE_ENABLED + "\" or \"" + ADBC_OPTION_VALUE_DISABLED + "\", got \"" + v + "\"");
        return ADBC_STATUS_OK;
    }
    if (k == ADBC_INGEST_OPTION_TARGET_TABLE)
    {
        fs->initAndGetBoundData().target_table = v;
        return ADBC_STATUS_OK;
    }
    if (k == ADBC_INGEST_OPTION_TARGET_CATALOG)
    {
        auto & ingest = fs->initAndGetBoundData();
        ingest.target_catalog = v;
        ingest.ingest_options_set = true;
        return ADBC_STATUS_OK;
    }
    if (k == ADBC_INGEST_OPTION_TARGET_DB_SCHEMA)
    {
        auto & ingest = fs->initAndGetBoundData();
        ingest.target_db_schema = v;
        ingest.ingest_options_set = true;
        return ADBC_STATUS_OK;
    }
    if (k == ADBC_INGEST_OPTION_MODE)
    {
        auto & ingest = fs->initAndGetBoundData();
        if (v == ADBC_INGEST_OPTION_MODE_APPEND)
            ingest.mode = IngestMode::Append;
        else if (v == ADBC_INGEST_OPTION_MODE_CREATE)
            ingest.mode = IngestMode::Create;
        else if (v == ADBC_INGEST_OPTION_MODE_REPLACE)
            ingest.mode = IngestMode::Replace;
        else if (v == ADBC_INGEST_OPTION_MODE_CREATE_APPEND)
            ingest.mode = IngestMode::CreateAppend;
        else
            // A rejected value changes nothing, the flag included.
            return SetError(error, ADBC_STATUS_INVALID_ARGUMENT, "Unknown ingest mode: " + v);
        ingest.ingest_options_set = true;
        return ADBC_STATUS_OK;
    }
    if (k == ADBC_INGEST_OPTION_TEMPORARY)
    {
        // Firebolt has no notion of session-temporary tables.  Accept the no-op
        // value silently so dbapi callers that pass temporary=False work.  Any
        // other value (including the standard "true") is reported as not
        // implemented so the dbapi cursor's NotSupportedError fallback can
        // engage.
        if (v == ADBC_OPTION_VALUE_DISABLED || v.empty())
            return ADBC_STATUS_OK;
        return SetError(error, ADBC_STATUS_NOT_IMPLEMENTED, "Temporary ingest tables are not supported");
    }
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

    // Clear bound state on every exit path so a bound payload is consumed
    // exactly once (including failures/exceptions).
    FIREBOLT_SCOPE_GUARD(if (fs->bound.has_value()) fs->bound.reset());

    try
    {
        // Bound data is either a bulk-ingest payload or a set of query parameters,
        // told apart by whether an ingest target table was set.
        const bool has_data = fs->bound && fs->bound->data_bound;
        const bool is_ingest = fs->bound && !fs->bound->target_table.empty();

        // The target table is what routes the payload to the ingest path, so without
        // it the other ingest options mean nothing: a mistyped or forgotten target
        // table would otherwise become N parameterised executions of the caller's raw
        // SQL.  Checked here, not where the options arrive, because ADBC does not
        // order option calls — the mode may legitimately be set first.
        if (fs->bound && !is_ingest && fs->bound->ingest_options_set)
            return SetError(
                error,
                ADBC_STATUS_INVALID_STATE,
                "Ingest options were set without " ADBC_INGEST_OPTION_TARGET_TABLE
                "; set the target table, or unset " ADBC_INGEST_OPTION_MODE " / " ADBC_INGEST_OPTION_TARGET_CATALOG
                " / " ADBC_INGEST_OPTION_TARGET_DB_SCHEMA " to bind the data as query parameters instead");

        // One `query_parameters` value per bound row, one execution each.  Built
        // before anything is sent, so an unrepresentable value cannot leave a
        // transaction open behind it.
        std::vector<std::string> param_sets;
        if (has_data && !is_ingest)
        {
            AdbcStatusCode rc = BuildParameterSets(*fs->bound, fs->bind_by_name, param_sets, error);
            if (rc != ADBC_STATUS_OK)
                return rc;

            if (param_sets.empty())
            {
                // Nothing bound, nothing to execute: one run with no parameters would
                // fail on the first `$N`.
                if (out)
                    return SetError(
                        error, ADBC_STATUS_INVALID_STATE, "No parameter sets were bound; a result-returning execution needs one.");
                if (rows_affected)
                    *rows_affected = 0;
                return ADBC_STATUS_OK;
            }
        }

        // Lazy BEGIN: start a transaction on the first statement when autocommit is off.
        if (!conn->autocommit && !conn->in_transaction)
        {
            AdbcStatusCode rc = RunSimpleSql(conn, "BEGIN", error);
            if (rc != ADBC_STATUS_OK)
                return rc;
            conn->in_transaction = true;
        }

        // Atomicity gate: Replace/Create/CreateAppend run DDL before the multipart
        // INSERT, so with nothing bound Replace would drop the table and leave it
        // empty.  Rejected before any pre-SQL is generated, so DDL never runs
        // without DML.
        if (is_ingest && !has_data)
            return SetError(
                error, ADBC_STATUS_INVALID_STATE, "Ingest target set but no data bound; call Bind/BindStream before ExecuteQuery");

        // Bulk ingest: synthesise CREATE / DROP / INSERT statements based on the
        // configured mode and the deep-copied bound schema.  Regenerated on every
        // call to support statement reuse.  Each pre-SQL entry is sent as a
        // separate HTTP request — Firebolt rejects multiple statements in one body.
        // The generated INSERT goes into a local, leaving the statement's own SQL
        // text untouched so it stays re-executable.
        std::string sql = fs->sql;
        std::vector<std::string> pre_sql;
        std::vector<uint8_t> ipc_bytes;
        if (is_ingest)
        {
            AdbcStatusCode rc = buildIngestSql(fs, pre_sql, sql, error);
            if (rc != ADBC_STATUS_OK)
                return rc;

            // The upload is the only consumer of Arrow IPC.  Encoded before any
            // pre-SQL runs, for the reason the gate above exists: an encoding failure
            // must not leave a Replace having dropped the target.
            rc = SerializeBoundData(*fs->bound, ipc_bytes, error);
            if (rc != ADBC_STATUS_OK)
                return rc;
        }

        for (const auto & stmt_sql : pre_sql)
        {
            AdbcStatusCode pre_rc = RunSimpleSql(conn, stmt_sql.c_str(), error);
            if (pre_rc != ADBC_STATUS_OK)
                return pre_rc;
        }

        HttpResponse resp;
        if (is_ingest)
        {
            resp = conn->http->executeInsert(sql, ipc_bytes, conn->session_params);
            ApplySessionUpdates(conn, resp);
            if (!resp.isSuccess())
                return HttpRespToStatus(resp, error);
        }
        else if (!param_sets.empty())
        {
            // One request per parameter set — "the query is executed once per row of
            // the bound data", for ExecuteQuery as much as ExecuteUpdate.  A failure
            // stops the run; earlier executions stay applied, which is what a
            // transaction is for.  A requested result set is the last execution's,
            // there being one ArrowArrayStream to hand back.
            for (const auto & param_set : param_sets)
            {
                // A per-request copy: a caller's own `query_parameters` session
                // parameter is overridden for this request, not mutated.
                std::unordered_map<std::string, std::string> params = conn->session_params;
                params[QUERY_PARAMETERS_SETTING] = param_set;

                resp = conn->http->executeQuery(sql, params);
                ApplySessionUpdates(conn, resp);
                if (!resp.isSuccess())
                    return HttpRespToStatus(resp, error);
            }
        }
        else
        {
            resp = conn->http->executeQuery(sql, conn->session_params);
            ApplySessionUpdates(conn, resp);
            if (!resp.isSuccess())
                return HttpRespToStatus(resp, error);
        }

        // Bound state is cleared by FIREBOLT_SCOPE_GUARD at function exit.

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

// The driver-specific entry point a driver manager derives from the driver name
// ("firebolt" -> AdbcDriverFireboltInit), so several drivers can share a process.
extern "C" __attribute__((visibility("default"))) AdbcStatusCode AdbcDriverFireboltInit(int version, void * raw_driver, AdbcError * error)
{
    return AdbcDriverInit(version, raw_driver, error);
}
