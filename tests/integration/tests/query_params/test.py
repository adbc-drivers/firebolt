"""Query parameter binding tests.

Firebolt substitutes `$1`, `$2`, … from the `query_parameters` setting, which the
driver builds from the Arrow data passed to Bind/BindStream.  Substitution happens in
the server's query validator — a parameter becomes a literal node of the validated
AST, not text spliced into the statement.

Two things are under test: that the value arrives intact, and that it arrives with
the right *type*.  A parameter's type is the JSON type of its value, so
`cursor.execute("SELECT $1", (41,))` returning an integer column is the proof that
the driver sent `41` and not `"41"`.
"""

import datetime
import decimal

import adbc_driver_manager
import pyarrow as pa
import pytest


class TestPositionalParameters:
    def test_integer_parameter_is_numeric(self, cursor) -> None:
        # Arithmetic on the placeholder only works if the server typed it as a
        # number; a TEXT parameter would need an explicit cast.
        cursor.execute("SELECT $1 + 1 AS result", (41,))
        assert cursor.fetchone()[0] == 42

    def test_integer_parameter_keeps_its_type(self, cursor) -> None:
        cursor.execute("SELECT $1 AS v", (41,))
        table = cursor.fetch_arrow_table()
        assert table["v"][0].as_py() == 41
        assert pa.types.is_integer(table.schema.field("v").type)

    def test_float_parameter_keeps_its_type(self, cursor) -> None:
        cursor.execute("SELECT $1 AS v", (1.5,))
        table = cursor.fetch_arrow_table()
        assert table["v"][0].as_py() == pytest.approx(1.5)
        assert pa.types.is_floating(table.schema.field("v").type)

    def test_whole_float_stays_floating_point(self, cursor) -> None:
        # 3.0 must not be sent as a bare `3`, which the server would read as an
        # integer parameter.
        cursor.execute("SELECT $1 AS v", (3.0,))
        table = cursor.fetch_arrow_table()
        assert pa.types.is_floating(table.schema.field("v").type)

    def test_boolean_parameter_keeps_its_type(self, cursor) -> None:
        cursor.execute("SELECT $1 AS v", (True,))
        table = cursor.fetch_arrow_table()
        assert table["v"][0].as_py() is True
        assert pa.types.is_boolean(table.schema.field("v").type)

    def test_string_parameter(self, cursor) -> None:
        cursor.execute("SELECT $1 || ', world!' AS v", ("Hello",))
        assert cursor.fetchone()[0] == "Hello, world!"

    def test_none_is_null(self, cursor) -> None:
        cursor.execute("SELECT $1 IS NULL AS v", (None,))
        assert cursor.fetchone()[0] is True

    def test_several_parameters_in_order(self, cursor) -> None:
        cursor.execute("SELECT $1 AS a, $2 AS b, $3 AS c", (1, "two", 3.5))
        row = cursor.fetchone()
        assert row[0] == 1
        assert row[1] == "two"
        assert row[2] == pytest.approx(3.5)

    def test_ten_or_more_parameters_stay_in_order(self, cursor) -> None:
        # Ordinal position past nine is where text-sorted parameter names would
        # start to disagree with the placeholders.
        values = tuple(range(1, 13))
        select = ", ".join(f"${i} AS c{i}" for i in range(1, 13))
        cursor.execute(f"SELECT {select}", values)
        assert cursor.fetchone() == values

    def test_parameter_reused_in_one_statement(self, cursor) -> None:
        cursor.execute("SELECT $1 + $1 AS v", (21,))
        assert cursor.fetchone()[0] == 42


