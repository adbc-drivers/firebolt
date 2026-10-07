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

#include <gtest/gtest.h>

#include "ArrowIpcStream.h"
#include "DescribeParameters.h"
#include "FireboltAdbcConnection.h"
#include "FireboltAdbcDatabase.h"
#include "FireboltAdbcStatement.h"
#include "IngestSqlBuilder.h"
#include "QueryParameters.h"
#include "adbc.h"

#include <nanoarrow/nanoarrow.hpp>
#include <nanoarrow/nanoarrow_ipc.hpp>

#include <curl/curl.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#    include <windows.h>
#else
#    include <dlfcn.h>
#endif

// Entry points defined in FireboltAdbcDriver.cpp
extern "C" AdbcStatusCode AdbcDriverInit(int version, void * raw_driver, AdbcError * error);
extern "C" AdbcStatusCode AdbcDriverFireboltInit(int version, void * raw_driver, AdbcError * error);

// Whether the libcurl this driver is linked against can speak TLS. The test is
// conditioned on this rather than on a build-time define, so it also remains
// correct for an explicit -DWITH_SSL=OFF build.
static bool CurlHasTls()
{
    const curl_version_info_data * v = curl_version_info(CURLVERSION_NOW);
    return v != nullptr && (v->features & CURL_VERSION_SSL) != 0;
}

// ============================================================
// Helper: call AdbcDriverInit and return a populated driver
// ============================================================

static AdbcDriver InitDriver()
{
    AdbcDriver driver{};
    AdbcError error = ADBC_ERROR_INIT;
    AdbcStatusCode code = AdbcDriverInit(ADBC_VERSION_1_1_0, &driver, &error);
    EXPECT_EQ(code, ADBC_STATUS_OK);
    if (error.release)
        error.release(&error);
    return driver;
}

// ============================================================
// Helper: build Arrow schemas for the type-mapping tests
// ============================================================

namespace
{

void * OpenLibrary(const char * path)
{
#if defined(_WIN32)
    return reinterpret_cast<void *>(LoadLibraryA(path));
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

void * FindSymbol(void * library, const char * name)
{
#if defined(_WIN32)
    return reinterpret_cast<void *>(GetProcAddress(reinterpret_cast<HMODULE>(library), name));
#else
    return dlsym(library, name);
#endif
}

void CloseLibrary(void * library)
{
#if defined(_WIN32)
    FreeLibrary(reinterpret_cast<HMODULE>(library));
#else
    dlclose(library);
#endif
}

// Build an initialised top-level (struct) schema with `n_columns` unset columns,
// ready for the caller to type and name each one.
nanoarrow::UniqueSchema MakeTopLevelSchema(int64_t n_columns)
{
    nanoarrow::UniqueSchema schema;
    ArrowSchemaInit(schema.get());
    EXPECT_EQ(ArrowSchemaSetTypeStruct(schema.get(), n_columns), 0);
    return schema;
}

// Build a top-level struct schema with one column for the given type and name.
nanoarrow::UniqueSchema MakeStructSchemaWithColumn(ArrowType column_type, const std::string & column_name, bool nullable = true)
{
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    EXPECT_EQ(ArrowSchemaSetType(schema->children[0], column_type), 0);
    EXPECT_EQ(ArrowSchemaSetName(schema->children[0], column_name.c_str()), 0);
    if (!nullable)
        schema->children[0]->flags &= ~ARROW_FLAG_NULLABLE;
    return schema;
}

// Build a one-column batch from integral values — usable for every type whose
// storage is an integer, which is most of them (ints, bool, date, time, timestamp).
nanoarrow::UniqueArray MakeOneColumnBatch(const ArrowSchema * schema, const std::vector<int64_t> & values)
{
    nanoarrow::UniqueArray batch;
    EXPECT_EQ(ArrowArrayInitFromSchema(batch.get(), schema, nullptr), 0);
    EXPECT_EQ(ArrowArrayStartAppending(batch.get()), 0);
    for (int64_t value : values)
    {
        EXPECT_EQ(ArrowArrayAppendInt(batch->children[0], value), 0);
        EXPECT_EQ(ArrowArrayFinishElement(batch.get()), 0);
    }
    EXPECT_EQ(ArrowArrayFinishBuilding(batch.get(), NANOARROW_VALIDATION_LEVEL_DEFAULT, nullptr), 0);
    return batch;
}

// Render one row of a batch as the `query_parameters` setting value, the way the
// driver does before a parameterised execution.
AdbcStatusCode RenderParameterSet(
    const ArrowSchema * schema, const ArrowArray * batch, int64_t row, bool bind_by_name, std::string & out_json, std::string & out_error)
{
    ArrowArrayView view{};
    memset(&view, 0, sizeof(view));
    ArrowError arrow_error{};
    EXPECT_EQ(ArrowArrayViewInitFromSchema(&view, schema, &arrow_error), 0) << arrow_error.message;
    EXPECT_EQ(ArrowArrayViewSetArray(&view, batch, &arrow_error), 0) << arrow_error.message;
    AdbcStatusCode rc = firebolt::adbc::buildQueryParametersJson(schema, &view, row, bind_by_name, out_json, out_error);
    ArrowArrayViewReset(&view);
    return rc;
}

// The common shape: one column of one integral value, rendered positionally.
std::string RenderOneIntegral(ArrowType column_type, int64_t value)
{
    nanoarrow::UniqueSchema schema = MakeStructSchemaWithColumn(column_type, "0");
    nanoarrow::UniqueArray batch = MakeOneColumnBatch(schema.get(), {value});
    std::string json;
    std::string error;
    EXPECT_EQ(RenderParameterSet(schema.get(), batch.get(), 0, false, json, error), ADBC_STATUS_OK) << error;
    return json;
}

} // namespace

// ============================================================
// Tests: driver init
// ============================================================

TEST(AdbcDriverInitTest, InitSucceeds)
{
    AdbcDriver driver{};
    AdbcError error = ADBC_ERROR_INIT;
    AdbcStatusCode code = AdbcDriverInit(ADBC_VERSION_1_1_0, &driver, &error);
    EXPECT_EQ(code, ADBC_STATUS_OK);
    EXPECT_NE(driver.DatabaseNew, nullptr);
    EXPECT_NE(driver.StatementExecuteQuery, nullptr);
    if (error.release)
        error.release(&error);
}

TEST(AdbcDriverInitTest, UnsupportedVersionFails)
{
    AdbcDriver driver{};
    AdbcError error = ADBC_ERROR_INIT;
    AdbcStatusCode code = AdbcDriverInit(999999, &driver, &error);
    EXPECT_EQ(code, ADBC_STATUS_NOT_IMPLEMENTED);
    if (error.release)
        error.release(&error);
}

TEST(AdbcDriverInitTest, FireboltEntryPoint)
{
    AdbcDriver driver{};
    AdbcError error = ADBC_ERROR_INIT;
    AdbcStatusCode code = AdbcDriverFireboltInit(ADBC_VERSION_1_1_0, &driver, &error);
    EXPECT_EQ(code, ADBC_STATUS_OK);
    if (error.release)
        error.release(&error);
}

// Open the shared library directly and check the platform export allowlist: the
// generic entry point, the driver-specific one a driver manager derives from
// the driver name, and nothing outside `Adbc*`.
TEST(AdbcDriverInitTest, ExportsOnlyAdbcEntryPoints)
{
    void * library = OpenLibrary(FIREBOLT_ADBC_DRIVER_LIBRARY_PATH);
    ASSERT_NE(library, nullptr);
    EXPECT_NE(FindSymbol(library, "AdbcDriverInit"), nullptr);
    EXPECT_NE(FindSymbol(library, "AdbcDriverFireboltInit"), nullptr);
    EXPECT_EQ(FindSymbol(library, "FireboltAdbcDriverInit"), nullptr);
    CloseLibrary(library);
}

// ============================================================
// Tests: database lifecycle
// ============================================================

TEST(DatabaseTest, NewInitRelease)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcError error = ADBC_ERROR_INIT;

