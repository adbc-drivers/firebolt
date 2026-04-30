"""
DECIMAL type tests.

Verify that DECIMAL(p, s) values are correctly round-tripped through the
ADBC driver and represented in Arrow as decimal128 with correct precision,
scale, and arithmetic behaviour.
"""

import decimal

import pyarrow as pa
import pytest


class TestDecimalLiterals:
    def test_decimal_value(self, run_query) -> None:
        t = run_query("SELECT 3.14::DECIMAL(5,2) AS x")
        v = t["x"][0].as_py()
        assert isinstance(v, decimal.Decimal)
        assert v == decimal.Decimal("3.14")

    def test_negative_decimal(self, run_query) -> None:
        t = run_query("SELECT -9.99::DECIMAL(5,2) AS x")
        assert t["x"][0].as_py() == decimal.Decimal("-9.99")

    def test_zero_decimal(self, run_query) -> None:
        t = run_query("SELECT 0.00::DECIMAL(5,2) AS x")
        assert t["x"][0].as_py() == decimal.Decimal("0.00")

    def test_large_decimal(self, run_query) -> None:
        t = run_query("SELECT 123456789.12::DECIMAL(18,2) AS x")
        assert t["x"][0].as_py() == decimal.Decimal("123456789.12")

    def test_high_precision(self, run_query) -> None:
        t = run_query("SELECT 1.123456789012::DECIMAL(18,12) AS x")
        v = t["x"][0].as_py()
        assert abs(float(v) - 1.123456789012) < 1e-11

    def test_arrow_type_is_decimal(self, run_query) -> None:
        t = run_query("SELECT 1.5::DECIMAL(10,2) AS x")
        assert pa.types.is_decimal(t.schema.field("x").type)

    def test_null_decimal(self, run_query) -> None:
        t = run_query("SELECT NULL::DECIMAL(10,2) AS x")
        assert t["x"][0].as_py() is None


class TestDecimalArithmetic:
    def test_addition(self, run_query) -> None:
        t = run_query("SELECT (1.1::DECIMAL(5,1) + 2.2::DECIMAL(5,1)) AS x")
        assert float(t["x"][0].as_py()) == pytest.approx(3.3, abs=1e-6)

    def test_subtraction(self, run_query) -> None:
        t = run_query("SELECT (5.0::DECIMAL(5,1) - 2.5::DECIMAL(5,1)) AS x")
        assert float(t["x"][0].as_py()) == pytest.approx(2.5, abs=1e-6)

    def test_multiplication(self, run_query) -> None:
        t = run_query("SELECT (2.5::DECIMAL(5,1) * 4.0::DECIMAL(5,1)) AS x")
        assert float(t["x"][0].as_py()) == pytest.approx(10.0, abs=1e-6)

    def test_comparison(self, run_query) -> None:
        t = run_query(
            "SELECT (1.1::DECIMAL(5,2) < 1.2::DECIMAL(5,2)) AS x"
        )
        assert t["x"][0].as_py() is True


class TestDecimalRoundtrip:
    def test_roundtrip_via_table(self, run_query, table_name) -> None:
        run_query(f"CREATE TABLE {table_name} (price DECIMAL(10,2))")
        run_query(f"INSERT INTO {table_name} VALUES (99.95), (0.01), (-12.50)")
        t = run_query(f"SELECT price FROM {table_name} ORDER BY price")
        values = [v.as_py() for v in t["price"]]
        assert values == [
            decimal.Decimal("-12.50"),
            decimal.Decimal("0.01"),
            decimal.Decimal("99.95"),
        ]
        run_query(f"DROP TABLE {table_name}")

    def test_sum_decimal_column(self, run_query, table_name) -> None:
        run_query(f"CREATE TABLE {table_name} (amount DECIMAL(10,2))")
        run_query(f"INSERT INTO {table_name} VALUES (10.00), (20.00), (30.00)")
        t = run_query(f"SELECT SUM(amount) AS total FROM {table_name}")
        assert float(t["total"][0].as_py()) == pytest.approx(60.00, abs=1e-6)
        run_query(f"DROP TABLE {table_name}")

    def test_avg_decimal_column(self, run_query, table_name) -> None:
        run_query(f"CREATE TABLE {table_name} (val DECIMAL(10,4))")
        run_query(f"INSERT INTO {table_name} VALUES (1.0000), (2.0000), (3.0000)")
        t = run_query(f"SELECT AVG(val) AS avg_val FROM {table_name}")
        assert float(t["avg_val"][0].as_py()) == pytest.approx(2.0, abs=1e-4)
        run_query(f"DROP TABLE {table_name}")

    def test_null_roundtrip(self, run_query, table_name) -> None:
        run_query(f"CREATE TABLE {table_name} (val DECIMAL(10,2))")
        run_query(f"INSERT INTO {table_name} VALUES (1.00), (NULL), (3.00)")
        t = run_query(f"SELECT val FROM {table_name} ORDER BY val")
        values = [v.as_py() for v in t["val"]]
        assert None in values
        assert decimal.Decimal("1.00") in values
        assert decimal.Decimal("3.00") in values

    def test_schema_precision_and_scale(self, run_query, table_name) -> None:
        run_query(f"CREATE TABLE {table_name} (val DECIMAL(15,4))")
        run_query(f"INSERT INTO {table_name} VALUES (1.0000)")
        t = run_query(f"SELECT val FROM {table_name}")
        field_type = t.schema.field("val").type
        assert pa.types.is_decimal(field_type)
        assert field_type.precision == 15
        assert field_type.scale == 4
        run_query(f"DROP TABLE {table_name}")
