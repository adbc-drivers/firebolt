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

#include "DescribeParameters.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace firebolt::adbc
{

namespace
{

    // ============================================================
    // Type-name normalisation
    // ============================================================

    std::string toLowerTrimmed(std::string_view name)
    {
        size_t begin = 0;
        size_t end = name.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(name[begin])))
            ++begin;
        while (end > begin && std::isspace(static_cast<unsigned char>(name[end - 1])))
            --end;
        std::string lower;
        lower.reserve(end - begin);
        for (size_t i = begin; i < end; ++i)
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
        return lower;
    }

    // Parse the "(p, s)" tail of decimal(38, 9) / numeric(38, 9).
    bool parseDecimalParams(const std::string & lower, int32_t & precision, int32_t & scale)
    {
        const size_t open = lower.find('(');
        if (open == std::string::npos || lower.back() != ')')
            return false;
        const std::string args = lower.substr(open + 1, lower.size() - open - 2);
        const size_t comma = args.find(',');
        if (comma == std::string::npos)
            return false;
        const long p = std::strtol(args.substr(0, comma).c_str(), nullptr, 10);
        const long s = std::strtol(args.substr(comma + 1).c_str(), nullptr, 10);
        if (p <= 0 || p > 76 || s < 0 || s > p)
            return false;
        precision = static_cast<int32_t>(p);
        scale = static_cast<int32_t>(s);
        return true;
    }

    // The index a parameter name sorts by.  Positional names are `$<digits>`;
    // anything else (a `param('name')` parameter) sorts after them, in place.
    int64_t positionalIndex(const std::string & name)
    {
        if (name.size() < 2 || name[0] != '$')
            return -1;
        int64_t index = 0;
        for (size_t i = 1; i < name.size(); ++i)
        {
            if (!std::isdigit(static_cast<unsigned char>(name[i])))
                return -1;
            index = index * 10 + (name[i] - '0');
            if (index > 1000000)
                return -1;
        }
        return index;
    }

} // namespace

AdbcStatusCode extractParameterTypes(std::string_view describe_json, ParameterTypeList & out, std::string & out_error)
{
    out.clear();

    // Parsed structurally, not searched for by key: a result column may itself be
    // named `parameter_types`.
    nlohmann::json document = nlohmann::json::parse(describe_json, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded() || !document.is_object())
    {
        out_error = "Malformed describe response: expected a JSON object";
        return ADBC_STATUS_INTERNAL;
    }

    auto member = document.find("parameter_types");
    // Absent or null is a statement with no placeholders: an empty list, not an error.
    if (member == document.end() || member->is_null())
        return ADBC_STATUS_OK;
    if (!member->is_object())
    {
        out_error = "Malformed describe response: parameter_types is not an object";
        return ADBC_STATUS_INTERNAL;
    }

    for (const auto & [name, type] : member->items())
    {
        if (!type.is_string())
        {
            out_error = "Malformed describe response: type of parameter '" + name + "' is not a string";
            return ADBC_STATUS_INTERNAL;
        }
        out.emplace_back(name, type.get<std::string>());
    }
    return ADBC_STATUS_OK;
}