    ASSERT_EQ(driver.DatabaseNew(&db, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "uri", "http://localhost:9123", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "firebolt.token", "tok", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "firebolt.database", "mydb", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseInit(&db, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseRelease(&db, &error), ADBC_STATUS_OK);
    if (error.release)
        error.release(&error);
}

TEST(DatabaseTest, InitWithoutUrlFails)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcError error = ADBC_ERROR_INIT;

    ASSERT_EQ(driver.DatabaseNew(&db, &error), ADBC_STATUS_OK);
    AdbcStatusCode code = driver.DatabaseInit(&db, &error);
    EXPECT_EQ(code, ADBC_STATUS_INVALID_ARGUMENT);
    EXPECT_NE(error.message, nullptr);

    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

// ============================================================
// Tests: database option validation — an unusable option must
// not be accepted silently.  Every case below used to return
// ADBC_STATUS_OK (or throw across the C ABI), so a typo or a
// malformed value produced a connection that quietly did the
// wrong thing.
//
// A rejected option is reported by DatabaseInit, not by
// DatabaseSetOption, and deliberately so: see RejectOption in
// FireboltAdbcDriver.cpp — the driver manager replays pre-Init
// options from inside AdbcDatabaseInit and its failure path
// there overflows a heap buffer by one byte.
// ============================================================

namespace
{

// DatabaseNew + one SetOption + Init.  Returns the Init status, which is where a
// bad option surfaces; `error` is left populated for the caller.
AdbcStatusCode InitWithOption(AdbcDriver & driver, const char * key, const char * value, AdbcError * error)
{
    AdbcDatabase db{};
    EXPECT_EQ(driver.DatabaseNew(&db, error), ADBC_STATUS_OK);
    // A valid uri, so that Init fails on the option under test and nothing else.
    EXPECT_EQ(driver.DatabaseSetOption(&db, "uri", "http://localhost:3473", error), ADBC_STATUS_OK);
    EXPECT_EQ(driver.DatabaseSetOption(&db, key, value, error), ADBC_STATUS_OK)
        << "a pre-Init option must not be refused on the spot; Init reports it";
    AdbcStatusCode code = driver.DatabaseInit(&db, error);
    driver.DatabaseRelease(&db, nullptr);
    return code;
}

} // namespace

TEST(DatabaseOptionTest, UnknownFireboltOptionRejected)
{
    // A typo in a driver-namespaced key is a configuration bug, not something
    // to swallow: `firebolt.databse` would otherwise leave the connection // codespell:ignore
    // pointed at the server's default database with no diagnostic anywhere.
    AdbcDriver driver = InitDriver();
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(InitWithOption(driver, "firebolt.databse", "mydb", &error), ADBC_STATUS_NOT_FOUND); // codespell:ignore
    ASSERT_NE(error.message, nullptr);
    EXPECT_NE(std::string(error.message).find("firebolt.databse"), std::string::npos) // codespell:ignore
        << "the error should name the offending key, got: " << error.message;
    if (error.release)
        error.release(&error);
}

TEST(DatabaseOptionTest, UnknownFireboltOptionRejectedImmediatelyAfterInit)
{
    // Past Init there is no option replay, so there is no reason to defer.
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.DatabaseNew(&db, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "uri", "http://localhost:3473", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseInit(&db, &error), ADBC_STATUS_OK);
    EXPECT_EQ(driver.DatabaseSetOption(&db, "firebolt.nonsense", "x", &error), ADBC_STATUS_NOT_FOUND);
    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

TEST(DatabaseOptionTest, EndpointAndCaBundleRefusedAfterInit)
{
    // Both are validated and resolved by DatabaseInit (scheme check, CA bundle).
    // Accepting a change afterwards would skip that: an http:// database moved
    // to https:// would have no CA bundle, and a new bundle would never be used.
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.DatabaseNew(&db, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "uri", "http://localhost:3473", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseInit(&db, &error), ADBC_STATUS_OK);
    for (const char * key : {"uri", "firebolt.ssl_certificate_path"})
    {
        EXPECT_EQ(driver.DatabaseSetOption(&db, key, "https://elsewhere.example.com", &error), ADBC_STATUS_INVALID_STATE) << key;
        ASSERT_NE(error.message, nullptr) << key;
        EXPECT_NE(std::string(error.message).find(key), std::string::npos) << error.message;
        error.release(&error);
        error = ADBC_ERROR_INIT;
    }
    driver.DatabaseRelease(&db, nullptr);
}

TEST(DatabaseOptionTest, UnknownNonNamespacedOptionRejected)
{
    AdbcDriver driver = InitDriver();
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(InitWithOption(driver, "max_retries", "3", &error), ADBC_STATUS_NOT_IMPLEMENTED);
    if (error.release)
        error.release(&error);
}

TEST(DatabaseOptionTest, NonNumericTimeoutRejected)
{
    // std::stol throws std::invalid_argument on this input.  The throw escaped
    // through the C ABI boundary into a driver manager with no handler, which
    // aborts the host process.  It has to become a status code.
    AdbcDriver driver = InitDriver();
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(InitWithOption(driver, "firebolt.timeout_sec", "soon", &error), ADBC_STATUS_INVALID_ARGUMENT);
    EXPECT_NE(error.message, nullptr);
    if (error.release)
        error.release(&error);
}

TEST(DatabaseOptionTest, TimeoutWithTrailingGarbageRejected)
{
    // std::stol would happily parse "30s" as 30 and drop the suffix.
    AdbcDriver driver = InitDriver();
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(InitWithOption(driver, "firebolt.timeout_sec", "30s", &error), ADBC_STATUS_INVALID_ARGUMENT);
    if (error.release)
        error.release(&error);
}

TEST(DatabaseOptionTest, OutOfRangeTimeoutRejected)
{
    // std::out_of_range, same C ABI problem as above.
    AdbcDriver driver = InitDriver();
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(InitWithOption(driver, "firebolt.timeout_sec", "99999999999999999999999", &error), ADBC_STATUS_INVALID_ARGUMENT);
    if (error.release)
        error.release(&error);
}

TEST(DatabaseOptionTest, NegativeTimeoutRejected)
{
    AdbcDriver driver = InitDriver();
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(InitWithOption(driver, "firebolt.timeout_sec", "-5", &error), ADBC_STATUS_INVALID_ARGUMENT);
    if (error.release)
        error.release(&error);
}

TEST(DatabaseOptionTest, ValidTimeoutAccepted)
{
    AdbcDriver driver = InitDriver();
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(InitWithOption(driver, "firebolt.timeout_sec", "30", &error), ADBC_STATUS_OK);
    EXPECT_EQ(InitWithOption(driver, "firebolt.timeout_sec", "0", &error), ADBC_STATUS_OK);
    if (error.release)
        error.release(&error);
}

TEST(DatabaseOptionTest, FirstRejectedOptionIsTheOneReported)
{
    // Several bad options: the first is kept so the message is deterministic.
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.DatabaseNew(&db, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "uri", "http://localhost:3473", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "firebolt.first_typo", "a", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "firebolt.second_typo", "b", &error), ADBC_STATUS_OK);
    EXPECT_EQ(driver.DatabaseInit(&db, &error), ADBC_STATUS_NOT_FOUND);
    ASSERT_NE(error.message, nullptr);
    EXPECT_NE(std::string(error.message).find("first_typo"), std::string::npos) << "got: " << error.message;
    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

// ============================================================
// Tests: DatabaseInit — uri validation.  Without these, a bad
// scheme surfaces as a bare libcurl string ("Unsupported
// protocol", "URL using bad/illegal format") at first query,
// far from the option that caused it.
// ============================================================

namespace
{

AdbcStatusCode InitWithUri(AdbcDriver & driver, const char * uri, AdbcError * error)
{
    AdbcDatabase db{};
    EXPECT_EQ(driver.DatabaseNew(&db, error), ADBC_STATUS_OK);
    EXPECT_EQ(driver.DatabaseSetOption(&db, "uri", uri, error), ADBC_STATUS_OK);
    AdbcStatusCode code = driver.DatabaseInit(&db, error);
    driver.DatabaseRelease(&db, nullptr);
    return code;
}

} // namespace

TEST(DatabaseInitTest, UriWithoutSchemeRejected)
{
    AdbcDriver driver = InitDriver();
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(InitWithUri(driver, "localhost:3473", &error), ADBC_STATUS_INVALID_ARGUMENT);
    EXPECT_NE(error.message, nullptr);
    if (error.release)
        error.release(&error);
}

TEST(DatabaseInitTest, UnsupportedSchemeRejected)
{
    AdbcDriver driver = InitDriver();
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(InitWithUri(driver, "ftp://localhost:3473/db", &error), ADBC_STATUS_INVALID_ARGUMENT);
    if (error.release)
        error.release(&error);
}

TEST(DatabaseInitTest, PlainHttpUriAccepted)
{
    AdbcDriver driver = InitDriver();
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(InitWithUri(driver, "http://localhost:3473", &error), ADBC_STATUS_OK);
    if (error.release)
        error.release(&error);
}

TEST(DatabaseInitTest, HttpsUriRejectedWhenCurlHasNoTls)
{
    // A driver built with -DWITH_SSL=OFF can never reach https://.  Say so at
    // Init, naming the limitation, instead of letting the first query fail with
    // CURLE_UNSUPPORTED_PROTOCOL.  A TLS build (what ships) must accept it.
    //
    // The CA bundle is named explicitly so the outcome does not depend on the
    // host's certificates or SSL_CERT_FILE: test-unit.sh runs on the host.
    const std::filesystem::path ca_path = std::filesystem::temp_directory_path() / "firebolt-adbc-test-ca.pem";
    {
        std::ofstream ca_file(ca_path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(ca_file.good());
    }
    const std::string ca_path_string = ca_path.string();

    AdbcDriver driver = InitDriver();
    AdbcError error = ADBC_ERROR_INIT;
    AdbcDatabase db{};
    ASSERT_EQ(driver.DatabaseNew(&db, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "uri", "https://api.example.com", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "firebolt.ssl_certificate_path", ca_path_string.c_str(), &error), ADBC_STATUS_OK);
    AdbcStatusCode code = driver.DatabaseInit(&db, &error);
    driver.DatabaseRelease(&db, nullptr);
    std::filesystem::remove(ca_path);
    if (CurlHasTls())
    {
        EXPECT_EQ(code, ADBC_STATUS_OK) << "this build has TLS; https:// must be accepted";
    }
    else
    {
        EXPECT_EQ(code, ADBC_STATUS_INVALID_ARGUMENT);
        ASSERT_NE(error.message, nullptr);
        EXPECT_NE(std::string(error.message).find("TLS"), std::string::npos)
            << "error should name the missing capability, got: " << error.message;
    }
    if (error.release)
        error.release(&error);
}

TEST(DatabaseInitTest, MissingSslCertificatePathRejectedAtInit)
{
    // A CA bundle the caller named but that does not exist would otherwise
    // surface on the first query as an opaque certificate failure.
    AdbcDriver driver = InitDriver();
    AdbcError error = ADBC_ERROR_INIT;
    AdbcDatabase db{};
    ASSERT_EQ(driver.DatabaseNew(&db, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "uri", "https://api.example.com", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "firebolt.ssl_certificate_path", "/no/such/ca.pem", &error), ADBC_STATUS_OK);
    AdbcStatusCode code = driver.DatabaseInit(&db, &error);
    driver.DatabaseRelease(&db, nullptr);
    EXPECT_EQ(code, ADBC_STATUS_INVALID_ARGUMENT);
    ASSERT_NE(error.message, nullptr);
    // Without TLS the https:// rejection comes first; either way the caller is told why.
    if (CurlHasTls())
        EXPECT_NE(std::string(error.message).find("/no/such/ca.pem"), std::string::npos) << error.message;
    if (error.release)
        error.release(&error);
}

namespace
{

// DatabaseNew + uri (+ optional firebolt.database) + Init, keeping the database
// alive so the test can inspect what Init made of the URI.
struct InitedDatabase
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcError error = ADBC_ERROR_INIT;
    AdbcStatusCode code = ADBC_STATUS_OK;

    explicit InitedDatabase(const char * uri, const char * database_option = nullptr)
    {
        EXPECT_EQ(driver.DatabaseNew(&db, &error), ADBC_STATUS_OK);
        EXPECT_EQ(driver.DatabaseSetOption(&db, "uri", uri, &error), ADBC_STATUS_OK);
        if (database_option)
            EXPECT_EQ(driver.DatabaseSetOption(&db, "firebolt.database", database_option, &error), ADBC_STATUS_OK);
        code = driver.DatabaseInit(&db, &error);
    }
    ~InitedDatabase()
    {
        driver.DatabaseRelease(&db, nullptr);
        if (error.release)
            error.release(&error);
    }
    firebolt::adbc::FireboltDatabase & fdb() const { return *static_cast<firebolt::adbc::FireboltDatabase *>(db.private_data); }
};

} // namespace

TEST(DatabaseInitTest, FireboltUriResolvedToHttpEndpoint)
{
    InitedDatabase d("firebolt://localhost:3473/analytics?ssl_mode=disable");
    ASSERT_EQ(d.code, ADBC_STATUS_OK) << (d.error.message ? d.error.message : "");
    EXPECT_EQ(d.fdb().url, "http://localhost:3473");
    EXPECT_EQ(d.fdb().database, "analytics");
}

TEST(DatabaseInitTest, DatabaseOptionTakesPrecedenceOverUriPath)
{
    InitedDatabase d("firebolt://localhost:3473/from_uri?ssl_mode=disable", "from_option");
    ASSERT_EQ(d.code, ADBC_STATUS_OK) << (d.error.message ? d.error.message : "");
    EXPECT_EQ(d.fdb().database, "from_option");
}

TEST(DatabaseInitTest, MalformedFireboltUriRejected)
{
    InitedDatabase d("firebolt://localhost/db?ssl_mode=sometimes");
    EXPECT_EQ(d.code, ADBC_STATUS_INVALID_ARGUMENT);
}

TEST(DatabaseInitTest, FailedInitDoesNotKeepFireboltUriDatabase)
{
    // A failed Init leaves the database uninitialised, so the caller may fix
    // the options and retry.  The first URI's database must not survive into
    // the retry as if it had been set explicitly.
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.DatabaseNew(&db, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "uri", "firebolt://localhost:3473/db1", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "firebolt.ssl_certificate_path", "/no/such/ca.pem", &error), ADBC_STATUS_OK);
    ASSERT_NE(driver.DatabaseInit(&db, &error), ADBC_STATUS_OK);
    if (error.release)
        error.release(&error);

    ASSERT_EQ(driver.DatabaseSetOption(&db, "uri", "firebolt://localhost:3473/db2?ssl_mode=disable", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "firebolt.ssl_certificate_path", "", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseInit(&db, &error), ADBC_STATUS_OK) << (error.message ? error.message : "");

    const auto & fdb = *static_cast<firebolt::adbc::FireboltDatabase *>(db.private_data);
    EXPECT_EQ(fdb.url, "http://localhost:3473");
    EXPECT_EQ(fdb.database, "db2");
    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

TEST(DatabaseInitTest, FireboltUriDefaultsToTls)
{
    // ssl_mode defaults to verify-full.  On a build without TLS that cannot
    // work, and the error should point at the way to ask for plaintext.
    InitedDatabase d("firebolt://localhost:3473/db");
    if (CurlHasTls())
    {
        ASSERT_EQ(d.code, ADBC_STATUS_OK);
        EXPECT_EQ(d.fdb().url, "https://localhost:3473");
    }
    else
    {
        EXPECT_EQ(d.code, ADBC_STATUS_INVALID_ARGUMENT);
        ASSERT_NE(d.error.message, nullptr);
        EXPECT_NE(std::string(d.error.message).find("ssl_mode=disable"), std::string::npos)
            << "error should say how to connect without TLS, got: " << d.error.message;
    }
}

// ============================================================
// Tests: connection lifecycle
// ============================================================

static void SetupDatabase(AdbcDriver & driver, AdbcDatabase & db)
{
    AdbcError error = ADBC_ERROR_INIT;
    driver.DatabaseNew(&db, &error);
    driver.DatabaseSetOption(&db, "uri", "http://localhost:9123", &error);
    driver.DatabaseSetOption(&db, "firebolt.token", "testtoken", &error);
    driver.DatabaseInit(&db, &error);
    if (error.release)
        error.release(&error);
}

TEST(ConnectionTest, NewInitRelease)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    SetupDatabase(driver, db);

    AdbcConnection conn{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.ConnectionNew(&conn, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionInit(&conn, &db, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionRelease(&conn, &error), ADBC_STATUS_OK);
    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

TEST(ConnectionTest, SetOptionAutocommitIgnored)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    SetupDatabase(driver, db);

    AdbcConnection conn{};
    AdbcError error = ADBC_ERROR_INIT;
    driver.ConnectionNew(&conn, &error);
    driver.ConnectionInit(&conn, &db, &error);
    EXPECT_EQ(driver.ConnectionSetOption(&conn, ADBC_CONNECTION_OPTION_AUTOCOMMIT, ADBC_OPTION_VALUE_ENABLED, &error), ADBC_STATUS_OK);
    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

// Two connections created from the same AdbcDatabase must be able to carry
// independent bearer tokens.
TEST(ConnectionTest, PerConnectionTokensAreIndependent)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    SetupDatabase(driver, db);

    AdbcConnection conn_a{};
    AdbcConnection conn_b{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.ConnectionNew(&conn_a, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionInit(&conn_a, &db, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionNew(&conn_b, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionInit(&conn_b, &db, &error), ADBC_STATUS_OK);

    ASSERT_EQ(driver.ConnectionSetOption(&conn_a, "firebolt.token", "tokenA", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionSetOption(&conn_b, "firebolt.token", "tokenB", &error), ADBC_STATUS_OK);

    auto * fc_a = static_cast<firebolt::adbc::FireboltConnection *>(conn_a.private_data);
    auto * fc_b = static_cast<firebolt::adbc::FireboltConnection *>(conn_b.private_data);
    ASSERT_NE(fc_a, nullptr);
    ASSERT_NE(fc_b, nullptr);
    EXPECT_EQ(fc_a->token, "tokenA");
    EXPECT_EQ(fc_b->token, "tokenB") << "second connection's token did not land on its own FireboltConnection";
    EXPECT_NE(fc_a->token, fc_b->token) << "tokens collided — connections share a token store";

    driver.ConnectionRelease(&conn_a, nullptr);
    driver.ConnectionRelease(&conn_b, nullptr);
    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

// Setting a token on connection B must not retroactively change the token
// already set on connection A (the actual BOLA exploit primitive).
TEST(ConnectionTest, SecondConnectionDoesNotOverrideFirst)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    SetupDatabase(driver, db);

    AdbcConnection conn_a{};
    AdbcConnection conn_b{};
    AdbcError error = ADBC_ERROR_INIT;
    driver.ConnectionNew(&conn_a, &error);
    driver.ConnectionInit(&conn_a, &db, &error);
    driver.ConnectionNew(&conn_b, &error);
    driver.ConnectionInit(&conn_b, &db, &error);

    ASSERT_EQ(driver.ConnectionSetOption(&conn_a, "firebolt.token", "tokenA", &error), ADBC_STATUS_OK);
    auto * fc_a = static_cast<firebolt::adbc::FireboltConnection *>(conn_a.private_data);
    ASSERT_EQ(fc_a->token, "tokenA");

    ASSERT_EQ(driver.ConnectionSetOption(&conn_b, "firebolt.token", "tokenB", &error), ADBC_STATUS_OK);
    EXPECT_EQ(fc_a->token, "tokenA") << "connection A's token was clobbered when connection B set its token";

    driver.ConnectionRelease(&conn_a, nullptr);
    driver.ConnectionRelease(&conn_b, nullptr);
    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

// A token set via ConnectionSetOption *before* ConnectionInit must survive
// initialisation.  The ADBC C API permits options to be set on a Connection
// after New and before Init; an earlier version of ConnectionInit
// unconditionally clobbered fc->token with fdb->token, silently dropping
// the user-supplied per-connection identity and falling back to the
// database-default token.  fc->token is now the single source of truth —
// HttpClient reads it live via a const reference held on the connection.
TEST(ConnectionTest, TokenSetBeforeInitIsPreserved)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    SetupDatabase(driver, db); // db default token = "testtoken"

    AdbcConnection conn{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.ConnectionNew(&conn, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionSetOption(&conn, "firebolt.token", "preInitToken", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionInit(&conn, &db, &error), ADBC_STATUS_OK);

    auto * fc = static_cast<firebolt::adbc::FireboltConnection *>(conn.private_data);
    ASSERT_NE(fc, nullptr);
    EXPECT_EQ(fc->token, "preInitToken") << "pre-Init ConnectionSetOption('firebolt.token') was clobbered by fdb->token in Init";

    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

// The bearer token must never end up in session_params, because session_params
// are URL-encoded and appended to the query URL on every request — leaking the
// JWT into proxy/server access logs.  ConnectionSetOption used to fall through
// from the token branch into the catch-all session_params write.
TEST(ConnectionTest, TokenNotStoredInSessionParams)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    SetupDatabase(driver, db);

    AdbcConnection conn{};
    AdbcError error = ADBC_ERROR_INIT;
    driver.ConnectionNew(&conn, &error);
    driver.ConnectionInit(&conn, &db, &error);
    ASSERT_EQ(driver.ConnectionSetOption(&conn, "firebolt.token", "secret_jwt", &error), ADBC_STATUS_OK);

    auto * fc = static_cast<firebolt::adbc::FireboltConnection *>(conn.private_data);
    ASSERT_NE(fc, nullptr);
    EXPECT_EQ(fc->session_params.count("firebolt.token"), 0u) << "token leaked into session_params; will be appended to query URL";

    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

TEST(ConnectionTest, SessionOptionRequiresNamespaceAndStripsIt)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    SetupDatabase(driver, db);

    AdbcConnection conn{};
    AdbcError error = ADBC_ERROR_INIT;
    driver.ConnectionNew(&conn, &error);
    driver.ConnectionInit(&conn, &db, &error);

    ASSERT_EQ(driver.ConnectionSetOption(&conn, "firebolt.session.query_parameters", "[]", &error), ADBC_STATUS_OK);
    auto * fc = static_cast<firebolt::adbc::FireboltConnection *>(conn.private_data);
    ASSERT_NE(fc, nullptr);
    EXPECT_EQ(fc->session_params.at("query_parameters"), "[]");
    EXPECT_EQ(fc->session_params.count("firebolt.session.query_parameters"), 0u);

    EXPECT_EQ(driver.ConnectionSetOption(&conn, "query_parameters", "[]", &error), ADBC_STATUS_NOT_IMPLEMENTED);
    if (error.release)
        error.release(&error);

    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
}

// ============================================================
// Tests: statement lifecycle
// ============================================================

static void SetupConnection(AdbcDriver & driver, AdbcDatabase & db, AdbcConnection & conn)
{
    SetupDatabase(driver, db);
    AdbcError error = ADBC_ERROR_INIT;
    driver.ConnectionNew(&conn, &error);
    driver.ConnectionInit(&conn, &db, &error);
    if (error.release)
        error.release(&error);
}

TEST(StatementTest, NewSetQueryRelease)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcConnection conn{};
    SetupConnection(driver, db, conn);

    AdbcStatement stmt{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.StatementNew(&conn, &stmt, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementSetSqlQuery(&stmt, "SELECT 1", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementPrepare(&stmt, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementRelease(&stmt, &error), ADBC_STATUS_OK);

    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

TEST(StatementTest, NullQueryFails)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcConnection conn{};
    SetupConnection(driver, db, conn);

    AdbcStatement stmt{};
    AdbcError error = ADBC_ERROR_INIT;
    driver.StatementNew(&conn, &stmt, &error);
    EXPECT_EQ(driver.StatementSetSqlQuery(&stmt, nullptr, &error), ADBC_STATUS_INVALID_ARGUMENT);

    driver.StatementRelease(&stmt, nullptr);
    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

// ============================================================
// Tests: ArrowIpcStream — empty body produces empty stream
// ============================================================

TEST(ArrowIpcStreamTest, EmptyBodyProducesEmptyStream)
{
    ArrowArrayStream stream{};
    std::string err = firebolt::adbc::ExportIpcBytesAsArrowStream({}, &stream);
    ASSERT_TRUE(err.empty()) << err;
    ASSERT_NE(stream.get_schema, nullptr);

    ArrowSchema schema{};
    EXPECT_EQ(stream.get_schema(&stream, &schema), 0);
    if (schema.release)
        schema.release(&schema);

    ArrowArray array{};
    EXPECT_EQ(stream.get_next(&stream, &array), 0);
    EXPECT_EQ(array.release, nullptr); // end-of-stream

    if (stream.release)
        stream.release(&stream);
}

// ============================================================
// Tests: ArrowIpcStream — round-trip IPC bytes via nanoarrow
// ============================================================

TEST(ArrowIpcStreamTest, RoundTripIpcBytes)
{
    // Build a simple int32 record batch using nanoarrow
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ASSERT_EQ(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT32), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "x"), 0);

    nanoarrow::UniqueArray batch;
    ASSERT_EQ(ArrowArrayInitFromSchema(batch.get(), schema.get(), nullptr), 0);
    ASSERT_EQ(ArrowArrayStartAppending(batch.get()), 0);
    for (int i = 0; i < 3; ++i)
    {
        ASSERT_EQ(ArrowArrayAppendInt(batch->children[0], i + 1), 0);
        ASSERT_EQ(ArrowArrayFinishElement(batch.get()), 0);
    }
    ASSERT_EQ(ArrowArrayFinishBuilding(batch.get(), NANOARROW_VALIDATION_LEVEL_DEFAULT, nullptr), 0);

    // Serialise to IPC stream bytes using nanoarrow.
    // ArrowBasicArrayStreamInit takes ownership of schema; SetArray takes ownership of batch.
    nanoarrow::UniqueArrayStream basic_stream;
    ASSERT_EQ(ArrowBasicArrayStreamInit(basic_stream.get(), schema.get(), 1), 0);
    ArrowBasicArrayStreamSetArray(basic_stream.get(), 0, batch.get());

    ArrowBuffer buf{};
    ArrowBufferInit(&buf);
    ArrowIpcOutputStream ipc_out_stream{};
    ASSERT_EQ(ArrowIpcOutputStreamInitBuffer(&ipc_out_stream, &buf), 0);
    ArrowIpcWriter writer{};
    ASSERT_EQ(ArrowIpcWriterInit(&writer, &ipc_out_stream), 0);
    ArrowError na_error{};
    ASSERT_EQ(ArrowIpcWriterWriteArrayStream(&writer, basic_stream.get(), &na_error), 0) << na_error.message;
    ArrowIpcWriterReset(&writer);

    std::vector<uint8_t> ipc_bytes(buf.data, buf.data + buf.size_bytes);
    ArrowBufferReset(&buf);

    // Round-trip through ExportIpcBytesAsArrowStream
    ArrowArrayStream out_stream{};
    std::string out_err = firebolt::adbc::ExportIpcBytesAsArrowStream(ipc_bytes, &out_stream);
    ASSERT_TRUE(out_err.empty()) << out_err;

    ArrowSchema out_schema{};
    ASSERT_EQ(out_stream.get_schema(&out_stream, &out_schema), 0);
    EXPECT_EQ(out_schema.n_children, 1);
    EXPECT_STREQ(out_schema.children[0]->name, "x");
    if (out_schema.release)
        out_schema.release(&out_schema);

    ArrowArray out_batch{};
    ASSERT_EQ(out_stream.get_next(&out_stream, &out_batch), 0);
    ASSERT_NE(out_batch.release, nullptr);
    EXPECT_EQ(out_batch.length, 3);
    if (out_batch.release)
        out_batch.release(&out_batch);

    ArrowArray eos{};
    ASSERT_EQ(out_stream.get_next(&out_stream, &eos), 0);
    EXPECT_EQ(eos.release, nullptr);

    if (out_stream.release)
        out_stream.release(&out_stream);
}

// ============================================================
// Tests: IngestSqlBuilder — quoteIdentifier / qualifiedTable
// ============================================================

TEST(IngestSqlBuilderTest, QuoteIdentifierBasic)
{
    EXPECT_EQ(firebolt::adbc::quoteIdentifier("users"), "\"users\"");
}

TEST(IngestSqlBuilderTest, QuoteIdentifierEscapesEmbeddedQuotes)
{
    // Inner double-quotes must be doubled (standard SQL identifier quoting).
    EXPECT_EQ(firebolt::adbc::quoteIdentifier("we\"ird"), "\"we\"\"ird\"");
}

TEST(IngestSqlBuilderTest, QualifiedTableTableOnly)
{
    EXPECT_EQ(firebolt::adbc::qualifiedTable("", "", "events"), "\"events\"");
}

TEST(IngestSqlBuilderTest, QualifiedTableSchemaPrefixed)
{
    EXPECT_EQ(firebolt::adbc::qualifiedTable("", "public", "events"), "\"public\".\"events\"");
}

TEST(IngestSqlBuilderTest, QualifiedTableFullyQualified)
{
    EXPECT_EQ(firebolt::adbc::qualifiedTable("warehouse", "public", "events"), "\"warehouse\".\"public\".\"events\"");
}

// ============================================================
// Tests: GetTableSchema probe SQL — identifier quoting must double
// embedded `"` so caller-supplied names cannot inject SQL.
// ============================================================

TEST(GetTableSchemaSqlTest, NoSchemaProducesSinglePart)
{
    EXPECT_EQ(firebolt::adbc::buildTableSchemaSql("", "", "users"), "SELECT * FROM \"users\" LIMIT 0");
}

TEST(GetTableSchemaSqlTest, WithSchemaProducesTwoPart)
{
    EXPECT_EQ(firebolt::adbc::buildTableSchemaSql("", "public", "users"), "SELECT * FROM \"public\".\"users\" LIMIT 0");
}

TEST(GetTableSchemaSqlTest, WithCatalogProducesThreePart)
{
    EXPECT_EQ(
        firebolt::adbc::buildTableSchemaSql("warehouse", "public", "users"), "SELECT * FROM \"warehouse\".\"public\".\"users\" LIMIT 0");
}

TEST(GetTableSchemaSqlTest, QuotesEmbeddedDoubleQuoteInTable)
{
    // Adversarial table name that would otherwise close the identifier and
    // inject SQL.  Embedded `"` must be doubled.
    EXPECT_EQ(firebolt::adbc::buildTableSchemaSql("", "", "users\"x"), "SELECT * FROM \"users\"\"x\" LIMIT 0");
}

TEST(GetTableSchemaSqlTest, QuotesEmbeddedDoubleQuoteInSchema)
{
    EXPECT_EQ(firebolt::adbc::buildTableSchemaSql("", "pu\"blic", "users"), "SELECT * FROM \"pu\"\"blic\".\"users\" LIMIT 0");
}

TEST(GetTableSchemaSqlTest, RejectsSqlInjectionAttempt)
{
    // Classic SQLi payload: close the identifier, inject DDL, comment out the rest.
    // After fix the payload is safely contained inside one quoted identifier.
    const std::string payload = "x\"; DROP TABLE secrets; --";
    std::string sql = firebolt::adbc::buildTableSchemaSql("", "", payload);
    // Embedded `"` is doubled, so the only `"` characters surround the identifier
    // exactly twice (once at start, once at end of the doubled-up identifier).
    EXPECT_EQ(sql, "SELECT * FROM \"x\"\"; DROP TABLE secrets; --\" LIMIT 0");
    // Sanity: no semicolon outside the quoted identifier.
    auto last_quote = sql.find_last_of('"');
    auto trailing = sql.substr(last_quote);
    EXPECT_EQ(trailing.find(';'), std::string::npos) << "trailing=" << trailing;
}

// ============================================================
// Tests: IngestSqlBuilder — Arrow → Firebolt SQL type mapping
// ============================================================

TEST(ArrowToFireboltTypeTest, BoolMapsToBoolean)
{
    auto schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_BOOL, "x");
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "BOOLEAN");
}

TEST(ArrowToFireboltTypeTest, Int32MapsToInt)
{
    auto schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_INT32, "x");
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "INT");
}

TEST(ArrowToFireboltTypeTest, Int64MapsToBigint)
{
    auto schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_INT64, "x");
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "BIGINT");
}

TEST(ArrowToFireboltTypeTest, FloatMapsToReal)
{
    auto schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_FLOAT, "x");
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "REAL");
}

TEST(ArrowToFireboltTypeTest, DoubleMapsToDoublePrecision)
{
    auto schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_DOUBLE, "x");
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "DOUBLE PRECISION");
}

TEST(ArrowToFireboltTypeTest, StringMapsToText)
{
    auto schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_STRING, "x");
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "TEXT");
}