class TestParameterValues:
    """Values whose text form has to survive URL-encoding and JSON escaping."""

    @pytest.mark.parametrize(
        "value",
        [
            "plain",
            "it's quoted",
            'double "quoted"',
            "back\\slash",
            "amp & equals = brace }",
            "percent %20 plus +",
            "new\nline\ttab",
            "Файрболт 🔥",
            "",
        ],
    )
    def test_string_round_trips(self, cursor, value: str) -> None:
        cursor.execute("SELECT $1 AS v", (value,))
        assert cursor.fetchone()[0] == value

    def test_sql_syntax_in_a_value_is_data(self, cursor, temp_table) -> None:
        cursor.execute(f"INSERT INTO {temp_table} VALUES (1, 'alice', 1.0)")
        cursor.execute(f"SELECT label FROM {temp_table} WHERE label = $1", ("'; DROP TABLE t; --",))
        assert cursor.fetchall() == []
        # The table is still there, which it would not be if the value had been
        # spliced into the statement text.
        cursor.execute(f"SELECT count(*) FROM {temp_table}")
        assert cursor.fetchone()[0] == 1

    def test_date_parameter(self, cursor) -> None:
        cursor.execute("SELECT $1::DATE AS v", (datetime.date(2024, 1, 5),))
        assert cursor.fetchone()[0] == datetime.date(2024, 1, 5)

    def test_timestamp_parameter(self, cursor) -> None:
        value = datetime.datetime(2024, 1, 5, 6, 7, 8, 123456)
        cursor.execute("SELECT $1::TIMESTAMP AS v", (value,))
        assert cursor.fetchone()[0] == value

    def test_decimal_parameter_keeps_full_precision(self, cursor) -> None:
        # Sent as a string, so no rounding through a double on the way.
        value = decimal.Decimal("-12345678901234567890.123456789")
        cursor.execute("SELECT $1::DECIMAL(38, 9) AS v", (value,))
        assert cursor.fetchone()[0] == value

    def test_large_integer_parameter(self, cursor) -> None:
        cursor.execute("SELECT $1 AS v", (2**63 - 1,))
        assert cursor.fetchone()[0] == 2**63 - 1

    def test_integer_above_bigint_is_rejected(self, conn) -> None:
        # The server reads integer parameters as signed BIGINT, so a uint64 above that
        # range is refused here, where the column is still identifiable.  Bound
        # low-level with an explicit uint64 array: a Python int this large fails in
        # pyarrow's own type inference.
        batch = pa.record_batch([pa.array([2**64 - 1], type=pa.uint64())], names=["0"])
        with adbc_driver_manager.AdbcStatement(conn) as stmt:
            stmt.set_sql_query("SELECT $1 AS v")
            stmt.bind(batch)
            with pytest.raises(Exception, match="BIGINT"):
                stmt.execute_query()

    def test_non_finite_float_is_rejected(self, cursor) -> None:
        with pytest.raises(Exception, match="finite"):
            cursor.execute("SELECT $1 AS v", (float("nan"),))


class TestParametersAgainstTables:
    def test_filter_on_an_integer_column(self, cursor, temp_table) -> None:
        cursor.execute(f"INSERT INTO {temp_table} VALUES (1, 'alice', 1.0), (2, 'bob', 2.0)")
        cursor.execute(f"SELECT label FROM {temp_table} WHERE id = $1", (2,))
        assert cursor.fetchall() == [("bob",)]

    def test_filter_on_a_text_column(self, cursor, temp_table) -> None:
        cursor.execute(f"INSERT INTO {temp_table} VALUES (1, 'alice', 1.0), (2, 'bob', 2.0)")
        cursor.execute(f"SELECT id FROM {temp_table} WHERE label = $1", ("alice",))
        assert cursor.fetchall() == [(1,)]

    def test_null_parameter_matches_nothing(self, cursor, temp_table) -> None:
        cursor.execute(f"INSERT INTO {temp_table} VALUES (1, 'alice', 1.0)")
        cursor.execute(f"SELECT id FROM {temp_table} WHERE label = $1", (None,))
        assert cursor.fetchall() == []

    def test_parameterised_insert(self, cursor, temp_table) -> None:
        cursor.execute(f"INSERT INTO {temp_table} VALUES ($1, $2, $3)", (10, "dave", 8.5))
        cursor.execute(f"SELECT id, label, value FROM {temp_table} WHERE id = 10")
        assert cursor.fetchone() == (10, "dave", pytest.approx(8.5))

    def test_executemany_runs_once_per_parameter_set(self, cursor, temp_table) -> None:
        rows = [(i, f"row{i}", float(i)) for i in range(5)]
        cursor.executemany(f"INSERT INTO {temp_table} VALUES ($1, $2, $3)", rows)
        cursor.execute(f"SELECT id, label FROM {temp_table} ORDER BY id")
        assert cursor.fetchall() == [(i, f"row{i}") for i in range(5)]

    def test_execute_with_multi_row_arrow_data_runs_every_row(self, cursor, temp_table) -> None:
        # dbapi's execute() documents that multi-row Arrow data means "multiple
        # parameters, which will each be bound in turn" — and it always asks for a
        # result set, so this must not be refused for wanting one.
        cursor.execute(
            f"INSERT INTO {temp_table} VALUES ($1, $2, $3)",
            pa.table({"a": [1, 2, 3], "b": ["x", "y", "z"], "c": [1.0, 2.0, 3.0]}),
        )
        cursor.execute(f"SELECT id, label FROM {temp_table} ORDER BY id")
        assert cursor.fetchall() == [(1, "x"), (2, "y"), (3, "z")]

    def test_view_layout_columns_can_be_bound(self, cursor) -> None:
        # A parameter never travels as Arrow IPC, so binding must accept the layouts
        # nanoarrow's IPC writer cannot encode.
        cursor.execute(
            "SELECT $1 AS s, $2 AS n",
            pa.table(
                {
                    "s": pa.array(["hello"], type=pa.string_view()),
                    "n": pa.array([41], type=pa.int64()),
                }
            ),
        )
        assert cursor.fetchone() == ("hello", 41)

    def test_executemany_with_no_parameter_sets_does_nothing(self, cursor, temp_table) -> None:
        cursor.executemany(f"INSERT INTO {temp_table} VALUES ($1, $2, $3)", [])
        cursor.execute(f"SELECT count(*) FROM {temp_table}")
        assert cursor.fetchone()[0] == 0

    def test_statement_reuse_with_different_values(self, cursor, temp_table) -> None:
        cursor.execute(f"INSERT INTO {temp_table} VALUES (1, 'alice', 1.0), (2, 'bob', 2.0), (3, 'carol', 3.0)")
        for expected_id, expected_label in [(1, "alice"), (2, "bob"), (3, "carol")]:
            cursor.execute(f"SELECT label FROM {temp_table} WHERE id = $1", (expected_id,))
            assert cursor.fetchone()[0] == expected_label