AdbcStatusCode fireboltTypeNameToArrowSchema(std::string_view type_name, ArrowSchema * out, std::string & out_error)
{
    if (!out)
    {
        out_error = "No schema to write the parameter type into";
        return ADBC_STATUS_INTERNAL;
    }

    std::string lower = toLowerTrimmed(type_name);

    // A trailing " null" is how the server marks the type nullable.
    bool nullable = false;
    if (lower.size() > 5 && lower.compare(lower.size() - 5, 5, " null") == 0)
    {
        nullable = true;
        lower.resize(lower.size() - 5);
        lower = toLowerTrimmed(lower);
    }

    ArrowSchemaInit(out);

    int rc = 0;
    if (lower.rfind("array(", 0) == 0 && !lower.empty() && lower.back() == ')')
    {
        rc = ArrowSchemaSetType(out, NANOARROW_TYPE_LIST);
        if (rc == 0)
        {
            // ArrowSchemaSetType allocated the "item" child; recurse for its element
            // type so nested arrays work.
            const std::string_view inner(lower.data() + 6, lower.size() - 7);
            ArrowSchema * item = out->children[0];
            if (item->release)
                item->release(item);
            AdbcStatusCode inner_rc = fireboltTypeNameToArrowSchema(inner, item, out_error);
            if (inner_rc != ADBC_STATUS_OK)
            {
                out->release(out);
                return inner_rc;
            }
            rc = ArrowSchemaSetName(item, "item");
        }
    }
    else if (lower.rfind("decimal(", 0) == 0 || lower.rfind("numeric(", 0) == 0)
    {
        int32_t precision = 0;
        int32_t scale = 0;
        if (parseDecimalParams(lower, precision, scale))
            rc = ArrowSchemaSetTypeDecimal(out, NANOARROW_TYPE_DECIMAL128, precision, scale);
        else
            rc = ArrowSchemaSetType(out, NANOARROW_TYPE_NA);
    }
    else if (lower == "integer" || lower == "int" || lower == "int4")
        rc = ArrowSchemaSetType(out, NANOARROW_TYPE_INT32);
    else if (lower == "bigint" || lower == "long" || lower == "int8")
        rc = ArrowSchemaSetType(out, NANOARROW_TYPE_INT64);
    else if (lower == "real" || lower == "float4" || lower == "float")
        rc = ArrowSchemaSetType(out, NANOARROW_TYPE_FLOAT);
    else if (lower == "double precision" || lower == "double" || lower == "float8")
        rc = ArrowSchemaSetType(out, NANOARROW_TYPE_DOUBLE);
    else if (lower == "boolean" || lower == "bool")
        rc = ArrowSchemaSetType(out, NANOARROW_TYPE_BOOL);
    else if (lower == "text" || lower == "varchar" || lower == "string")
        rc = ArrowSchemaSetType(out, NANOARROW_TYPE_STRING);
    else if (lower == "bytea")
        rc = ArrowSchemaSetType(out, NANOARROW_TYPE_BINARY);
    else if (lower == "date" || lower == "pgdate")
        rc = ArrowSchemaSetType(out, NANOARROW_TYPE_DATE32);
    else if (lower == "timestamp" || lower == "timestampntz")
        rc = ArrowSchemaSetTypeDateTime(out, NANOARROW_TYPE_TIMESTAMP, NANOARROW_TIME_UNIT_MICRO, nullptr);
    else if (lower == "timestamptz")
        rc = ArrowSchemaSetTypeDateTime(out, NANOARROW_TYPE_TIMESTAMP, NANOARROW_TIME_UNIT_MICRO, "UTC");
    else if (lower == "decimal" || lower == "numeric")
        rc = ArrowSchemaSetTypeDecimal(out, NANOARROW_TYPE_DECIMAL128, 38, 9);
    else
        // ADBC asks for NA when a parameter's type cannot be determined.
        rc = ArrowSchemaSetType(out, NANOARROW_TYPE_NA);

    if (rc != 0)
    {
        if (out->release)
            out->release(out);
        out_error = "Failed to build an Arrow type for parameter type '" + std::string(type_name) + "'";
        return ADBC_STATUS_INTERNAL;
    }

    if (nullable)
        out->flags |= ARROW_FLAG_NULLABLE;
    else
        out->flags &= ~ARROW_FLAG_NULLABLE;
    return ADBC_STATUS_OK;
}

AdbcStatusCode buildParameterSchema(std::string_view describe_json, ArrowSchema * out, std::string & out_error)
{
    if (!out)
    {
        out_error = "No schema to write the parameter schema into";
        return ADBC_STATUS_INTERNAL;
    }

    ParameterTypeList params;
    AdbcStatusCode rc = extractParameterTypes(describe_json, params, out_error);
    if (rc != ADBC_STATUS_OK)
        return rc;

    // Ordinal position, not lexicographic name order: "$10" must follow "$2".
    std::stable_sort(params.begin(), params.end(), [](const auto & a, const auto & b) {
        const int64_t ia = positionalIndex(a.first);
        const int64_t ib = positionalIndex(b.first);
        if (ia < 0 || ib < 0)
            return ia > ib; // named parameters keep their order, after positional ones
        return ia < ib;
    });

    ArrowSchemaInit(out);
    if (ArrowSchemaSetTypeStruct(out, static_cast<int64_t>(params.size())) != 0)
    {
        if (out->release)
            out->release(out);
        out_error = "Failed to allocate the parameter schema";
        return ADBC_STATUS_INTERNAL;
    }

    for (size_t i = 0; i < params.size(); ++i)
    {
        ArrowSchema * child = out->children[i];
        if (child->release)
            child->release(child);
        rc = fireboltTypeNameToArrowSchema(params[i].second, child, out_error);
        if (rc != ADBC_STATUS_OK)
        {
            out->release(out);
            return rc;
        }
        if (ArrowSchemaSetName(child, params[i].first.c_str()) != 0)
        {
            out->release(out);
            out_error = "Failed to name a parameter schema field";
            return ADBC_STATUS_INTERNAL;
        }
        // Every parameter is nullable whatever the server reports: a nullability
        // marker describes the *column* the placeholder was compared against, not what
        // may be bound to it.  NULL is bindable to any parameter, and a caller
        // building its batch from this schema must be able to pass one.
        child->flags |= ARROW_FLAG_NULLABLE;
    }
    return ADBC_STATUS_OK;
}

} // namespace firebolt::adbc