TEST(ArrowToFireboltTypeTest, LargeStringMapsToText)
{
    auto schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_LARGE_STRING, "x");
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "TEXT");
}

TEST(ArrowToFireboltTypeTest, Date32MapsToDate)
{
    auto schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_DATE32, "x");
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "DATE");
}

TEST(ArrowToFireboltTypeTest, TimestampNoTimezoneMapsToTimestampNtz)
{
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ASSERT_EQ(ArrowSchemaSetTypeDateTime(schema->children[0], NANOARROW_TYPE_TIMESTAMP, NANOARROW_TIME_UNIT_MICRO, nullptr), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "x"), 0);
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "TIMESTAMPNTZ");
}

TEST(ArrowToFireboltTypeTest, TimestampWithTimezoneMapsToTimestampTz)
{
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ASSERT_EQ(ArrowSchemaSetTypeDateTime(schema->children[0], NANOARROW_TYPE_TIMESTAMP, NANOARROW_TIME_UNIT_MICRO, "UTC"), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "x"), 0);
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "TIMESTAMPTZ");
}

TEST(ArrowToFireboltTypeTest, Decimal128PreservesPrecisionAndScale)
{
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ASSERT_EQ(ArrowSchemaSetTypeDecimal(schema->children[0], NANOARROW_TYPE_DECIMAL128, 18, 4), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "x"), 0);
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "DECIMAL(18, 4)");
}

TEST(ArrowToFireboltTypeTest, ListOfIntMapsToArrayInt)
{
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ASSERT_EQ(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_LIST), 0);
    ASSERT_EQ(ArrowSchemaSetType(schema->children[0]->children[0], NANOARROW_TYPE_INT32), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "vals"), 0);
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "ARRAY(INT)");
}

TEST(ArrowToFireboltTypeTest, StructMapsToFireboltStruct)
{
    // STRUCT<id:int32 not null, name:string>.  `id` is non-nullable on purpose:
    // Firebolt rejects a non-nullable STRUCT field ("STRUCT fields have to be
    // nullable"), so the field nullability has to be dropped here — otherwise
    // create-mode ingest emits DDL the server refuses.  Column-level nullability
    // is rendered by buildCreateTableColumns instead.
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ASSERT_EQ(ArrowSchemaSetTypeStruct(schema->children[0], 2), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "person"), 0);

    ASSERT_EQ(ArrowSchemaSetType(schema->children[0]->children[0], NANOARROW_TYPE_INT32), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0]->children[0], "id"), 0);
    schema->children[0]->children[0]->flags &= ~ARROW_FLAG_NULLABLE;

    ASSERT_EQ(ArrowSchemaSetType(schema->children[0]->children[1], NANOARROW_TYPE_STRING), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0]->children[1], "name"), 0);

    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "STRUCT(\"id\" INT, \"name\" TEXT)");
}

TEST(ArrowToFireboltTypeTest, NestedStructMapsToNestedFireboltStruct)
{
    // STRUCT<x:int32, child:STRUCT<y:int64, z:string>>
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ArrowSchema * outer = schema->children[0];
    ASSERT_EQ(ArrowSchemaSetTypeStruct(outer, 2), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer, "s"), 0);

    ASSERT_EQ(ArrowSchemaSetType(outer->children[0], NANOARROW_TYPE_INT32), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer->children[0], "x"), 0);

    ASSERT_EQ(ArrowSchemaSetTypeStruct(outer->children[1], 2), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer->children[1], "child"), 0);
    ASSERT_EQ(ArrowSchemaSetType(outer->children[1]->children[0], NANOARROW_TYPE_INT64), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer->children[1]->children[0], "y"), 0);
    ASSERT_EQ(ArrowSchemaSetType(outer->children[1]->children[1], NANOARROW_TYPE_STRING), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer->children[1]->children[1], "z"), 0);

    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(outer), "STRUCT(\"x\" INT, \"child\" STRUCT(\"y\" BIGINT, \"z\" TEXT))");
}

TEST(ArrowToFireboltTypeTest, ListOfStructMapsToArrayOfStruct)
{
    // LIST<STRUCT<k:int32, v:string>>
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ArrowSchema * list = schema->children[0];
    ASSERT_EQ(ArrowSchemaSetType(list, NANOARROW_TYPE_LIST), 0);
    ASSERT_EQ(ArrowSchemaSetName(list, "items"), 0);
    ASSERT_EQ(ArrowSchemaSetTypeStruct(list->children[0], 2), 0);
    ASSERT_EQ(ArrowSchemaSetType(list->children[0]->children[0], NANOARROW_TYPE_INT32), 0);
    ASSERT_EQ(ArrowSchemaSetName(list->children[0]->children[0], "k"), 0);
    ASSERT_EQ(ArrowSchemaSetType(list->children[0]->children[1], NANOARROW_TYPE_STRING), 0);
    ASSERT_EQ(ArrowSchemaSetName(list->children[0]->children[1], "v"), 0);

    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(list), "ARRAY(STRUCT(\"k\" INT, \"v\" TEXT))");
}

