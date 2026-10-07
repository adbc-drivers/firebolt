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

#include "QueryParameters.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace firebolt::adbc
{

namespace
{

    // ============================================================
    // Date and time rendering
    // ============================================================
    // Query parameters have no date/time type, so these travel as text in canonical
    // SQL form and the server coerces them.  Calendar arithmetic is std::chrono's;
    // only the digits are laid out here, since libstdc++ 11 (the pinned builder
    // image) has C++20's calendar types but not its formatters.

    std::string formatDate(std::chrono::sys_days day)
    {
        const std::chrono::year_month_day ymd{day};
        char buf[32];
        std::snprintf(
            buf,
            sizeof(buf),
            "%04d-%02u-%02u",
            static_cast<int>(ymd.year()),
            static_cast<unsigned>(ymd.month()),
            static_cast<unsigned>(ymd.day()));
        return buf;
    }

    // `since_midnight` must be within a day.
    template <typename Duration>
    void appendTimeOfDay(std::string & out, Duration since_midnight)
    {
        const std::chrono::hh_mm_ss<Duration> hms{since_midnight};
        char buf[48];
        std::snprintf(
            buf,
            sizeof(buf),
            "%02lld:%02lld:%02lld",
            static_cast<long long>(hms.hours().count()),
            static_cast<long long>(hms.minutes().count()),
            static_cast<long long>(hms.seconds().count()));
        out += buf;
        // A fraction only when there is one.
        if (hms.subseconds().count() != 0)
        {
            std::snprintf(
                buf,
                sizeof(buf),
                ".%0*lld",
                static_cast<int>(decltype(hms)::fractional_width),
                static_cast<long long>(hms.subseconds().count()));
            out += buf;
        }
    }

    // An instant since the epoch as "YYYY-MM-DD HH:MM:SS[.ffffff]".
    template <typename Duration>
    std::string formatInstant(int64_t count, bool zoned)
    {
        const std::chrono::sys_time<Duration> instant{Duration{count}};
        const auto day = std::chrono::floor<std::chrono::days>(instant);
        std::string out = formatDate(day);
        out += ' ';
        appendTimeOfDay(out, instant - day);
        // Arrow stores a zoned timestamp as a UTC instant; without the offset the
        // server reads it as wall-clock local time.
        if (zoned)
            out += "+00";
        return out;
    }

    // Dispatch an Arrow time unit to its std::chrono duration.  False for an unknown
    // unit: the caller must refuse the parameter rather than guess its scale.
    template <typename Renderer>
    bool withTimeUnit(ArrowTimeUnit unit, Renderer && render)
    {
        switch (unit)
        {
            case NANOARROW_TIME_UNIT_SECOND:
                render(std::chrono::seconds{});
                return true;
            case NANOARROW_TIME_UNIT_MILLI:
                render(std::chrono::milliseconds{});
                return true;
            case NANOARROW_TIME_UNIT_MICRO:
                render(std::chrono::microseconds{});
                return true;
            case NANOARROW_TIME_UNIT_NANO:
                render(std::chrono::nanoseconds{});
                return true;
            default:
                return false;
        }
    }

    // A float32 widened to double carries conversion noise: 0.1f serialises as
    // "0.10000000149011612", the shortest text round-tripping the *double*.  Nine
    // significant digits round-trip a float exactly, so recover the value through
    // them.
    double floatAsAuthoredDouble(double widened)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.9g", widened);
        return std::strtod(buf, nullptr);
    }

    AdbcStatusCode unsupported(std::string & out_error, const char * type_name)
    {
        out_error = std::string("Cannot bind a parameter of Arrow type ") + type_name
            + ": Firebolt query parameters carry only NULL, booleans, numbers and strings";
        return ADBC_STATUS_NOT_IMPLEMENTED;
    }

    // Set `out` to the value at `row` of `view`.  `field` supplies the logical type —
    // an ArrowArrayView carries only a storage type, which date, time, timestamp and
    // decimal cannot be read from.
    AdbcStatusCode
    arrowValueToJson(nlohmann::json & out, const ArrowSchema * field, const ArrowArrayView * view, int64_t row, std::string & out_error)
    {
        if (!field || !view)
        {
            out_error = "Bound parameter column is missing";
            return ADBC_STATUS_INTERNAL;
        }

        // JSON null becomes an untyped SQL NULL, not a NULL of the column's type.
        // The one value every Arrow type can bind as, including those refused below.
        if (ArrowArrayViewIsNull(view, row))
        {
            out = nullptr;
            return ADBC_STATUS_OK;
        }

        ArrowSchemaView schema_view{};
        ArrowError err{};
        if (ArrowSchemaViewInit(&schema_view, field, &err) != 0)
        {
            out_error = std::string("Cannot interpret bound parameter schema: ") + err.message;
            return ADBC_STATUS_INVALID_ARGUMENT;
        }

        switch (schema_view.type)
        {
            case NANOARROW_TYPE_BOOL:
                out = (ArrowArrayViewGetIntUnsafe(view, row) != 0);
                return ADBC_STATUS_OK;

            case NANOARROW_TYPE_INT8:
            case NANOARROW_TYPE_INT16:
            case NANOARROW_TYPE_INT32:
            case NANOARROW_TYPE_INT64:
            case NANOARROW_TYPE_UINT8:
            case NANOARROW_TYPE_UINT16:
            case NANOARROW_TYPE_UINT32:
                out = ArrowArrayViewGetIntUnsafe(view, row);
                return ADBC_STATUS_OK;

            case NANOARROW_TYPE_UINT64: {
                // The server parses integers with std::stol and would reject this with
                // a message about BIGINT; say so here, where the column is known.
                const uint64_t value = ArrowArrayViewGetUIntUnsafe(view, row);
                if (value > static_cast<uint64_t>(INT64_MAX))
                {
                    out_error = "Parameter value " + std::to_string(value) + " exceeds the BIGINT range Firebolt parameters support";
                    return ADBC_STATUS_INVALID_ARGUMENT;
                }
                out = static_cast<int64_t>(value);
                return ADBC_STATUS_OK;
            }

            case NANOARROW_TYPE_HALF_FLOAT:
            case NANOARROW_TYPE_FLOAT:
            case NANOARROW_TYPE_DOUBLE: {
                const double value = ArrowArrayViewGetDoubleUnsafe(view, row);
                // JSON has no NaN or Infinity, and Firebolt has no literal for them.
                if (!std::isfinite(value))
                {
                    out_error = "Parameter value is not finite; JSON query parameters cannot carry NaN or Infinity";
                    return ADBC_STATUS_INVALID_ARGUMENT;
                }
                // The fractional part survives serialisation, keeping this a DOUBLE
                // rather than a BIGINT.
                out = (schema_view.type == NANOARROW_TYPE_DOUBLE) ? value : floatAsAuthoredDouble(value);
                return ADBC_STATUS_OK;
            }

            case NANOARROW_TYPE_STRING:
            case NANOARROW_TYPE_LARGE_STRING:
            case NANOARROW_TYPE_STRING_VIEW: {
                ArrowStringView s = ArrowArrayViewGetStringUnsafe(view, row);
                out = std::string_view(s.data, static_cast<size_t>(s.size_bytes));
                return ADBC_STATUS_OK;
            }

            case NANOARROW_TYPE_DATE32:
            case NANOARROW_TYPE_DATE64: {
                const int64_t raw = ArrowArrayViewGetIntUnsafe(view, row);
                // date32 counts days; date64 counts milliseconds, which floors to the
                // day it falls in.
                const std::chrono::sys_days day = (schema_view.type == NANOARROW_TYPE_DATE32)
                    ? std::chrono::sys_days{std::chrono::days{raw}}
                    : std::chrono::floor<std::chrono::days>(
                          std::chrono::sys_time<std::chrono::milliseconds>{std::chrono::milliseconds{raw}});
                out = formatDate(day);
                return ADBC_STATUS_OK;
            }

            case NANOARROW_TYPE_TIMESTAMP: {
                const int64_t raw = ArrowArrayViewGetIntUnsafe(view, row);
                const bool zoned = schema_view.timezone && *schema_view.timezone;
                std::string text;
                if (!withTimeUnit(schema_view.time_unit, [&](auto unit) { text = formatInstant<decltype(unit)>(raw, zoned); }))
                    return unsupported(out_error, "timestamp with an unknown time unit");
                out = std::move(text);
                return ADBC_STATUS_OK;
            }

            case NANOARROW_TYPE_TIME32:
            case NANOARROW_TYPE_TIME64: {
                const int64_t raw = ArrowArrayViewGetIntUnsafe(view, row);
                // hh_mm_ss reports the magnitude of a negative duration, rendering the
                // sign away silently.
                if (raw < 0)
                {
                    out_error = "Parameter value " + std::to_string(raw) + " is not a valid time of day";
                    return ADBC_STATUS_INVALID_ARGUMENT;
                }
                std::string text;
                if (!withTimeUnit(schema_view.time_unit, [&](auto unit) { appendTimeOfDay(text, decltype(unit){raw}); }))
                    return unsupported(out_error, "time with an unknown time unit");
                out = std::move(text);
                return ADBC_STATUS_OK;
            }

            case NANOARROW_TYPE_DECIMAL32:
            case NANOARROW_TYPE_DECIMAL64:
            case NANOARROW_TYPE_DECIMAL128:
            case NANOARROW_TYPE_DECIMAL256: {
                // Sent as a string, never as a JSON number: a JSON number is read back
                // through std::stod, which would quietly round a 38-digit NUMERIC.
                ArrowDecimal decimal{};
                ArrowDecimalInit(&decimal, schema_view.decimal_bitwidth, schema_view.decimal_precision, schema_view.decimal_scale);
                ArrowArrayViewGetDecimalUnsafe(view, row, &decimal);

                // ArrowDecimalAppendStringToBuffer puts the sign at buffer position 0,
                // so it needs a buffer of its own.
                ArrowBuffer buf{};
                ArrowBufferInit(&buf);
                if (ArrowDecimalAppendStringToBuffer(&decimal, &buf) != NANOARROW_OK)
                {
                    ArrowBufferReset(&buf);
                    out_error = "Failed to render a decimal parameter";
                    return ADBC_STATUS_INTERNAL;
                }
                out = std::string(reinterpret_cast<const char *>(buf.data), static_cast<size_t>(buf.size_bytes));
                ArrowBufferReset(&buf);
                return ADBC_STATUS_OK;
            }

            // No representation in query parameters.  Sending one anyway would be
            // misread — a JSON array arrives as pretty-printed TEXT, not an ARRAY.
            case NANOARROW_TYPE_BINARY:
            case NANOARROW_TYPE_LARGE_BINARY:
            case NANOARROW_TYPE_FIXED_SIZE_BINARY:
            case NANOARROW_TYPE_BINARY_VIEW:
                return unsupported(out_error, "binary");
            case NANOARROW_TYPE_LIST:
            case NANOARROW_TYPE_LARGE_LIST:
            case NANOARROW_TYPE_FIXED_SIZE_LIST:
                return unsupported(out_error, "list");
            case NANOARROW_TYPE_STRUCT:
                return unsupported(out_error, "struct");
            case NANOARROW_TYPE_MAP:
                return unsupported(out_error, "map");
            case NANOARROW_TYPE_DICTIONARY: {
                if (!field->dictionary || !view->dictionary)
                {
                    out_error = "Bound dictionary parameter is missing its dictionary values";
                    return ADBC_STATUS_INVALID_ARGUMENT;
                }

                int64_t dictionary_index = -1;
                switch (schema_view.storage_type)
                {
                    case NANOARROW_TYPE_INT8:
                    case NANOARROW_TYPE_INT16:
                    case NANOARROW_TYPE_INT32:
                    case NANOARROW_TYPE_INT64:
                        dictionary_index = ArrowArrayViewGetIntUnsafe(view, row);
                        break;
                    case NANOARROW_TYPE_UINT8:
                    case NANOARROW_TYPE_UINT16:
                    case NANOARROW_TYPE_UINT32:
                    case NANOARROW_TYPE_UINT64: {
                        const uint64_t unsigned_index = ArrowArrayViewGetUIntUnsafe(view, row);
                        if (unsigned_index <= static_cast<uint64_t>(INT64_MAX))
                            dictionary_index = static_cast<int64_t>(unsigned_index);
                        break;
                    }
                    default:
                        out_error = "Bound dictionary parameter has a non-integer index type";
                        return ADBC_STATUS_INVALID_ARGUMENT;
                }

                if (dictionary_index < 0 || dictionary_index >= view->dictionary->length)
                {
                    out_error = "Bound dictionary parameter index is outside the dictionary";
                    return ADBC_STATUS_INVALID_ARGUMENT;
                }
                return arrowValueToJson(out, field->dictionary, view->dictionary, dictionary_index, out_error);
            }
            default:
                return unsupported(out_error, ArrowTypeString(schema_view.type));
        }
    }

} // namespace

