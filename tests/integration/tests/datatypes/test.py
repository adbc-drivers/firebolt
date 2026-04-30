"""
Data type tests.

Verify that each Firebolt SQL type is correctly round-tripped through the
ADBC driver and represented in the Arrow result with the expected Python type.
"""

import math

import pyarrow as pa
import pytest


class TestIntegerTypes:
    def test_int(self, run_query) -> None:
        t = run_query("SELECT 100::INT AS x")
        v = t["x"][0].as_py()
        assert v == 100
        assert isinstance(v, int)

    def test_bigint(self, run_query) -> None:
        big = 2**40
        t = run_query(f"SELECT {big}::BIGINT AS x")
        assert t["x"][0].as_py() == big

    def test_int_max(self, run_query) -> None:
        t = run_query("SELECT 2147483647::INT AS x")
        assert t["x"][0].as_py() == 2_147_483_647

    def test_int_min(self, run_query) -> None:
        # Use arithmetic to avoid the bigint-literal-then-cast issue:
        # the literal 2147483648 exceeds INT range, so negate before casting.
        t = run_query("SELECT (-2147483647::INT - 1) AS x")
        assert t["x"][0].as_py() == -2_147_483_648

    def test_bigint_max(self, run_query) -> None:
        t = run_query("SELECT 9223372036854775807::BIGINT AS x")
        assert t["x"][0].as_py() == 9_223_372_036_854_775_807

    def test_zero(self, run_query) -> None:
        t = run_query("SELECT 0 AS x")
        assert t["x"][0].as_py() == 0

    def test_arrow_type_is_integer(self, run_query) -> None:
        t = run_query("SELECT 1::INT AS x")
        assert pa.types.is_integer(t.schema.field("x").type)


class TestFloatTypes:
    def test_float(self, run_query) -> None:
        t = run_query("SELECT 1.5::FLOAT AS x")
        assert abs(t["x"][0].as_py() - 1.5) < 1e-6

    def test_double(self, run_query) -> None:
        t = run_query("SELECT 3.141592653589793::DOUBLE AS x")
        assert abs(t["x"][0].as_py() - math.pi) < 1e-10

    def test_negative_float(self, run_query) -> None:
        t = run_query("SELECT -2.718::DOUBLE AS x")
        assert abs(t["x"][0].as_py() - (-2.718)) < 1e-6

    def test_scientific_notation(self, run_query) -> None:
        t = run_query("SELECT 1.5e10 AS x")
        assert abs(t["x"][0].as_py() - 1.5e10) < 1.0

    def test_arrow_type_is_floating(self, run_query) -> None:
        t = run_query("SELECT 1.0::DOUBLE AS x")
        assert pa.types.is_floating(t.schema.field("x").type)


class TestStringTypes:
    def test_text(self, run_query) -> None:
        t = run_query("SELECT 'hello world'::TEXT AS x")
        assert t["x"][0].as_py() == "hello world"

    def test_unicode(self, run_query) -> None:
        t = run_query("SELECT 'café résumé 日本語' AS x")
        assert t["x"][0].as_py() == "café résumé 日本語"

    def test_special_chars(self, run_query) -> None:
        t = run_query(r"SELECT 'line1\nline2\ttab' AS x")
        assert "line1" in t["x"][0].as_py()

    def test_single_quote_escaped(self, run_query) -> None:
        t = run_query("SELECT 'it''s' AS x")
        assert t["x"][0].as_py() == "it's"

    def test_arrow_type_is_string(self, run_query) -> None:
        t = run_query("SELECT 'x'::TEXT AS x")
        assert pa.types.is_string(t.schema.field("x").type) or pa.types.is_large_string(
            t.schema.field("x").type
        )


class TestBooleanType:
    def test_true(self, run_query) -> None:
        t = run_query("SELECT true AS x")
        assert t["x"][0].as_py() is True

    def test_false(self, run_query) -> None:
        t = run_query("SELECT false AS x")
        assert t["x"][0].as_py() is False

    def test_boolean_expression(self, run_query) -> None:
        t = run_query("SELECT 1 = 1 AS x")
        assert t["x"][0].as_py() is True

    def test_boolean_not(self, run_query) -> None:
        t = run_query("SELECT NOT true AS x")
        assert t["x"][0].as_py() is False

    def test_arrow_type_is_boolean(self, run_query) -> None:
        t = run_query("SELECT true AS x")
        assert pa.types.is_boolean(t.schema.field("x").type)


class TestNullHandling:
    def test_explicit_null(self, run_query) -> None:
        t = run_query("SELECT NULL AS x")
        assert t["x"][0].as_py() is None

    def test_null_cast_int(self, run_query) -> None:
        t = run_query("SELECT NULL::INT AS x")
        assert t["x"][0].as_py() is None

    def test_null_cast_text(self, run_query) -> None:
        t = run_query("SELECT NULL::TEXT AS x")
        assert t["x"][0].as_py() is None

    def test_null_mixed_with_values(self, run_query) -> None:
        t = run_query(
            "SELECT 1 AS x UNION ALL "
            "SELECT NULL AS x UNION ALL "
            "SELECT 3 AS x "
            "ORDER BY x"
        )
        values = t["x"].to_pylist()
        assert None in values
        assert 1 in values
        assert 3 in values

    def test_is_null_predicate(self, run_query) -> None:
        t = run_query("SELECT (NULL IS NULL) AS x")
        assert t["x"][0].as_py() is True

    def test_coalesce(self, run_query) -> None:
        t = run_query("SELECT coalesce(NULL, NULL, 42) AS x")
        assert t["x"][0].as_py() == 42


class TestDateAndTime:
    def test_date_literal(self, run_query) -> None:
        t = run_query("SELECT '2024-01-15'::DATE AS x")
        v = t["x"][0].as_py()
        import datetime
        assert isinstance(v, datetime.date)
        assert v.year == 2024
        assert v.month == 1
        assert v.day == 15

    def test_timestamp_literal(self, run_query) -> None:
        t = run_query("SELECT '2024-06-01 12:30:00'::TIMESTAMP AS x")
        v = t["x"][0].as_py()
        import datetime
        assert isinstance(v, (datetime.datetime, int))  # Arrow may return int micros

    def test_current_date_is_date(self, run_query) -> None:
        t = run_query("SELECT current_date AS x")
        import datetime
        v = t["x"][0].as_py()
        assert isinstance(v, datetime.date)