TEST(ArrowToFireboltTypeTest, StructContainingListMapsToStructWithArray)
{
    // STRUCT<xs:LIST<int32>>
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ArrowSchema * outer = schema->children[0];
    ASSERT_EQ(ArrowSchemaSetTypeStruct(outer, 1), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer, "s"), 0);
    ASSERT_EQ(ArrowSchemaSetType(outer->children[0], NANOARROW_TYPE_LIST), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer->children[0], "xs"), 0);
    ASSERT_EQ(ArrowSchemaSetType(outer->children[0]->children[0], NANOARROW_TYPE_INT32), 0);

    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(outer), "STRUCT(\"xs\" ARRAY(INT))");
}

TEST(ArrowToFireboltTypeTest, NestedListOfStructMapsToNestedArrayOfStruct)
{
    // LIST<LIST<STRUCT<m:int32>>> — every list level has to be descended.
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ArrowSchema * outer = schema->children[0];
    ASSERT_EQ(ArrowSchemaSetType(outer, NANOARROW_TYPE_LIST), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer, "aas"), 0); // codespell:ignore
    ASSERT_EQ(ArrowSchemaSetType(outer->children[0], NANOARROW_TYPE_LARGE_LIST), 0);
    ASSERT_EQ(ArrowSchemaSetTypeStruct(outer->children[0]->children[0], 1), 0);
    ASSERT_EQ(ArrowSchemaSetType(outer->children[0]->children[0]->children[0], NANOARROW_TYPE_INT32), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer->children[0]->children[0]->children[0], "m"), 0);

    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(outer), "ARRAY(ARRAY(STRUCT(\"m\" INT)))");
}

TEST(ArrowToFireboltTypeTest, StructFieldNamesAreQuotedAndEscaped)
{
    // A reserved keyword and an embedded double quote both have to survive
    // quoting, or the generated DDL is either rejected or injectable.
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ArrowSchema * outer = schema->children[0];
    ASSERT_EQ(ArrowSchemaSetTypeStruct(outer, 2), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer, "s"), 0);
    ASSERT_EQ(ArrowSchemaSetType(outer->children[0], NANOARROW_TYPE_INT32), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer->children[0], "order"), 0);
    ASSERT_EQ(ArrowSchemaSetType(outer->children[1], NANOARROW_TYPE_STRING), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer->children[1], "a\"b"), 0);

    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(outer), "STRUCT(\"order\" INT, \"a\"\"b\" TEXT)");
}

TEST(ArrowToFireboltTypeTest, EmptyStructReturnsEmpty)
{
    // Firebolt has no zero-field STRUCT; rendering "STRUCT()" would produce a
    // syntax error at the server. The mapping must fail up front instead, so
    // buildIngestSql reports ADBC_STATUS_NOT_IMPLEMENTED.
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ASSERT_EQ(ArrowSchemaSetTypeStruct(schema->children[0], 0), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "s"), 0);
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "");
}

TEST(ArrowToFireboltTypeTest, StructWithUnsupportedFieldReturnsEmpty)
{
    // A field type with no Firebolt equivalent (here MAP, which read_arrow() also
    // rejects) has to fail the whole struct rather than be skipped.
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ArrowSchema * outer = schema->children[0];
    ASSERT_EQ(ArrowSchemaSetTypeStruct(outer, 2), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer, "s"), 0);
    ASSERT_EQ(ArrowSchemaSetType(outer->children[0], NANOARROW_TYPE_INT32), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer->children[0], "ok"), 0);
    // MAP allocates an "entries" struct child; its key/value still need types.
    ASSERT_EQ(ArrowSchemaSetType(outer->children[1], NANOARROW_TYPE_MAP), 0);
    ASSERT_EQ(ArrowSchemaSetType(outer->children[1]->children[0]->children[0], NANOARROW_TYPE_STRING), 0);
    ASSERT_EQ(ArrowSchemaSetType(outer->children[1]->children[0]->children[1], NANOARROW_TYPE_INT32), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer->children[1], "bad"), 0);

    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(outer), "");
}

TEST(ArrowToFireboltTypeTest, UnnamedStructFieldReturnsEmpty)
{
    // A nameless field would render as the empty identifier `""`; reject it the
    // same way buildCreateTableColumns rejects a nameless column.
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ArrowSchema * outer = schema->children[0];
    ASSERT_EQ(ArrowSchemaSetTypeStruct(outer, 1), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer, "s"), 0);
    ASSERT_EQ(ArrowSchemaSetType(outer->children[0], NANOARROW_TYPE_INT32), 0);
    ASSERT_EQ(ArrowSchemaSetName(outer->children[0], nullptr), 0);

    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(outer), "");
}

TEST(ArrowToFireboltTypeTest, UnsupportedTypeReturnsEmpty)
{
    auto schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_INTERVAL_DAY_TIME, "x");
    EXPECT_EQ(firebolt::adbc::arrowTypeToFireboltSqlType(schema->children[0]), "");
}

// ============================================================
// Tests: IngestSqlBuilder — buildCreateTableColumns
// ============================================================

TEST(BuildCreateTableColumnsTest, MultipleColumns)
{
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(3);

    ASSERT_EQ(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT32), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "id"), 0);
    schema->children[0]->flags &= ~ARROW_FLAG_NULLABLE; // NOT NULL

    ASSERT_EQ(ArrowSchemaSetType(schema->children[1], NANOARROW_TYPE_STRING), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[1], "label"), 0);

    ASSERT_EQ(ArrowSchemaSetType(schema->children[2], NANOARROW_TYPE_DOUBLE), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[2], "value"), 0);

    EXPECT_EQ(firebolt::adbc::buildCreateTableColumns(schema.get()), "\"id\" INT NOT NULL, \"label\" TEXT, \"value\" DOUBLE PRECISION");
}

TEST(BuildCreateTableColumnsTest, NotNullStructColumnKeepsColumnLevelNotNull)
{
    // NOT NULL is legal on the column but not on the struct's fields; the two
    // levels must not be conflated.
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ArrowSchema * col = schema->children[0];
    ASSERT_EQ(ArrowSchemaSetTypeStruct(col, 1), 0);
    ASSERT_EQ(ArrowSchemaSetName(col, "s"), 0);
    col->flags &= ~ARROW_FLAG_NULLABLE; // column: NOT NULL
    ASSERT_EQ(ArrowSchemaSetType(col->children[0], NANOARROW_TYPE_INT32), 0);
    ASSERT_EQ(ArrowSchemaSetName(col->children[0], "a"), 0);
    col->children[0]->flags &= ~ARROW_FLAG_NULLABLE; // field: must be dropped

    EXPECT_EQ(firebolt::adbc::buildCreateTableColumns(schema.get()), "\"s\" STRUCT(\"a\" INT) NOT NULL");
}

TEST(BuildCreateTableColumnsTest, EmptySchemaReturnsEmpty)
{
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(0);
    EXPECT_EQ(firebolt::adbc::buildCreateTableColumns(schema.get()), "");
}

TEST(BuildCreateTableColumnsTest, UnsupportedColumnTypeReturnsEmpty)
{
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ASSERT_EQ(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INTERVAL_DAY_TIME), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "x"), 0);
    EXPECT_EQ(firebolt::adbc::buildCreateTableColumns(schema.get()), "");
}

// ============================================================
// Tests: StatementSetOption — ingest options
// ============================================================

namespace
{

struct IngestOptionFixture
{
    AdbcDriver driver{};
    AdbcDatabase db{};
    AdbcConnection conn{};
    AdbcStatement stmt{};

    IngestOptionFixture()
    {
        driver = InitDriver();
        SetupConnection(driver, db, conn);
        AdbcError error = ADBC_ERROR_INIT;
        EXPECT_EQ(driver.StatementNew(&conn, &stmt, &error), ADBC_STATUS_OK);
        if (error.release)
            error.release(&error);
    }

    ~IngestOptionFixture()
    {
        driver.StatementRelease(&stmt, nullptr);
        driver.ConnectionRelease(&conn, nullptr);
        driver.DatabaseRelease(&db, nullptr);
    }
};

} // namespace

// ============================================================
// Tests: not-implemented stubs
// ============================================================

TEST(StatementSetOptionTest, TargetTableAccepted)
{
    IngestOptionFixture f;
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(f.driver.StatementSetOption(&f.stmt, ADBC_INGEST_OPTION_TARGET_TABLE, "events", &error), ADBC_STATUS_OK);
    if (error.release)
        error.release(&error);
}

TEST(StatementSetOptionTest, TargetCatalogAndSchemaAccepted)
{
    IngestOptionFixture f;
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(f.driver.StatementSetOption(&f.stmt, ADBC_INGEST_OPTION_TARGET_CATALOG, "warehouse", &error), ADBC_STATUS_OK);
    EXPECT_EQ(f.driver.StatementSetOption(&f.stmt, ADBC_INGEST_OPTION_TARGET_DB_SCHEMA, "public", &error), ADBC_STATUS_OK);
    if (error.release)
        error.release(&error);
}

TEST(StatementSetOptionTest, KnownIngestModesAccepted)
{
    IngestOptionFixture f;
    AdbcError error = ADBC_ERROR_INIT;
    for (const char * mode : {
             ADBC_INGEST_OPTION_MODE_APPEND,
             ADBC_INGEST_OPTION_MODE_CREATE,
             ADBC_INGEST_OPTION_MODE_REPLACE,
             ADBC_INGEST_OPTION_MODE_CREATE_APPEND,
         })
    {
        EXPECT_EQ(f.driver.StatementSetOption(&f.stmt, ADBC_INGEST_OPTION_MODE, mode, &error), ADBC_STATUS_OK) << "mode=" << mode;
    }
    if (error.release)
        error.release(&error);
}

// ADBC's default ingest mode is create ("Whether to create (the default) or append",
// adbc.h).  Only the low-level path can observe it — dbapi's Cursor.adbc_ingest()
// always sends a mode — and an append into a table nobody created is a "table does
// not exist" error.
TEST(StatementSetOptionTest, IngestModeDefaultsToCreate)
{
    IngestOptionFixture f;
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(f.driver.StatementSetOption(&f.stmt, ADBC_INGEST_OPTION_TARGET_TABLE, "events", &error), ADBC_STATUS_OK);

    auto * fs = static_cast<firebolt::adbc::FireboltStatement *>(f.stmt.private_data);
    ASSERT_NE(fs, nullptr);
    ASSERT_TRUE(fs->bound.has_value());
    EXPECT_EQ(fs->bound->mode, firebolt::adbc::IngestMode::Create);

    // And the default has to reach the generated SQL, not just the struct.
    nanoarrow::UniqueSchema schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_INT32, "x");
    fs->bound->schema.reset();
    ASSERT_EQ(ArrowSchemaDeepCopy(schema.get(), fs->bound->schema.get()), 0);

    std::vector<std::string> pre_sql;
    std::string insert_sql;
    ASSERT_EQ(firebolt::adbc::buildIngestSql(fs, pre_sql, insert_sql, &error), ADBC_STATUS_OK);
    ASSERT_EQ(pre_sql.size(), 1u) << "the default mode generated no CREATE TABLE";
    EXPECT_EQ(pre_sql[0], "CREATE TABLE \"events\" (\"x\" INT)");

    if (error.release)
        error.release(&error);
}

TEST(StatementSetOptionTest, UnknownIngestModeRejected)
{
    IngestOptionFixture f;
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(
        f.driver.StatementSetOption(&f.stmt, ADBC_INGEST_OPTION_MODE, "adbc.ingest.mode.bogus", &error), ADBC_STATUS_INVALID_ARGUMENT);
    if (error.release)
        error.release(&error);
}

TEST(StatementSetOptionTest, TemporaryFalseSilentlyAccepted)
{
    // Firebolt has no notion of session-temporary tables.  temporary=false is a
    // no-op that dbapi sends by default; it must not cause adbc_ingest() to fail.
    IngestOptionFixture f;
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(f.driver.StatementSetOption(&f.stmt, ADBC_INGEST_OPTION_TEMPORARY, ADBC_OPTION_VALUE_DISABLED, &error), ADBC_STATUS_OK);
    if (error.release)
        error.release(&error);
}

TEST(StatementSetOptionTest, TemporaryTrueRejectedWithNotImplemented)
{
    // dbapi catches NotSupportedError to fall through; surface it explicitly.
    IngestOptionFixture f;
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(
        f.driver.StatementSetOption(&f.stmt, ADBC_INGEST_OPTION_TEMPORARY, ADBC_OPTION_VALUE_ENABLED, &error), ADBC_STATUS_NOT_IMPLEMENTED);
    if (error.release)
        error.release(&error);
}

TEST(StatementSetOptionTest, NullKeyRejected)
{
    IngestOptionFixture f;
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(f.driver.StatementSetOption(&f.stmt, nullptr, "x", &error), ADBC_STATUS_INVALID_ARGUMENT);
    if (error.release)
        error.release(&error);
}

// Misreading the value would silently send the wrong parameter names, so only the
// two canonical spellings are accepted — the rule the autocommit option follows.
TEST(StatementSetOptionTest, BindByNameAcceptsOnlyCanonicalValues)
{
    IngestOptionFixture f;
    AdbcError error = ADBC_ERROR_INIT;
    auto * fs = static_cast<firebolt::adbc::FireboltStatement *>(f.stmt.private_data);
    ASSERT_NE(fs, nullptr);

    ASSERT_EQ(f.driver.StatementSetOption(&f.stmt, "adbc.statement.bind_by_name", ADBC_OPTION_VALUE_ENABLED, &error), ADBC_STATUS_OK);
    EXPECT_TRUE(fs->bind_by_name);

    ASSERT_EQ(f.driver.StatementSetOption(&f.stmt, "adbc.statement.bind_by_name", ADBC_OPTION_VALUE_DISABLED, &error), ADBC_STATUS_OK);
    EXPECT_FALSE(fs->bind_by_name);

    const std::vector<const char *> bad_values = {"TRUE", "1", "yes", ""};
    for (const char * value : bad_values)
    {
        EXPECT_EQ(f.driver.StatementSetOption(&f.stmt, "adbc.statement.bind_by_name", value, &error), ADBC_STATUS_INVALID_ARGUMENT)
            << "value: " << value;
        if (error.release)
            error.release(&error);
    }
    EXPECT_FALSE(fs->bind_by_name) << "a rejected value must not change the setting";

    if (error.release)
        error.release(&error);
}

