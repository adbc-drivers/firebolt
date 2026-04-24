#include <gtest/gtest.h>

#include "ArrowIpcStream.h"
#include "adbc.h"

#include <nanoarrow/nanoarrow.hpp>
#include <nanoarrow/nanoarrow_ipc.hpp>

#include <cstring>
#include <vector>

// Entry points defined in FireboltAdbcDriver.cpp
extern "C" AdbcStatusCode AdbcDriverInit(int version, void * raw_driver, AdbcError * error);
extern "C" AdbcStatusCode FireboltAdbcDriverInit(int version, void * raw_driver, AdbcError * error);

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
    AdbcStatusCode code = FireboltAdbcDriverInit(ADBC_VERSION_1_1_0, &driver, &error);
    EXPECT_EQ(code, ADBC_STATUS_OK);
    if (error.release)
        error.release(&error);
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
    ASSERT_EQ(driver.DatabaseSetOption(&db, "adbc.firebolt.token", "tok", &error), ADBC_STATUS_OK);
    ASSERT_EQ(driver.DatabaseSetOption(&db, "adbc.firebolt.database", "mydb", &error), ADBC_STATUS_OK);
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
// Tests: connection lifecycle
// ============================================================

static void SetupDatabase(AdbcDriver & driver, AdbcDatabase & db)
{
    AdbcError error = ADBC_ERROR_INIT;
    driver.DatabaseNew(&db, &error);
    driver.DatabaseSetOption(&db, "uri", "http://localhost:9123", &error);
    driver.DatabaseSetOption(&db, "adbc.firebolt.token", "testtoken", &error);
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
    nanoarrow::UniqueSchema schema;
    ArrowSchemaInit(schema.get());
    ASSERT_EQ(ArrowSchemaSetTypeStruct(schema.get(), 1), 0);
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
// Tests: not-implemented stubs
// ============================================================

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