class TestNamedParameters:
    """Firebolt has no named *placeholder* — `$foo` lexes as an identifier — so a
    named parameter is read back through the `param()` function, which always
    yields TEXT regardless of the JSON type the driver sent."""

    def test_named_parameter(self, cursor) -> None:
        cursor.execute("SELECT param('who') AS v", {"who": "ann"})
        assert cursor.fetchone()[0] == "ann"
        # lol

    def test_named_parameter_needs_a_cast_to_be_numeric(self, cursor) -> None:
        cursor.execute("SELECT param('cutoff')::INT + 1 AS v", {"cutoff": 41})
        assert cursor.fetchone()[0] == 42

    def test_named_binding_survives_repeated_execution(self, cursor) -> None:
        # dbapi sends bind_by_name only when its own idea of the setting changes, so a
        # driver that reset it with the bound payload would send `$1` instead of `who`
        # on the second call and the server would report the parameter as not set.
        for name in ["ann", "bob", "carol"]:
            cursor.execute("SELECT param('who') AS v", {"who": name})
            assert cursor.fetchone()[0] == name

    def test_switching_between_named_and_positional(self, cursor) -> None:
        cursor.execute("SELECT param('who') AS v", {"who": "ann"})
        assert cursor.fetchone()[0] == "ann"
        cursor.execute("SELECT $1 AS v", ("bob",))
        assert cursor.fetchone()[0] == "bob"
        cursor.execute("SELECT param('who') AS v", {"who": "carol"})
        assert cursor.fetchone()[0] == "carol"

    def test_positional_still_works_after_a_named_call(self, cursor, temp_table) -> None:
        # dbapi sends bind_by_name once per cursor and never revises it when
        # parameters arrive as Arrow data, so it is still set here.  It must not leave
        # the `$N` placeholders unbound.
        cursor.execute("SELECT param('who') AS v", {"who": "ann"})
        assert cursor.fetchone()[0] == "ann"

        cursor.executemany(
            f"INSERT INTO {temp_table} VALUES ($1, $2, $3)",
            pa.table({"a": [1, 2], "b": ["x", "y"], "c": [1.0, 2.0]}),
        )
        cursor.execute(f"SELECT id, label FROM {temp_table} ORDER BY id")
        assert cursor.fetchall() == [(1, "x"), (2, "y")]

    def test_several_named_parameters(self, cursor, temp_table) -> None:
        cursor.execute(f"INSERT INTO {temp_table} VALUES (1, 'alice', 1.0), (2, 'bob', 2.0)")
        cursor.execute(
            f"SELECT id FROM {temp_table} WHERE label = param('who') AND id < param('lt')::INT",
            {"who": "alice", "lt": 2},
        )
        assert cursor.fetchall() == [(1,)]


class TestParameterErrors:
    def test_placeholder_without_a_bound_value(self, cursor) -> None:
        # The server reports this: the driver cannot know how many placeholders
        # the statement has without asking.
        with pytest.raises(Exception, match=r"\$1"):
            cursor.execute("SELECT $1 AS v")

    def test_binary_parameter_is_rejected(self, cursor) -> None:
        # `query_parameters` carries only NULL, booleans, numbers and strings.
        with pytest.raises(Exception, match="binary"):
            cursor.execute("SELECT $1 AS v", (b"\x01\x02",))

    def test_list_parameter_is_rejected(self, cursor) -> None:
        with pytest.raises(Exception, match="list"):
            cursor.execute("SELECT $1 AS v", ([1, 2],))