// A statement option, so it must outlive the payload it applies to and survive a new
// query.  A driver manager re-sends it only when its own idea of the setting changes
// (dbapi tracks it per cursor), so resetting it here would silently revert to
// positional naming and leave the server reporting parameters "not set in the query".
TEST(StatementSetOptionTest, BindByNameSurvivesExecuteAndSetSqlQuery)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.DatabaseNew(&db, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "uri", "http://127.0.0.1:1", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseInit(&db, &error), ADBC_STATUS_OK);

    AdbcConnection conn{};
    ASSERT_EQ(driver.ConnectionNew(&conn, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionInit(&conn, &db, &error), ADBC_STATUS_OK);

    AdbcStatement stmt{};
    ASSERT_EQ(driver.StatementNew(&conn, &stmt, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementSetOption(&stmt, "adbc.statement.bind_by_name", ADBC_OPTION_VALUE_ENABLED, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementSetSqlQuery(&stmt, "SELECT param('who')", &error), ADBC_STATUS_OK);

    auto * fs = static_cast<firebolt::adbc::FireboltStatement *>(stmt.private_data);
    ASSERT_NE(fs, nullptr);
    EXPECT_TRUE(fs->bind_by_name) << "SetSqlQuery cleared a statement option";

    nanoarrow::UniqueSchema schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_INT64, "who");
    nanoarrow::UniqueArray batch = MakeOneColumnBatch(schema.get(), {1});
    ASSERT_EQ(driver.StatementBind(&stmt, batch.get(), schema.get(), &error), ADBC_STATUS_OK);
    // Unreachable endpoint: the execution fails at the HTTP layer, having consumed
    // the bound payload on the way out.
    EXPECT_EQ(driver.StatementExecuteQuery(&stmt, nullptr, nullptr, &error), ADBC_STATUS_IO);
    if (error.release)
        error.release(&error);
    EXPECT_FALSE(fs->bound.has_value());
    EXPECT_TRUE(fs->bind_by_name) << "the option died with the payload it applied to";

    driver.StatementRelease(&stmt, nullptr);
    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

// Ingest atomicity: in a mode that generates DDL (Replace / Create / CreateAppend)
// with nothing bound, ExecuteQuery must fail before any pre-SQL runs — Replace would
// otherwise DROP the table with no INSERT able to land.  The state below is a
// BindStream whose schema capture succeeded and whose bind did not.  Against the
// unreachable 127.0.0.1:1, a DROP attempt would show as IO instead of INVALID_STATE.
TEST(StatementExecuteTest, IngestReplaceWithoutBoundDataRejectedBeforeAnyHttp)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.DatabaseNew(&db, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "uri", "http://127.0.0.1:1", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseInit(&db, &error), ADBC_STATUS_OK);

    AdbcConnection conn{};
    ASSERT_EQ(driver.ConnectionNew(&conn, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionInit(&conn, &db, &error), ADBC_STATUS_OK);

    AdbcStatement stmt{};
    ASSERT_EQ(driver.StatementNew(&conn, &stmt, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementSetOption(&stmt, ADBC_INGEST_OPTION_TARGET_TABLE, "foo", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementSetOption(&stmt, ADBC_INGEST_OPTION_MODE, ADBC_INGEST_OPTION_MODE_REPLACE, &error), ADBC_STATUS_OK);

    auto * fs = static_cast<firebolt::adbc::FireboltStatement *>(stmt.private_data);
    ASSERT_NE(fs, nullptr);
    ASSERT_TRUE(fs->bound.has_value());

    // Synthesize the post-bind state: schema captured, no data bound.
    nanoarrow::UniqueSchema bound_schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_INT32, "x");
    fs->bound->schema.reset();
    ASSERT_EQ(ArrowSchemaDeepCopy(bound_schema.get(), fs->bound->schema.get()), 0);
    ASSERT_FALSE(fs->bound->data_bound);

    AdbcStatusCode rc = driver.StatementExecuteQuery(&stmt, nullptr, nullptr, &error);
    EXPECT_EQ(rc, ADBC_STATUS_INVALID_STATE) << "ingest with no bound data must short-circuit before any HTTP / pre-SQL";

    if (error.release)
        error.release(&error);
    driver.StatementRelease(&stmt, nullptr);
    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
}

// Changing the statement's SQL invalidates any bound payload, so data from a prior
// ingest cannot ride along on a subsequent SELECT.
TEST(StatementReuseTest, IngestStateClearedOnSetSqlQuery)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcConnection conn{};
    SetupConnection(driver, db, conn);

    AdbcStatement stmt{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.StatementNew(&conn, &stmt, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementSetOption(&stmt, ADBC_INGEST_OPTION_TARGET_TABLE, "foo", &error), ADBC_STATUS_OK);

    nanoarrow::UniqueSchema schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_INT64, "x");
    nanoarrow::UniqueArray batch = MakeOneColumnBatch(schema.get(), {41});
    ASSERT_EQ(driver.StatementBind(&stmt, batch.get(), schema.get(), &error), ADBC_STATUS_OK);

    auto * fs = static_cast<firebolt::adbc::FireboltStatement *>(stmt.private_data);
    ASSERT_NE(fs, nullptr);
    ASSERT_TRUE(fs->bound.has_value());
    ASSERT_TRUE(fs->bound->data_bound);

    ASSERT_EQ(driver.StatementSetSqlQuery(&stmt, "SELECT 1", &error), ADBC_STATUS_OK);

    EXPECT_FALSE(fs->bound.has_value()) << "stale bind data persisted across SetSqlQuery; would attach to next query";

    driver.StatementRelease(&stmt, nullptr);
    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

// The same on the failure path: a failed ExecuteQuery leaves no bound payload.
TEST(StatementReuseTest, IngestStateClearedOnFailedExecute)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.DatabaseNew(&db, &error), ADBC_STATUS_OK);
    // Unreachable port — any HTTP attempt will fail with curl error.
    ASSERT_EQ(driver.DatabaseSetOption(&db, "uri", "http://127.0.0.1:1", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseInit(&db, &error), ADBC_STATUS_OK);

    AdbcConnection conn{};
    ASSERT_EQ(driver.ConnectionNew(&conn, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionInit(&conn, &db, &error), ADBC_STATUS_OK);

    AdbcStatement stmt{};
    ASSERT_EQ(driver.StatementNew(&conn, &stmt, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementSetOption(&stmt, ADBC_INGEST_OPTION_TARGET_TABLE, "foo", &error), ADBC_STATUS_OK);
    // Append mode: no DDL pre-SQL, so nothing but the bound-data gate stands between
    // this and the HTTP attempt the unreachable port rejects.
    ASSERT_EQ(driver.StatementSetOption(&stmt, ADBC_INGEST_OPTION_MODE, ADBC_INGEST_OPTION_MODE_APPEND, &error), ADBC_STATUS_OK);

    nanoarrow::UniqueSchema schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_INT64, "x");
    nanoarrow::UniqueArray batch = MakeOneColumnBatch(schema.get(), {41});
    ASSERT_EQ(driver.StatementBind(&stmt, batch.get(), schema.get(), &error), ADBC_STATUS_OK);

    auto * fs = static_cast<firebolt::adbc::FireboltStatement *>(stmt.private_data);
    ASSERT_NE(fs, nullptr);
    ASSERT_TRUE(fs->bound.has_value());

    AdbcStatusCode rc = driver.StatementExecuteQuery(&stmt, nullptr, nullptr, &error);
    EXPECT_EQ(rc, ADBC_STATUS_IO) << "expected HTTP layer to reject the unreachable target";

    EXPECT_FALSE(fs->bound.has_value()) << "ingest state survived a failed ExecuteQuery; bound data would attach to next query";

    if (error.release)
        error.release(&error);
    driver.StatementRelease(&stmt, nullptr);
    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
}

// Bound data with no ingest target — what `cursor.execute(sql, params)` produces —
// is a set of query parameters.  The multipart-insert branch keys on the ingest
// target, not on the presence of a payload, so a SELECT picks up no data.arrow part.
TEST(StatementExecuteTest, BindWithoutIngestTargetTakesParameterPath)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.DatabaseNew(&db, &error), ADBC_STATUS_OK);
    // Unreachable port: reaching the HTTP layer shows as IO, which is how this
    // test tells "sent as a parameterised query" from "refused up front".
    ASSERT_EQ(driver.DatabaseSetOption(&db, "uri", "http://127.0.0.1:1", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseInit(&db, &error), ADBC_STATUS_OK);

    AdbcConnection conn{};
    ASSERT_EQ(driver.ConnectionNew(&conn, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionInit(&conn, &db, &error), ADBC_STATUS_OK);

    AdbcStatement stmt{};
    ASSERT_EQ(driver.StatementNew(&conn, &stmt, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementSetSqlQuery(&stmt, "SELECT $1", &error), ADBC_STATUS_OK);

    nanoarrow::UniqueSchema schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_INT64, "0");
    nanoarrow::UniqueArray batch = MakeOneColumnBatch(schema.get(), {41});
    ASSERT_EQ(driver.StatementBind(&stmt, batch.get(), schema.get(), &error), ADBC_STATUS_OK);

    auto * fs = static_cast<firebolt::adbc::FireboltStatement *>(stmt.private_data);
    ASSERT_NE(fs, nullptr);
    ASSERT_TRUE(fs->bound->target_table.empty());

    AdbcStatusCode rc = driver.StatementExecuteQuery(&stmt, nullptr, nullptr, &error);
    EXPECT_EQ(rc, ADBC_STATUS_IO) << "a bound parameter set must be sent as a parameterised query";

    if (error.release)
        error.release(&error);
    driver.StatementRelease(&stmt, nullptr);
    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
}

// The target table decides the payload's destination, so ingest options arriving
// without one are an incomplete request: discarding them silently turns a misspelled
// target-table key into N parameterised executions of the caller's raw SQL.
TEST(StatementExecuteTest, IngestOptionsWithoutTargetTableRejected)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.DatabaseNew(&db, &error), ADBC_STATUS_OK);
    // Unreachable port: anything that reaches the HTTP layer shows up as IO, which
    // is how this test tells "refused up front" from "sent as something else".
    ASSERT_EQ(driver.DatabaseSetOption(&db, "uri", "http://127.0.0.1:1", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseInit(&db, &error), ADBC_STATUS_OK);

    AdbcConnection conn{};
    ASSERT_EQ(driver.ConnectionNew(&conn, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionInit(&conn, &db, &error), ADBC_STATUS_OK);

    AdbcStatement stmt{};
    ASSERT_EQ(driver.StatementNew(&conn, &stmt, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementSetSqlQuery(&stmt, "SELECT $1", &error), ADBC_STATUS_OK);
    // Every ingest option except the target table — the shape a typo in the
    // target-table key leaves behind.
    ASSERT_EQ(driver.StatementSetOption(&stmt, ADBC_INGEST_OPTION_MODE, ADBC_INGEST_OPTION_MODE_REPLACE, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementSetOption(&stmt, ADBC_INGEST_OPTION_TARGET_DB_SCHEMA, "public", &error), ADBC_STATUS_OK);

    nanoarrow::UniqueSchema schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_INT64, "0");
    nanoarrow::UniqueArray batch = MakeOneColumnBatch(schema.get(), {41});
    ASSERT_EQ(driver.StatementBind(&stmt, batch.get(), schema.get(), &error), ADBC_STATUS_OK);

    EXPECT_EQ(driver.StatementExecuteQuery(&stmt, nullptr, nullptr, &error), ADBC_STATUS_INVALID_STATE)
        << "ingest options without a target table must be an error, not a silent parameter binding";

    if (error.release)
        error.release(&error);
    driver.StatementRelease(&stmt, nullptr);
    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
}

// ADBC does not order option calls, so the mode may legitimately be set before the
// target table.  At option-set time the completeness check would reject this.
TEST(StatementExecuteTest, IngestModeSetBeforeTargetTableStillIngests)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.DatabaseNew(&db, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "uri", "http://127.0.0.1:1", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseInit(&db, &error), ADBC_STATUS_OK);

    AdbcConnection conn{};
    ASSERT_EQ(driver.ConnectionNew(&conn, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionInit(&conn, &db, &error), ADBC_STATUS_OK);

    AdbcStatement stmt{};
    ASSERT_EQ(driver.StatementNew(&conn, &stmt, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementSetOption(&stmt, ADBC_INGEST_OPTION_MODE, ADBC_INGEST_OPTION_MODE_APPEND, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementSetOption(&stmt, ADBC_INGEST_OPTION_TARGET_TABLE, "events", &error), ADBC_STATUS_OK);

    nanoarrow::UniqueSchema schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_INT64, "x");
    nanoarrow::UniqueArray batch = MakeOneColumnBatch(schema.get(), {41});
    ASSERT_EQ(driver.StatementBind(&stmt, batch.get(), schema.get(), &error), ADBC_STATUS_OK);

    EXPECT_EQ(driver.StatementExecuteQuery(&stmt, nullptr, nullptr, &error), ADBC_STATUS_IO)
        << "a complete ingest must reach the HTTP layer regardless of option order";

    if (error.release)
        error.release(&error);
    driver.StatementRelease(&stmt, nullptr);
    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
}

// BindStream is single-pass and the caller may release its side once Bind returns,
// so every batch has to be taken from the stream there and kept — one parameter set
// per row across all of them.
TEST(StatementExecuteTest, BindStreamRetainsEveryBatch)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.DatabaseNew(&db, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "uri", "http://127.0.0.1:1", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseInit(&db, &error), ADBC_STATUS_OK);

    AdbcConnection conn{};
    ASSERT_EQ(driver.ConnectionNew(&conn, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionInit(&conn, &db, &error), ADBC_STATUS_OK);

    AdbcStatement stmt{};
    ASSERT_EQ(driver.StatementNew(&conn, &stmt, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementSetSqlQuery(&stmt, "SELECT $1", &error), ADBC_STATUS_OK);

    // Two batches behind one stream, as a record batch reader yields them.
    nanoarrow::UniqueSchema schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_INT64, "0");
    nanoarrow::UniqueArray first = MakeOneColumnBatch(schema.get(), {1, 2});
    nanoarrow::UniqueArray second = MakeOneColumnBatch(schema.get(), {3});
    nanoarrow::UniqueArrayStream stream;
    ASSERT_EQ(ArrowBasicArrayStreamInit(stream.get(), schema.get(), 2), 0); // takes the schema
    ArrowBasicArrayStreamSetArray(stream.get(), 0, first.get());
    ArrowBasicArrayStreamSetArray(stream.get(), 1, second.get());

    ASSERT_EQ(driver.StatementBindStream(&stmt, stream.get(), &error), ADBC_STATUS_OK);

    auto * fs = static_cast<firebolt::adbc::FireboltStatement *>(stmt.private_data);
    ASSERT_NE(fs, nullptr);
    ASSERT_TRUE(fs->bound.has_value());
    EXPECT_TRUE(fs->bound->data_bound);
    EXPECT_EQ(fs->bound->batches.size(), 2u) << "a batch was left behind in the stream";

    // A result-returning execution: with the batches dropped, no parameter set would
    // be built and this would fail with INVALID_STATE without reaching the network.
    ArrowArrayStream out{};
    EXPECT_EQ(driver.StatementExecuteQuery(&stmt, &out, nullptr, &error), ADBC_STATUS_IO)
        << "the retained batches must be rendered into parameter sets and sent";
    if (out.release)
        out.release(&out);

    if (error.release)
        error.release(&error);
    driver.StatementRelease(&stmt, nullptr);
    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
}

// A parameter never travels as Arrow IPC, so binding must accept the view layouts
// nanoarrow's IPC writer cannot encode — pyarrow produces `string_view` for anyone
// who asks for it, and the value renders as an ordinary JSON string.
TEST(StatementExecuteTest, StringViewParameterCanBeBound)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.DatabaseNew(&db, &error), ADBC_STATUS_OK);
    // Unreachable port: IO means the parameter set was built and a request went out.
    ASSERT_EQ(driver.DatabaseSetOption(&db, "uri", "http://127.0.0.1:1", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseInit(&db, &error), ADBC_STATUS_OK);

    AdbcConnection conn{};
    ASSERT_EQ(driver.ConnectionNew(&conn, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionInit(&conn, &db, &error), ADBC_STATUS_OK);

    AdbcStatement stmt{};
    ASSERT_EQ(driver.StatementNew(&conn, &stmt, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementSetSqlQuery(&stmt, "SELECT $1", &error), ADBC_STATUS_OK);

    nanoarrow::UniqueSchema schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_STRING_VIEW, "0");
    nanoarrow::UniqueArray batch;
    ASSERT_EQ(ArrowArrayInitFromSchema(batch.get(), schema.get(), nullptr), 0);
    ASSERT_EQ(ArrowArrayStartAppending(batch.get()), 0);
    ASSERT_EQ(ArrowArrayAppendString(batch->children[0], ArrowCharView("hello")), 0);
    ASSERT_EQ(ArrowArrayFinishElement(batch.get()), 0);
    ASSERT_EQ(ArrowArrayFinishBuilding(batch.get(), NANOARROW_VALIDATION_LEVEL_DEFAULT, nullptr), 0);

    ASSERT_EQ(driver.StatementBind(&stmt, batch.get(), schema.get(), &error), ADBC_STATUS_OK)
        << "a string_view column must be bindable: " << (error.message ? error.message : "");
    if (error.release)
        error.release(&error);

    EXPECT_EQ(driver.StatementExecuteQuery(&stmt, nullptr, nullptr, &error), ADBC_STATUS_IO)
        << "the bound string_view value must be rendered and sent";

    if (error.release)
        error.release(&error);
    driver.StatementRelease(&stmt, nullptr);
    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
}

// A type Firebolt's query parameters cannot express has to be refused before any
// request goes out, rather than sent as something the server would misread.
TEST(StatementExecuteTest, UnrepresentableParameterRefusedBeforeAnyHttp)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.DatabaseNew(&db, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "uri", "http://127.0.0.1:1", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseInit(&db, &error), ADBC_STATUS_OK);

    AdbcConnection conn{};
    ASSERT_EQ(driver.ConnectionNew(&conn, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionInit(&conn, &db, &error), ADBC_STATUS_OK);

    AdbcStatement stmt{};
    ASSERT_EQ(driver.StatementNew(&conn, &stmt, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementSetSqlQuery(&stmt, "SELECT $1", &error), ADBC_STATUS_OK);

    nanoarrow::UniqueSchema schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_BINARY, "0");
    nanoarrow::UniqueArray batch;
    ASSERT_EQ(ArrowArrayInitFromSchema(batch.get(), schema.get(), nullptr), 0);
    ASSERT_EQ(ArrowArrayStartAppending(batch.get()), 0);
    ArrowBufferView bytes{};
    bytes.data.as_char = "\x01\x02";
    bytes.size_bytes = 2;
    ASSERT_EQ(ArrowArrayAppendBytes(batch->children[0], bytes), 0);
    ASSERT_EQ(ArrowArrayFinishElement(batch.get()), 0);
    ASSERT_EQ(ArrowArrayFinishBuilding(batch.get(), NANOARROW_VALIDATION_LEVEL_DEFAULT, nullptr), 0);
    ASSERT_EQ(driver.StatementBind(&stmt, batch.get(), schema.get(), &error), ADBC_STATUS_OK);

    AdbcStatusCode rc = driver.StatementExecuteQuery(&stmt, nullptr, nullptr, &error);
    EXPECT_EQ(rc, ADBC_STATUS_NOT_IMPLEMENTED) << "a binary parameter must be refused, not sent; got IO if it was sent";
    if (error.message)
        EXPECT_NE(std::string(error.message).find("binary"), std::string::npos)
            << "error should name the offending type, got: " << error.message;

    if (error.release)
        error.release(&error);
    driver.StatementRelease(&stmt, nullptr);
    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
}

// Prepare must not talk to the server: driver managers call it whenever the query
// text changes, so a round-trip here would double the request count of every
// execute().  Against an unreachable endpoint that shows up as OK, not IO.
TEST(StatementPrepareTest, PrepareIssuesNoRequest)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.DatabaseNew(&db, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "uri", "http://127.0.0.1:1", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseInit(&db, &error), ADBC_STATUS_OK);

    AdbcConnection conn{};
    ASSERT_EQ(driver.ConnectionNew(&conn, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.ConnectionInit(&conn, &db, &error), ADBC_STATUS_OK);

    AdbcStatement stmt{};
    ASSERT_EQ(driver.StatementNew(&conn, &stmt, &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.StatementSetSqlQuery(&stmt, "SELECT $1", &error), ADBC_STATUS_OK);
    EXPECT_EQ(driver.StatementPrepare(&stmt, &error), ADBC_STATUS_OK) << "Prepare reached the network";

    driver.StatementRelease(&stmt, nullptr);
    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

// GetParameterSchema, by contrast, does need the server — but only when asked.
TEST(StatementPrepareTest, GetParameterSchemaWithoutQueryFails)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcConnection conn{};
    SetupConnection(driver, db, conn);

    AdbcStatement stmt{};
    AdbcError error = ADBC_ERROR_INIT;
    ASSERT_EQ(driver.StatementNew(&conn, &stmt, &error), ADBC_STATUS_OK);

    ArrowSchema schema{};
    EXPECT_EQ(driver.StatementGetParameterSchema(&stmt, &schema, &error), ADBC_STATUS_INVALID_STATE);

    if (error.release)
        error.release(&error);
    driver.StatementRelease(&stmt, nullptr);
    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
}

// ADBC canonicalises exactly two values for a boolean option: "true" and
// "false" (ADBC_OPTION_VALUE_ENABLED / _DISABLED).  The autocommit branch used
// to read `v != "false"`, so every other spelling — "0", "FALSE", a typo, the
// empty string — silently meant autocommit ON.  That is the one option where
// misreading the value changes durability: a caller who believes they opened a
// transaction gets each statement committed as it executes, and their
// subsequent commit()/rollback() fails with INVALID_STATE against data that is
// already permanent.  Anything non-canonical has to be refused.
namespace
{

// Set the autocommit option on a fresh connection and return its status.
AdbcStatusCode SetAutocommit(AdbcDriver & driver, AdbcDatabase & db, const char * value, AdbcError * error)
{
    AdbcConnection conn{};
    EXPECT_EQ(driver.ConnectionNew(&conn, error), ADBC_STATUS_OK);
    EXPECT_EQ(driver.ConnectionInit(&conn, &db, error), ADBC_STATUS_OK);
    AdbcStatusCode code = driver.ConnectionSetOption(&conn, ADBC_CONNECTION_OPTION_AUTOCOMMIT, value, error);
    driver.ConnectionRelease(&conn, nullptr);
    return code;
}

} // namespace

TEST(AutocommitTest, CanonicalValuesAccepted)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    SetupDatabase(driver, db);
    AdbcError error = ADBC_ERROR_INIT;

    EXPECT_EQ(SetAutocommit(driver, db, ADBC_OPTION_VALUE_ENABLED, &error), ADBC_STATUS_OK);
    EXPECT_EQ(SetAutocommit(driver, db, ADBC_OPTION_VALUE_DISABLED, &error), ADBC_STATUS_OK);

    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

TEST(AutocommitTest, NonCanonicalValuesRejected)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    SetupDatabase(driver, db);

    // "0" and "FALSE" are the dangerous ones: a caller means autocommit off and
    // silently gets it on.  The rest guard against the same class of typo.
    for (const char * value : {"0", "1", "FALSE", "True", "flase", "", "yes", "off"}) // codespell:ignore
    {
        AdbcError error = ADBC_ERROR_INIT;
        EXPECT_EQ(SetAutocommit(driver, db, value, &error), ADBC_STATUS_INVALID_ARGUMENT)
            << "value=" << value << " must not be silently reinterpreted";
        if (error.message)
            EXPECT_NE(std::string(error.message).find("autocommit"), std::string::npos)
                << "error should name the option, got: " << error.message;
        if (error.release)
            error.release(&error);
    }

    driver.DatabaseRelease(&db, nullptr);
}

TEST(AutocommitTest, RejectedValueLeavesModeUnchanged)
{
    // A refused value must not half-apply: the connection has to keep whatever
    // mode it had, so the caller's next statement behaves predictably.
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcConnection conn{};
    SetupConnection(driver, db, conn);
    AdbcError error = ADBC_ERROR_INIT;

    auto * fc = static_cast<firebolt::adbc::FireboltConnection *>(conn.private_data);
    ASSERT_NE(fc, nullptr);
    ASSERT_TRUE(fc->autocommit) << "autocommit is the ADBC default";

    // Turn it off legitimately, then try to turn it back on with a bad spelling.
    ASSERT_EQ(driver.ConnectionSetOption(&conn, ADBC_CONNECTION_OPTION_AUTOCOMMIT, ADBC_OPTION_VALUE_DISABLED, &error), ADBC_STATUS_OK);
    ASSERT_FALSE(fc->autocommit);

    EXPECT_EQ(driver.ConnectionSetOption(&conn, ADBC_CONNECTION_OPTION_AUTOCOMMIT, "1", &error), ADBC_STATUS_INVALID_ARGUMENT);
    EXPECT_FALSE(fc->autocommit) << "a rejected value must not change the mode";

    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

// The autocommit key must not fall through to the session-parameter catch-all
// either: a rejected value that still landed in session_params would be
// URL-encoded onto every request.
TEST(AutocommitTest, RejectedValueNotStoredAsSessionParam)
{
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcConnection conn{};
    SetupConnection(driver, db, conn);
    AdbcError error = ADBC_ERROR_INIT;

    EXPECT_EQ(driver.ConnectionSetOption(&conn, ADBC_CONNECTION_OPTION_AUTOCOMMIT, "0", &error), ADBC_STATUS_INVALID_ARGUMENT);

    auto * fc = static_cast<firebolt::adbc::FireboltConnection *>(conn.private_data);
    ASSERT_NE(fc, nullptr);
    EXPECT_EQ(fc->session_params.count(ADBC_CONNECTION_OPTION_AUTOCOMMIT), 0u);

    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
    if (error.release)
        error.release(&error);
}

TEST(AutocommitTest, CommitRollbackInAutocommitFails)
{
    // Commit and rollback in autocommit mode (the default) must fail with
    // ADBC_STATUS_INVALID_STATE so callers know they need to disable autocommit first.
    AdbcDriver driver = InitDriver();
    AdbcDatabase db{};
    AdbcConnection conn{};
    SetupConnection(driver, db, conn);

    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(driver.ConnectionCommit(&conn, &error), ADBC_STATUS_INVALID_STATE);
    if (error.release)
        error.release(&error);
    error = ADBC_ERROR_INIT;
    EXPECT_EQ(driver.ConnectionRollback(&conn, &error), ADBC_STATUS_INVALID_STATE);
    if (error.release)
        error.release(&error);

    driver.ConnectionRelease(&conn, nullptr);
    driver.DatabaseRelease(&db, nullptr);
}

// ============================================================
// Tests: QueryParameters — Arrow values → query_parameters JSON
// ============================================================
// Firebolt derives each parameter's SQL type from the JSON type of its value, so
// which values are quoted and which are not is load-bearing, not cosmetic.

TEST(QueryParametersTest, IntegersAreJsonIntegers)
{
    EXPECT_EQ(RenderOneIntegral(NANOARROW_TYPE_INT64, 41), R"([{"name":"$1","value":41}])");
    EXPECT_EQ(RenderOneIntegral(NANOARROW_TYPE_INT32, -7), R"([{"name":"$1","value":-7}])");
    EXPECT_EQ(RenderOneIntegral(NANOARROW_TYPE_INT8, 127), R"([{"name":"$1","value":127}])");
    EXPECT_EQ(RenderOneIntegral(NANOARROW_TYPE_UINT32, 4294967295LL), R"([{"name":"$1","value":4294967295}])");
    // The largest value the server can still read back as a BIGINT.
    EXPECT_EQ(RenderOneIntegral(NANOARROW_TYPE_INT64, INT64_MAX), R"([{"name":"$1","value":9223372036854775807}])");
}

TEST(QueryParametersTest, BooleansAreJsonBooleans)
{
    EXPECT_EQ(RenderOneIntegral(NANOARROW_TYPE_BOOL, 1), R"([{"name":"$1","value":true}])");
    EXPECT_EQ(RenderOneIntegral(NANOARROW_TYPE_BOOL, 0), R"([{"name":"$1","value":false}])");
}

TEST(QueryParametersTest, NullIsAnUntypedJsonNull)
{
    nanoarrow::UniqueSchema schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_INT64, "0");
    nanoarrow::UniqueArray batch;
    ASSERT_EQ(ArrowArrayInitFromSchema(batch.get(), schema.get(), nullptr), 0);
    ASSERT_EQ(ArrowArrayStartAppending(batch.get()), 0);
    ASSERT_EQ(ArrowArrayAppendNull(batch->children[0], 1), 0);
    ASSERT_EQ(ArrowArrayFinishElement(batch.get()), 0);
    ASSERT_EQ(ArrowArrayFinishBuilding(batch.get(), NANOARROW_VALIDATION_LEVEL_DEFAULT, nullptr), 0);

    std::string json;
    std::string error;
    ASSERT_EQ(RenderParameterSet(schema.get(), batch.get(), 0, false, json, error), ADBC_STATUS_OK) << error;
    EXPECT_EQ(json, R"([{"name":"$1","value":null}])");
}

// A double has to stay a JSON number with a fractional part: emitted as a bare "3"
// the server would infer an integer parameter and substitute a BIGINT literal.
TEST(QueryParametersTest, DoublesStayFloatingPoint)
{
    nanoarrow::UniqueSchema schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_DOUBLE, "0");
    const std::vector<std::pair<double, std::string>> cases = {
        {1.5, "1.5"},
        {3.0, "3.0"},
        {-0.25, "-0.25"},
        // Shortest round-trip: a plain %.17g would print 0.10000000000000001.
        {0.1, "0.1"},
        {1e300, "1e+300"},
    };
    for (const auto & [value, expected] : cases)
    {
        nanoarrow::UniqueArray batch;
        ASSERT_EQ(ArrowArrayInitFromSchema(batch.get(), schema.get(), nullptr), 0);
        ASSERT_EQ(ArrowArrayStartAppending(batch.get()), 0);
        ASSERT_EQ(ArrowArrayAppendDouble(batch->children[0], value), 0);
        ASSERT_EQ(ArrowArrayFinishElement(batch.get()), 0);
        ASSERT_EQ(ArrowArrayFinishBuilding(batch.get(), NANOARROW_VALIDATION_LEVEL_DEFAULT, nullptr), 0);

        std::string json;
        std::string error;
        ASSERT_EQ(RenderParameterSet(schema.get(), batch.get(), 0, false, json, error), ADBC_STATUS_OK) << error;
        EXPECT_EQ(json, R"([{"name":"$1","value":)" + expected + "}]") << "value " << value;
    }
}

TEST(QueryParametersTest, NonFiniteDoubleRejected)
{
    nanoarrow::UniqueSchema schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_DOUBLE, "0");
    const std::vector<double> values = {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()};
    for (double value : values)
    {
        nanoarrow::UniqueArray batch;
        ASSERT_EQ(ArrowArrayInitFromSchema(batch.get(), schema.get(), nullptr), 0);
        ASSERT_EQ(ArrowArrayStartAppending(batch.get()), 0);
        ASSERT_EQ(ArrowArrayAppendDouble(batch->children[0], value), 0);
        ASSERT_EQ(ArrowArrayFinishElement(batch.get()), 0);
        ASSERT_EQ(ArrowArrayFinishBuilding(batch.get(), NANOARROW_VALIDATION_LEVEL_DEFAULT, nullptr), 0);

        std::string json;
        std::string error;
        // JSON has no NaN or Infinity literal, so there is nothing honest to send.
        EXPECT_EQ(RenderParameterSet(schema.get(), batch.get(), 0, false, json, error), ADBC_STATUS_INVALID_ARGUMENT);
        EXPECT_NE(error.find("finite"), std::string::npos) << error;
    }
}

// The server reads an integer parameter with std::stol, so a value above the
// signed range has to be reported here, where the offending column is still known.
TEST(QueryParametersTest, UnsignedOverflowRejected)
{
    nanoarrow::UniqueSchema schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_UINT64, "0");
    nanoarrow::UniqueArray batch;
    ASSERT_EQ(ArrowArrayInitFromSchema(batch.get(), schema.get(), nullptr), 0);
    ASSERT_EQ(ArrowArrayStartAppending(batch.get()), 0);
    ASSERT_EQ(ArrowArrayAppendUInt(batch->children[0], UINT64_MAX), 0);
    ASSERT_EQ(ArrowArrayFinishElement(batch.get()), 0);
    ASSERT_EQ(ArrowArrayFinishBuilding(batch.get(), NANOARROW_VALIDATION_LEVEL_DEFAULT, nullptr), 0);

    std::string json;
    std::string error;
    EXPECT_EQ(RenderParameterSet(schema.get(), batch.get(), 0, false, json, error), ADBC_STATUS_INVALID_ARGUMENT);
    EXPECT_NE(error.find("BIGINT"), std::string::npos) << error;
}

TEST(QueryParametersTest, StringsAreEscaped)
{
    nanoarrow::UniqueSchema schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_STRING, "0");
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"hello", R"("hello")"},
        // A quote or a backslash must not end the JSON string early.
        {"it's \"quoted\"", R"("it's \"quoted\"")"},
        {"back\\slash", R"("back\\slash")"},
        {"line\nbreak\ttab", R"("line\nbreak\ttab")"},
        // A C0 control with no short escape.
        {std::string("bell\x07"), R"("bell\u0007")"},
        // UTF-8 passes through byte for byte.
        {"\xD0\xBC\xD0\xBE\xD1\x88\xD0\xB0", "\"\xD0\xBC\xD0\xBE\xD1\x88\xD0\xB0\""},
        // SQL syntax in a value is data: the server substitutes parameters into the
        // validated AST, it does not splice text into the statement.
        {"'; DROP TABLE t; --", R"("'; DROP TABLE t; --")"},
    };
    for (const auto & [value, expected] : cases)
    {
        nanoarrow::UniqueArray batch;
        ASSERT_EQ(ArrowArrayInitFromSchema(batch.get(), schema.get(), nullptr), 0);
        ASSERT_EQ(ArrowArrayStartAppending(batch.get()), 0);
        ArrowStringView sv{value.data(), static_cast<int64_t>(value.size())};
        ASSERT_EQ(ArrowArrayAppendString(batch->children[0], sv), 0);
        ASSERT_EQ(ArrowArrayFinishElement(batch.get()), 0);
        ASSERT_EQ(ArrowArrayFinishBuilding(batch.get(), NANOARROW_VALIDATION_LEVEL_DEFAULT, nullptr), 0);

        std::string json;
        std::string error;
        ASSERT_EQ(RenderParameterSet(schema.get(), batch.get(), 0, false, json, error), ADBC_STATUS_OK) << error;
        EXPECT_EQ(json, R"([{"name":"$1","value":)" + expected + "}]") << "value " << value;
    }
}

// No JSON document can carry bytes that are not valid UTF-8, so such a string is
// reported — naming the parameter set — rather than sent as something malformed.
TEST(QueryParametersTest, InvalidUtf8StringRejected)
{
    nanoarrow::UniqueSchema schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_STRING, "0");
    nanoarrow::UniqueArray batch;
    ASSERT_EQ(ArrowArrayInitFromSchema(batch.get(), schema.get(), nullptr), 0);
    ASSERT_EQ(ArrowArrayStartAppending(batch.get()), 0);
    // A lone continuation byte: never valid on its own.
    const std::string invalid("bad\x80utf8", 8);
    ArrowStringView sv{invalid.data(), static_cast<int64_t>(invalid.size())};
    ASSERT_EQ(ArrowArrayAppendString(batch->children[0], sv), 0);
    ASSERT_EQ(ArrowArrayFinishElement(batch.get()), 0);
    ASSERT_EQ(ArrowArrayFinishBuilding(batch.get(), NANOARROW_VALIDATION_LEVEL_DEFAULT, nullptr), 0);

    std::string json;
    std::string error;
    EXPECT_EQ(RenderParameterSet(schema.get(), batch.get(), 0, false, json, error), ADBC_STATUS_INVALID_ARGUMENT);
    EXPECT_NE(error.find("serialise"), std::string::npos) << error;
}

