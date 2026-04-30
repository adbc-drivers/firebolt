#pragma once

#include "adbc.h"

#include <nanoarrow/nanoarrow.h>

#include <string>
#include <vector>

namespace firebolt::adbc
{

struct FireboltStatement;

// Quote a SQL identifier with double quotes, escaping any embedded double quotes
// by doubling them (standard SQL identifier quoting).
std::string quoteIdentifier(const std::string & name);

// Build "catalog"."schema"."table", omitting empty components.
std::string qualifiedTable(const std::string & catalog, const std::string & db_schema, const std::string & table);

// Map a single Arrow column schema to a Firebolt SQL type name.  Returns "" if
// the input cannot be represented as a Firebolt SQL type.
std::string arrowTypeToFireboltSqlType(const ArrowSchema * field);

// Build a "col1 TYPE [NOT NULL], col2 TYPE [NOT NULL], ..." column list for a
// CREATE TABLE statement from a top-level (struct) schema.  Returns "" on the
// first column that cannot be mapped or is missing a name.
std::string buildCreateTableColumns(const ArrowSchema * top_level_schema);

// Build the auto-generated SQL for a bulk ingest.  `out_pre_sql` collects the
// CREATE / DROP statements to run before the multipart INSERT — Firebolt does
// not accept multiple statements per HTTP request, so each entry is run as a
// separate request.
//
// Example: target "events" with columns (id INT NOT NULL, label STRING):
//   mode = append:
//     out_pre_sql    = []
//     out_insert_sql = INSERT INTO "events" ("id", "label")
//                      SELECT * FROM read_arrow('upload://data.arrow')
//   mode = create:
//     out_pre_sql    = [ CREATE TABLE "events" ("id" INT NOT NULL, "label" TEXT) ]
//     out_insert_sql = INSERT INTO "events" ("id", "label") SELECT * FROM read_arrow(...)
//   mode = replace:
//     out_pre_sql    = [ DROP TABLE IF EXISTS "events",
//                        CREATE TABLE "events" ("id" INT NOT NULL, "label" TEXT) ]
//     out_insert_sql = INSERT INTO "events" ("id", "label") SELECT * FROM read_arrow(...)
//   mode = create_append:
//     out_pre_sql    = [ CREATE TABLE IF NOT EXISTS "events" ("id" INT NOT NULL, "label" TEXT) ]
//     out_insert_sql = INSERT INTO "events" ("id", "label") SELECT * FROM read_arrow(...)
// With catalog/db_schema set the table reference becomes "catalog"."schema"."events".
AdbcStatusCode
buildIngestSql(const FireboltStatement * fs, std::vector<std::string> & out_pre_sql, std::string & out_insert_sql, AdbcError * error);

} // namespace firebolt::adbc