AdbcStatusCode buildQueryParametersJson(
    const ArrowSchema * schema,
    const ArrowArrayView * batch_view,
    int64_t row,
    bool bind_by_name,
    std::string & out_json,
    std::string & out_error)
{
    if (!schema || !batch_view)
    {
        out_error = "Bound parameter data is missing";
        return ADBC_STATUS_INTERNAL;
    }

    const int64_t n_columns = schema->n_children;
    if (batch_view->n_children != n_columns)
    {
        out_error = "Bound parameter data does not match its schema";
        return ADBC_STATUS_INTERNAL;
    }

    nlohmann::json parameters = nlohmann::json::array();

    // All positional names are reserved up front, so an alias cannot collide with one
    // not yet reached — a column named "$2" at position 1 would duplicate "$2".
    std::set<std::string> reserved;
    for (int64_t i = 0; i < n_columns; ++i)
        reserved.insert("$" + std::to_string(i + 1));

    for (int64_t i = 0; i < n_columns; ++i)
    {
        nlohmann::json value;
        AdbcStatusCode rc = arrowValueToJson(value, schema->children[i], batch_view->children[i], row, out_error);
        if (rc != ADBC_STATUS_OK)
            return rc;

        // The positional name is always emitted: `$N` is the server's only
        // placeholder syntax, and an unreferenced parameter is ignored.  So it costs
        // one unused entry and makes a stale `bind_by_name` harmless — dbapi sends the
        // option only when its own idea of it changes, never revising it once
        // parameters arrive as Arrow data.
        parameters.push_back({{"name", "$" + std::to_string(i + 1)}, {"value", value}});

        if (!bind_by_name)
            continue;

        // An extra alias so `param('name')` resolves.  An unnamed column has no alias
        // rather than failing the set.  Duplicate names are a server-side error, so a
        // column colliding with another parameter keeps only its positional entry.
        const char * column_name = schema->children[i] ? schema->children[i]->name : nullptr;
        if (!column_name || !*column_name || !reserved.insert(column_name).second)
            continue;
        parameters.push_back({{"name", column_name}, {"value", std::move(value)}});
    }

    try
    {
        out_json = parameters.dump();
    }
    catch (const nlohmann::json::exception & ex)
    {
        // Only fails on a string that is not valid UTF-8, which no JSON document can
        // carry.
        out_error = std::string("Cannot serialise the bound parameters: ") + ex.what();
        return ADBC_STATUS_INVALID_ARGUMENT;
    }
    return ADBC_STATUS_OK;
}

} // namespace firebolt::adbc