TEST(QueryParametersTest, DatesRenderAsIsoStrings)
{
    // Day 19727 is 2024-01-05; day 0 is the epoch, and -1 the day before it.
    EXPECT_EQ(RenderOneIntegral(NANOARROW_TYPE_DATE32, 19727), R"([{"name":"$1","value":"2024-01-05"}])");
    EXPECT_EQ(RenderOneIntegral(NANOARROW_TYPE_DATE32, 0), R"([{"name":"$1","value":"1970-01-01"}])");
    EXPECT_EQ(RenderOneIntegral(NANOARROW_TYPE_DATE32, -1), R"([{"name":"$1","value":"1969-12-31"}])");
}

TEST(QueryParametersTest, TimestampsRenderAsIsoStrings)
{
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ASSERT_EQ(ArrowSchemaSetTypeDateTime(schema->children[0], NANOARROW_TYPE_TIMESTAMP, NANOARROW_TIME_UNIT_MICRO, nullptr), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "0"), 0);

    // 2024-01-05 06:07:08.123456 UTC
    nanoarrow::UniqueArray batch = MakeOneColumnBatch(schema.get(), {1704434828123456LL});
    std::string json;
    std::string error;
    ASSERT_EQ(RenderParameterSet(schema.get(), batch.get(), 0, false, json, error), ADBC_STATUS_OK) << error;
    EXPECT_EQ(json, R"([{"name":"$1","value":"2024-01-05 06:07:08.123456"}])");

    // A whole second spends no characters on a fraction.
    nanoarrow::UniqueArray whole = MakeOneColumnBatch(schema.get(), {1704434828000000LL});
    ASSERT_EQ(RenderParameterSet(schema.get(), whole.get(), 0, false, json, error), ADBC_STATUS_OK) << error;
    EXPECT_EQ(json, R"([{"name":"$1","value":"2024-01-05 06:07:08"}])");

    // Before the epoch the split into day and time-of-day must floor rather than
    // truncate towards zero.
    nanoarrow::UniqueArray before = MakeOneColumnBatch(schema.get(), {-1});
    ASSERT_EQ(RenderParameterSet(schema.get(), before.get(), 0, false, json, error), ADBC_STATUS_OK) << error;
    EXPECT_EQ(json, R"([{"name":"$1","value":"1969-12-31 23:59:59.999999"}])");
}

