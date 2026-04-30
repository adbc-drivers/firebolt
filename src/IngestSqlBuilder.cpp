#include "IngestSqlBuilder.h"

#include "FireboltAdbcStatement.h"

#include <cstdlib>
#include <cstring>

namespace firebolt::adbc
{

namespace
{

    AdbcStatusCode setError(AdbcError * e, AdbcStatusCode code, const std::string & msg)
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

} // namespace

std::string quoteIdentifier(const std::string & name)
{
    std::string result;
    result.reserve(name.size() + 2);
    result += '"';
    for (char c : name)
    {
        if (c == '"')
            result += '"';
        result += c;
    }
    result += '"';
    return result;
}

std::string qualifiedTable(const std::string & catalog, const std::string & db_schema, const std::string & table)
{
    std::string result;
    if (!catalog.empty())
    {
        result += quoteIdentifier(catalog);
        result += '.';
    }
    if (!db_schema.empty())
    {
        result += quoteIdentifier(db_schema);
        result += '.';
    }
    result += quoteIdentifier(table);
    return result;
}

std::string arrowTypeToFireboltSqlType(const ArrowSchema * field)
{
    if (!field)
        return {};

    ArrowSchemaView view{};
    ArrowError err{};
    if (ArrowSchemaViewInit(&view, field, &err) != 0)
        return {};

    switch (view.type)
    {
        case NANOARROW_TYPE_BOOL:
            return "BOOLEAN";
        // Firebolt has no narrower integer columns; promote tiny ints to INT/BIGINT.
        case NANOARROW_TYPE_INT8:
        case NANOARROW_TYPE_INT16:
        case NANOARROW_TYPE_INT32:
        case NANOARROW_TYPE_UINT8:
        case NANOARROW_TYPE_UINT16:
            return "INT";
        case NANOARROW_TYPE_UINT32:
        case NANOARROW_TYPE_INT64:
        case NANOARROW_TYPE_UINT64:
            return "BIGINT";
        case NANOARROW_TYPE_FLOAT:
            return "REAL";
        case NANOARROW_TYPE_DOUBLE:
            return "DOUBLE PRECISION";
        case NANOARROW_TYPE_STRING:
        case NANOARROW_TYPE_LARGE_STRING:
        case NANOARROW_TYPE_STRING_VIEW:
            return "TEXT";
        case NANOARROW_TYPE_BINARY:
        case NANOARROW_TYPE_LARGE_BINARY:
        case NANOARROW_TYPE_FIXED_SIZE_BINARY:
        case NANOARROW_TYPE_BINARY_VIEW:
            return "BYTEA";
        case NANOARROW_TYPE_DATE32:
        case NANOARROW_TYPE_DATE64:
            return "DATE";
        case NANOARROW_TYPE_TIMESTAMP:
            return (view.timezone && *view.timezone) ? "TIMESTAMPTZ" : "TIMESTAMPNTZ";
        case NANOARROW_TYPE_DECIMAL128:
        case NANOARROW_TYPE_DECIMAL256:
        case NANOARROW_TYPE_DECIMAL64:
        case NANOARROW_TYPE_DECIMAL32:
            return "DECIMAL(" + std::to_string(view.decimal_precision) + ", " + std::to_string(view.decimal_scale) + ")";
        case NANOARROW_TYPE_LIST:
        case NANOARROW_TYPE_LARGE_LIST:
        case NANOARROW_TYPE_FIXED_SIZE_LIST: {
            if (field->n_children != 1 || !field->children[0])
                return {};
            std::string inner = arrowTypeToFireboltSqlType(field->children[0]);
            if (inner.empty())
                return {};
            return "ARRAY(" + inner + ")";
        }
        case NANOARROW_TYPE_STRUCT: {
            std::string s = "STRUCT(";
            for (int64_t i = 0; i < field->n_children; ++i)
            {
                const ArrowSchema * child = field->children[i];
                if (!child)
                    return {};
                std::string child_type = arrowTypeToFireboltSqlType(child);
                if (child_type.empty())
                    return {};
                if (i > 0)
                    s += ", ";
                s += quoteIdentifier(child->name ? child->name : "");
                s += ' ';
                s += child_type;
                bool nullable = (child->flags & ARROW_FLAG_NULLABLE) != 0;
                if (!nullable)
                    s += " NOT NULL";
            }
            s += ')';
            return s;
        }
        default:
            return {};
    }
}

std::string buildCreateTableColumns(const ArrowSchema * top_level_schema)
{
    if (!top_level_schema || top_level_schema->n_children == 0)
        return {};
    std::string out;
    for (int64_t i = 0; i < top_level_schema->n_children; ++i)
    {
        const ArrowSchema * child = top_level_schema->children[i];
        if (!child || !child->name)
            return {};
        std::string sql_type = arrowTypeToFireboltSqlType(child);
        if (sql_type.empty())
            return {};
        if (i > 0)
            out += ", ";
        out += quoteIdentifier(child->name);
        out += ' ';
        out += sql_type;
        bool nullable = (child->flags & ARROW_FLAG_NULLABLE) != 0;
        if (!nullable)
            out += " NOT NULL";
    }
    return out;
}

AdbcStatusCode
buildIngestSql(const FireboltStatement * fs, std::vector<std::string> & out_pre_sql, std::string & out_insert_sql, AdbcError * error)
{
    const BulkIngestState & ingest = *fs->ingest;
    const ArrowSchema * top = ingest.schema.get();
    const bool have_schema = top && top->release && top->n_children > 0;

    // Column-name list for the INSERT clause.
    std::string col_names;
    if (have_schema)
    {
        for (int64_t i = 0; i < top->n_children; ++i)
        {
            const ArrowSchema * child = top->children[i];
            if (!child || !child->name)
                return setError(error, ADBC_STATUS_INVALID_ARGUMENT, "Bound schema column has no name");
            if (i > 0)
                col_names += ", ";
            col_names += quoteIdentifier(child->name);
        }
    }

    const std::string table_ref = qualifiedTable(ingest.target_catalog, ingest.target_db_schema, ingest.target_table);

    out_pre_sql.clear();
    if (ingest.mode == IngestMode::Create || ingest.mode == IngestMode::Replace || ingest.mode == IngestMode::CreateAppend)
    {
        if (!have_schema)
            return setError(error, ADBC_STATUS_INVALID_STATE, "ingest mode requires bound data with a schema");
        std::string columns_ddl = buildCreateTableColumns(top);
        if (columns_ddl.empty())
            return setError(error, ADBC_STATUS_NOT_IMPLEMENTED, "ingest column type cannot be mapped to a Firebolt SQL type");

        if (ingest.mode == IngestMode::Replace)
            out_pre_sql.emplace_back("DROP TABLE IF EXISTS " + table_ref);
        const char * create_kw = (ingest.mode == IngestMode::CreateAppend) ? "CREATE TABLE IF NOT EXISTS " : "CREATE TABLE ";
        out_pre_sql.emplace_back(std::string(create_kw) + table_ref + " (" + columns_ddl + ")");
    }

    out_insert_sql = "INSERT INTO " + table_ref;
    if (!col_names.empty())
        out_insert_sql += " (" + col_names + ")";
    out_insert_sql += " SELECT * FROM read_arrow('upload://data.arrow')";
    return ADBC_STATUS_OK;
}

} // namespace firebolt::adbc