// Arrow stores a zoned timestamp as a UTC instant.  Say so, or the server reads it
// as a wall-clock time in whatever zone the session is using.
TEST(QueryParametersTest, ZonedTimestampCarriesItsOffset)
{
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ASSERT_EQ(ArrowSchemaSetTypeDateTime(schema->children[0], NANOARROW_TYPE_TIMESTAMP, NANOARROW_TIME_UNIT_SECOND, "America/New_York"), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "0"), 0);

    nanoarrow::UniqueArray batch = MakeOneColumnBatch(schema.get(), {1704434828LL});
    std::string json;
    std::string error;
    ASSERT_EQ(RenderParameterSet(schema.get(), batch.get(), 0, false, json, error), ADBC_STATUS_OK) << error;
    EXPECT_EQ(json, R"([{"name":"$1","value":"2024-01-05 06:07:08+00"}])");
}

TEST(QueryParametersTest, TimesRenderAsClockStrings)
{
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ASSERT_EQ(ArrowSchemaSetTypeDateTime(schema->children[0], NANOARROW_TYPE_TIME64, NANOARROW_TIME_UNIT_MICRO, nullptr), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "0"), 0);

    nanoarrow::UniqueArray batch = MakeOneColumnBatch(schema.get(), {(13 * 3600 + 14 * 60 + 15) * 1000000LL + 500});
    std::string json;
    std::string error;
    ASSERT_EQ(RenderParameterSet(schema.get(), batch.get(), 0, false, json, error), ADBC_STATUS_OK) << error;
    EXPECT_EQ(json, R"([{"name":"$1","value":"13:14:15.000500"}])");
}

// hh_mm_ss reports the magnitude of a negative duration, which would render as a
// plausible-looking time with the sign dropped.
TEST(QueryParametersTest, NegativeTimeOfDayRejected)
{
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ASSERT_EQ(ArrowSchemaSetTypeDateTime(schema->children[0], NANOARROW_TYPE_TIME64, NANOARROW_TIME_UNIT_MICRO, nullptr), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "0"), 0);

    nanoarrow::UniqueArray batch = MakeOneColumnBatch(schema.get(), {-1});
    std::string json;
    std::string error;
    EXPECT_EQ(RenderParameterSet(schema.get(), batch.get(), 0, false, json, error), ADBC_STATUS_INVALID_ARGUMENT);
    EXPECT_NE(error.find("time of day"), std::string::npos) << error;
}

// date64 counts milliseconds, so it has to floor to the day it falls in — including
// before the epoch, where truncation towards zero would land a day late.
TEST(QueryParametersTest, Date64FloorsToItsDay)
{
    nanoarrow::UniqueSchema schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_DATE64, "0");
    // Midday on 2024-01-05, then one millisecond before the epoch.
    nanoarrow::UniqueArray batch = MakeOneColumnBatch(schema.get(), {19727LL * 86400000LL + 43200000LL, -1LL});

    std::string json;
    std::string error;
    ASSERT_EQ(RenderParameterSet(schema.get(), batch.get(), 0, false, json, error), ADBC_STATUS_OK) << error;
    EXPECT_EQ(json, R"([{"name":"$1","value":"2024-01-05"}])");
    ASSERT_EQ(RenderParameterSet(schema.get(), batch.get(), 1, false, json, error), ADBC_STATUS_OK) << error;
    EXPECT_EQ(json, R"([{"name":"$1","value":"1969-12-31"}])");
}

// A decimal travels as a string: sent as a JSON number the server would read it
// back through std::stod, quietly rounding anything a double cannot hold exactly.
TEST(QueryParametersTest, DecimalsRenderAsExactStrings)
{
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
    ASSERT_EQ(ArrowSchemaSetTypeDecimal(schema->children[0], NANOARROW_TYPE_DECIMAL128, 38, 9), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "0"), 0);

    nanoarrow::UniqueArray batch;
    ASSERT_EQ(ArrowArrayInitFromSchema(batch.get(), schema.get(), nullptr), 0);
    ASSERT_EQ(ArrowArrayStartAppending(batch.get()), 0);
    ArrowDecimal decimal{};
    ArrowDecimalInit(&decimal, 128, 38, 9);
    ASSERT_EQ(ArrowDecimalSetDigits(&decimal, ArrowCharView("-12345678901234567890123456789")), 0);
    ASSERT_EQ(ArrowArrayAppendDecimal(batch->children[0], &decimal), 0);
    ASSERT_EQ(ArrowArrayFinishElement(batch.get()), 0);
    ASSERT_EQ(ArrowArrayFinishBuilding(batch.get(), NANOARROW_VALIDATION_LEVEL_DEFAULT, nullptr), 0);

    std::string json;
    std::string error;
    ASSERT_EQ(RenderParameterSet(schema.get(), batch.get(), 0, false, json, error), ADBC_STATUS_OK) << error;
    EXPECT_EQ(json, R"([{"name":"$1","value":"-12345678901234567890.123456789"}])");
}

// Types the mechanism cannot express are refused by name rather than misread — a
// JSON array arrives as pretty-printed TEXT, not an ARRAY.
TEST(QueryParametersTest, UnrepresentableTypesRejected)
{
    const std::vector<std::pair<ArrowType, std::string>> cases = {
        {NANOARROW_TYPE_BINARY, "binary"},
        {NANOARROW_TYPE_LARGE_BINARY, "binary"},
        {NANOARROW_TYPE_LIST, "list"},
        {NANOARROW_TYPE_INTERVAL_MONTHS, "interval"},
    };
    for (const auto & [column_type, expected_word] : cases)
    {
        nanoarrow::UniqueSchema schema = MakeTopLevelSchema(1);
        ASSERT_EQ(ArrowSchemaSetType(schema->children[0], column_type), 0) << ArrowTypeString(column_type);
        ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "0"), 0);
        if (column_type == NANOARROW_TYPE_LIST)
            ASSERT_EQ(ArrowSchemaSetType(schema->children[0]->children[0], NANOARROW_TYPE_INT32), 0);

        // Append a null — the one element every type can build — then mark it
        // present, since a null renders as JSON null without the type being read.
        nanoarrow::UniqueArray batch;
        ASSERT_EQ(ArrowArrayInitFromSchema(batch.get(), schema.get(), nullptr), 0);
        ASSERT_EQ(ArrowArrayStartAppending(batch.get()), 0);
        ASSERT_EQ(ArrowArrayAppendNull(batch->children[0], 1), 0);
        ASSERT_EQ(ArrowArrayFinishElement(batch.get()), 0);
        ASSERT_EQ(ArrowArrayFinishBuilding(batch.get(), NANOARROW_VALIDATION_LEVEL_DEFAULT, nullptr), 0);
        ArrowBitSet(const_cast<uint8_t *>(static_cast<const uint8_t *>(batch->children[0]->buffers[0])), 0);
        batch->children[0]->null_count = 0;

        std::string json;
        std::string error;
        EXPECT_EQ(RenderParameterSet(schema.get(), batch.get(), 0, false, json, error), ADBC_STATUS_NOT_IMPLEMENTED)
            << "type " << ArrowTypeString(column_type);
        EXPECT_NE(error.find(expected_word), std::string::npos) << error;
    }
}

TEST(QueryParametersTest, PositionalNamesFollowColumnOrder)
{
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(2);
    ASSERT_EQ(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT64), 0);
    // The dbapi layer names bound columns "0", "1", …; positional binding ignores
    // those names and uses ordinal position, which is what `$N` resolves against.
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "0"), 0);
    ASSERT_EQ(ArrowSchemaSetType(schema->children[1], NANOARROW_TYPE_STRING), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[1], "1"), 0);

    nanoarrow::UniqueArray batch;
    ASSERT_EQ(ArrowArrayInitFromSchema(batch.get(), schema.get(), nullptr), 0);
    ASSERT_EQ(ArrowArrayStartAppending(batch.get()), 0);
    ASSERT_EQ(ArrowArrayAppendInt(batch->children[0], 7), 0);
    ASSERT_EQ(ArrowArrayAppendString(batch->children[1], ArrowCharView("ann")), 0);
    ASSERT_EQ(ArrowArrayFinishElement(batch.get()), 0);
    ASSERT_EQ(ArrowArrayFinishBuilding(batch.get(), NANOARROW_VALIDATION_LEVEL_DEFAULT, nullptr), 0);

    std::string json;
    std::string error;
    ASSERT_EQ(RenderParameterSet(schema.get(), batch.get(), 0, false, json, error), ADBC_STATUS_OK) << error;
    EXPECT_EQ(json, R"([{"name":"$1","value":7},{"name":"$2","value":"ann"}])");

    // By name, each parameter is carried under its column's name *as well as* its
    // position — so param('who') resolves without `$2` ceasing to.
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "id"), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[1], "who"), 0);
    ASSERT_EQ(RenderParameterSet(schema.get(), batch.get(), 0, true, json, error), ADBC_STATUS_OK) << error;
    EXPECT_EQ(json, R"([{"name":"$1","value":7},{"name":"id","value":7},{"name":"$2","value":"ann"},{"name":"who","value":"ann"}])");
}

// A driver manager sends bind_by_name only when its own idea of it changes, and
// dbapi never revises it once parameters arrive as Arrow data, so a stale `true` is
// unavoidable.  It must not leave the `$N` placeholders unbound.
TEST(QueryParametersTest, StaleBindByNameStillBindsPositionally)
{
    nanoarrow::UniqueSchema schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_INT64, "c");
    nanoarrow::UniqueArray batch = MakeOneColumnBatch(schema.get(), {7});

    std::string json;
    std::string error;
    ASSERT_EQ(RenderParameterSet(schema.get(), batch.get(), 0, true, json, error), ADBC_STATUS_OK) << error;
    EXPECT_EQ(json, R"([{"name":"$1","value":7},{"name":"c","value":7}])")
        << "a stale bind_by_name must add a name, not replace the positional one";
}

// Duplicate names are a server-side error, so an alias that collides with a
// positional name — or with another column's — is dropped rather than sent.
TEST(QueryParametersTest, BindByNameSkipsCollidingAliases)
{
    nanoarrow::UniqueSchema schema = MakeTopLevelSchema(2);
    ASSERT_EQ(ArrowSchemaSetType(schema->children[0], NANOARROW_TYPE_INT64), 0);
    // A column literally named like the second positional parameter.
    ASSERT_EQ(ArrowSchemaSetName(schema->children[0], "$2"), 0);
    ASSERT_EQ(ArrowSchemaSetType(schema->children[1], NANOARROW_TYPE_INT64), 0);
    ASSERT_EQ(ArrowSchemaSetName(schema->children[1], "dup"), 0);

    nanoarrow::UniqueArray batch;
    ASSERT_EQ(ArrowArrayInitFromSchema(batch.get(), schema.get(), nullptr), 0);
    ASSERT_EQ(ArrowArrayStartAppending(batch.get()), 0);
    ASSERT_EQ(ArrowArrayAppendInt(batch->children[0], 1), 0);
    ASSERT_EQ(ArrowArrayAppendInt(batch->children[1], 2), 0);
    ASSERT_EQ(ArrowArrayFinishElement(batch.get()), 0);
    ASSERT_EQ(ArrowArrayFinishBuilding(batch.get(), NANOARROW_VALIDATION_LEVEL_DEFAULT, nullptr), 0);

    std::string json;
    std::string error;
    ASSERT_EQ(RenderParameterSet(schema.get(), batch.get(), 0, true, json, error), ADBC_STATUS_OK) << error;
    EXPECT_EQ(json, R"([{"name":"$1","value":1},{"name":"$2","value":2},{"name":"dup","value":2}])")
        << "the \"$2\" alias collides with the second positional name and must be dropped";
}

TEST(QueryParametersTest, EachRowIsItsOwnParameterSet)
{
    nanoarrow::UniqueSchema schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_INT64, "0");
    nanoarrow::UniqueArray batch = MakeOneColumnBatch(schema.get(), {10, 20, 30});

    std::string error;
    for (int64_t row = 0; row < 3; ++row)
    {
        std::string json;
        ASSERT_EQ(RenderParameterSet(schema.get(), batch.get(), row, false, json, error), ADBC_STATUS_OK) << error;
        EXPECT_EQ(json, R"([{"name":"$1","value":)" + std::to_string((row + 1) * 10) + "}]");
    }
}

// An unnamed column has no alias to offer, which is not a reason to fail the whole
// parameter set: the positional name still reaches the server.
TEST(QueryParametersTest, BindByNameToleratesUnnamedColumns)
{
    nanoarrow::UniqueSchema schema = MakeStructSchemaWithColumn(NANOARROW_TYPE_INT64, "");
    nanoarrow::UniqueArray batch = MakeOneColumnBatch(schema.get(), {1});

    std::string json;
    std::string error;
    ASSERT_EQ(RenderParameterSet(schema.get(), batch.get(), 0, true, json, error), ADBC_STATUS_OK) << error;
    EXPECT_EQ(json, R"([{"name":"$1","value":1}])");
}

// ============================================================
// Tests: DescribeParameters — describe response → Arrow schema
// ============================================================

namespace
{

// The shape the server produces: a compact JSON dump of result_columns and
// parameter_types, carried as one string cell of a one-row result.
std::string DescribeJson(const std::string & result_columns, const std::string & parameter_types)
{
    return R"({"result_columns":)" + result_columns + R"(,"parameter_types":)" + parameter_types + "}";
}

// The Arrow format string a Firebolt type name maps to.
std::string ArrowFormatFor(const std::string & type_name)
{
    ArrowSchema schema{};
    std::string error;
    EXPECT_EQ(firebolt::adbc::fireboltTypeNameToArrowSchema(type_name, &schema, error), ADBC_STATUS_OK) << error;
    std::string format = schema.format ? schema.format : "";
    if (schema.release)
        schema.release(&schema);
    return format;
}

} // namespace

TEST(DescribeParametersTest, ExtractsParameterTypes)
{
    firebolt::adbc::ParameterTypeList params;
    std::string error;
    ASSERT_EQ(
        firebolt::adbc::extractParameterTypes(
            DescribeJson(R"([{"name":"n","type":"integer"}])", R"({"$1":"integer","$2":"text null"})"), params, error),
        ADBC_STATUS_OK)
        << error;
    ASSERT_EQ(params.size(), 2u);
    EXPECT_EQ(params[0].first, "$1");
    EXPECT_EQ(params[0].second, "integer");
    EXPECT_EQ(params[1].first, "$2");
    EXPECT_EQ(params[1].second, "text null");
}

TEST(DescribeParametersTest, NoParametersIsNotAnError)
{
    firebolt::adbc::ParameterTypeList params;
    std::string error;
    // A statement without placeholders: the member is present but empty, and on
    // some paths null.
    EXPECT_EQ(firebolt::adbc::extractParameterTypes(DescribeJson("[]", "{}"), params, error), ADBC_STATUS_OK) << error;
    EXPECT_TRUE(params.empty());
    EXPECT_EQ(firebolt::adbc::extractParameterTypes(DescribeJson("[]", "null"), params, error), ADBC_STATUS_OK) << error;
    EXPECT_TRUE(params.empty());
    // And a response carrying only result columns.
    EXPECT_EQ(firebolt::adbc::extractParameterTypes(R"({"result_columns":[{"name":"n","type":"integer"}]})", params, error), ADBC_STATUS_OK)
        << error;
    EXPECT_TRUE(params.empty());
}

// `SELECT 1 AS parameter_types` puts that name inside result_columns; searching for
// the key rather than walking the structure would read the column list as the map.
TEST(DescribeParametersTest, ResultColumnNamedLikeTheKeyDoesNotConfuseTheReader)
{
    firebolt::adbc::ParameterTypeList params;
    std::string error;
    ASSERT_EQ(
        firebolt::adbc::extractParameterTypes(
            DescribeJson(R"([{"name":"parameter_types","type":"integer"}])", R"({"$1":"bigint"})"), params, error),
        ADBC_STATUS_OK)
        << error;
    ASSERT_EQ(params.size(), 1u);
    EXPECT_EQ(params[0].first, "$1");
    EXPECT_EQ(params[0].second, "bigint");
}

// Braces and quotes inside a column name must not throw off the bracket matching
// that skips over the result_columns array.
TEST(DescribeParametersTest, StructuralCharactersInsideStringsAreSkipped)
{
    firebolt::adbc::ParameterTypeList params;
    std::string error;
    ASSERT_EQ(
        firebolt::adbc::extractParameterTypes(
            DescribeJson(R"([{"name":"}] \"parameter_types\":{","type":"text"}])", R"({"$1":"date"})"), params, error),
        ADBC_STATUS_OK)
        << error;
    ASSERT_EQ(params.size(), 1u);
    EXPECT_EQ(params[0].second, "date");
}

TEST(DescribeParametersTest, MalformedJsonIsReported)
{
    firebolt::adbc::ParameterTypeList params;
    std::string error;
    const std::vector<std::string> bad_inputs = {"", "not json", R"({"parameter_types":)", R"({"parameter_types":{"$1":42}})"};
    for (const auto & bad : bad_inputs)
    {
        EXPECT_EQ(firebolt::adbc::extractParameterTypes(bad, params, error), ADBC_STATUS_INTERNAL) << "input: " << bad;
        EXPECT_FALSE(error.empty());
    }
}

TEST(DescribeParametersTest, TypeNamesMapToArrowTypes)
{
    EXPECT_EQ(ArrowFormatFor("integer"), "i");
    EXPECT_EQ(ArrowFormatFor("INT"), "i");
    EXPECT_EQ(ArrowFormatFor("bigint"), "l");
    EXPECT_EQ(ArrowFormatFor("long"), "l");
    EXPECT_EQ(ArrowFormatFor("real"), "f");
    EXPECT_EQ(ArrowFormatFor("double precision"), "g");
    EXPECT_EQ(ArrowFormatFor("boolean"), "b");
    EXPECT_EQ(ArrowFormatFor("text"), "u");
    EXPECT_EQ(ArrowFormatFor("bytea"), "z");
    EXPECT_EQ(ArrowFormatFor("date"), "tdD");
    EXPECT_EQ(ArrowFormatFor("timestamp"), "tsu:");
    EXPECT_EQ(ArrowFormatFor("timestampntz"), "tsu:");
    EXPECT_EQ(ArrowFormatFor("timestamptz"), "tsu:UTC");
    EXPECT_EQ(ArrowFormatFor("decimal(38, 9)"), "d:38,9");
    EXPECT_EQ(ArrowFormatFor("numeric(10,2)"), "d:10,2");
    EXPECT_EQ(ArrowFormatFor("array(integer)"), "+l");
    // A name the driver does not know: ADBC asks for NA rather than a guess.
    EXPECT_EQ(ArrowFormatFor("geography"), "n");
    EXPECT_EQ(ArrowFormatFor("unknown"), "n");
}

TEST(DescribeParametersTest, TrailingNullMarksTheFieldNullable)
{
    ArrowSchema schema{};
    std::string error;
    ASSERT_EQ(firebolt::adbc::fireboltTypeNameToArrowSchema("integer null", &schema, error), ADBC_STATUS_OK) << error;
    EXPECT_STREQ(schema.format, "i");
    EXPECT_TRUE(schema.flags & ARROW_FLAG_NULLABLE);
    schema.release(&schema);

    ASSERT_EQ(firebolt::adbc::fireboltTypeNameToArrowSchema("integer", &schema, error), ADBC_STATUS_OK) << error;
    EXPECT_FALSE(schema.flags & ARROW_FLAG_NULLABLE);
    schema.release(&schema);
}

TEST(DescribeParametersTest, NestedArraysRecurse)
{
    ArrowSchema schema{};
    std::string error;
    ASSERT_EQ(firebolt::adbc::fireboltTypeNameToArrowSchema("array(array(integer))", &schema, error), ADBC_STATUS_OK) << error;
    EXPECT_STREQ(schema.format, "+l");
    ASSERT_EQ(schema.n_children, 1);
    EXPECT_STREQ(schema.children[0]->format, "+l");
    EXPECT_STREQ(schema.children[0]->name, "item");
    ASSERT_EQ(schema.children[0]->n_children, 1);
    EXPECT_STREQ(schema.children[0]->children[0]->format, "i");
    schema.release(&schema);
}

// Ordinal position, not name order: sorted as text, "$10" lands before "$2" — as it
// does in the server's own PostgreSQL ParameterDescription.
TEST(DescribeParametersTest, ParameterSchemaIsOrderedByOrdinalPosition)
{
    std::string types = "{";
    for (int i = 1; i <= 11; ++i)
        types += (i > 1 ? "," : "") + std::string(R"("$)") + std::to_string(i) + R"(":"integer")";
    types += "}";

    ArrowSchema schema{};
    std::string error;
    ASSERT_EQ(firebolt::adbc::buildParameterSchema(DescribeJson("[]", types), &schema, error), ADBC_STATUS_OK) << error;
    ASSERT_EQ(schema.n_children, 11);
    for (int i = 0; i < 11; ++i)
        EXPECT_STREQ(schema.children[i]->name, ("$" + std::to_string(i + 1)).c_str()) << "field " << i;
    schema.release(&schema);
}

TEST(DescribeParametersTest, ParameterSchemaCarriesNamesAndTypes)
{
    ArrowSchema schema{};
    std::string error;
    ASSERT_EQ(firebolt::adbc::buildParameterSchema(DescribeJson("[]", R"({"$1":"integer","$2":"text"})"), &schema, error), ADBC_STATUS_OK)
        << error;
    EXPECT_STREQ(schema.format, "+s");
    ASSERT_EQ(schema.n_children, 2);
    EXPECT_STREQ(schema.children[0]->name, "$1");
    EXPECT_STREQ(schema.children[0]->format, "i");
    EXPECT_STREQ(schema.children[1]->name, "$2");
    EXPECT_STREQ(schema.children[1]->format, "u");
    schema.release(&schema);

    // Every parameter field is nullable whatever the server reported: NULL binds to
    // any parameter, and a caller builds its batch from this schema.  A `NOT NULL`
    // column the placeholder was compared against says nothing about that.
    ASSERT_EQ(
        firebolt::adbc::buildParameterSchema(DescribeJson("[]", R"({"$1":"integer","$2":"text null"})"), &schema, error), ADBC_STATUS_OK)
        << error;
    ASSERT_EQ(schema.n_children, 2);
    EXPECT_TRUE(schema.children[0]->flags & ARROW_FLAG_NULLABLE) << "a non-nullable parameter field cannot be bound a NULL";
    EXPECT_TRUE(schema.children[1]->flags & ARROW_FLAG_NULLABLE);
    schema.release(&schema);

    // A statement with no placeholders has an empty parameter schema, not an error.
    ASSERT_EQ(firebolt::adbc::buildParameterSchema(DescribeJson("[]", "{}"), &schema, error), ADBC_STATUS_OK) << error;
    EXPECT_STREQ(schema.format, "+s");
    EXPECT_EQ(schema.n_children, 0);
    schema.release(&schema);
}
