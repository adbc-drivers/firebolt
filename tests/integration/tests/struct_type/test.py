# Copyright (c) 2026 ADBC Drivers Contributors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#         http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""
STRUCT type tests — retrieval and insertion.

Two directions, both over the same shape catalogue:

* **Retrieval** — a Firebolt STRUCT column arrives as an Arrow ``struct<...>``
  and an ``ARRAY(STRUCT(...))`` as ``list<struct<...>>``, nested to any depth,
  with NULLs preserved at every level.
* **Insertion** — bulk ingest uploads Arrow IPC bytes that the server reads
  with ``read_arrow()``.  For the create-style modes the driver also
  synthesises the ``CREATE TABLE`` DDL from the bound Arrow schema, so each
  nested shape has to render to a Firebolt type the server accepts.

Shapes mirror the server-side ``read_arrow_struct*`` suites: flat and nested
structs, arrays inside structs, structs inside arrays, arrays of arrays of
structs, struct/array alternation three levels deep, every scalar type as a
struct field, NULLs at each level, and reserved-keyword field names.

Firebolt requires every STRUCT *field* to be nullable, so a non-nullable Arrow
field is widened on the way in; that is pinned below rather than left to
chance.  The exact Arrow → Firebolt type strings are unit-tested in
``tests/unit/adbc_driver_test.cpp``; what the shape cases here add is that the
server accepts the DDL and the values survive the round trip.
"""

import datetime
from decimal import Decimal

import adbc_driver_manager
import pyarrow as pa
import pytest
from adbc_driver_manager import dbapi

# --------------------------------------------------------------------------- #
# Helpers  (`conn`, `run_query`, `ingest`, `cursor` and `scratch_table` come   #
# from conftest.py)                                                           #
# --------------------------------------------------------------------------- #


def _column_types(run_query, table_name):
    """{column_name: Firebolt type text} for a table, straight from the catalog.

    The type text is how the server rendered the DDL the driver synthesised, so
    it pins the Arrow → Firebolt nested-type mapping end to end.
    """
    t = run_query(
        "SELECT column_name, data_type FROM information_schema.columns "
        f"WHERE table_name = '{table_name}'"
    )
    return dict(zip(t["column_name"].to_pylist(), t["data_type"].to_pylist()))


# Reused shapes.  Field/element nullability is pyarrow's default (nullable).
_FLAT = pa.struct([("a", pa.int32()), ("b", pa.string())])
_NESTED = pa.struct(
    [("x", pa.int32()), ("child", pa.struct([("y", pa.int64()), ("z", pa.string())]))]
)
_ARR_STRUCT = pa.list_(pa.struct([("k", pa.int32()), ("v", pa.string())]))
# ARRAY(STRUCT(a ARRAY(STRUCT(leaf INT)), b STRUCT(xs ARRAY(INT)))) — alternation
# in both directions inside one column.
_ALTERNATING = pa.list_(
    pa.struct(
        [
            ("a", pa.list_(pa.struct([("leaf", pa.int32())]))),
            ("b", pa.struct([("xs", pa.list_(pa.int32()))])),
        ]
    )
)


# --------------------------------------------------------------------------- #
# Retrieval: SQL STRUCT → Arrow struct                                        #
# --------------------------------------------------------------------------- #


class TestStructRetrievalShapes:
    """Arrow type reconstruction for each nesting shape, read straight off a
    SELECT of struct literals — no table involved."""

    def test_flat_struct(self, run_query) -> None:
        t = run_query("SELECT struct(1, 'x')::struct(a INT, b TEXT) AS s")
        typ = t.schema.field("s").type
        assert pa.types.is_struct(typ)
        assert [f.name for f in typ] == ["a", "b"]
        assert typ.field("a").type == pa.int32()
        assert typ.field("b").type == pa.string()
        assert t["s"][0].as_py() == {"a": 1, "b": "x"}

    def test_nested_struct(self, run_query) -> None:
        t = run_query(
            "SELECT struct(10, struct(20, 'y')::struct(y INT, z TEXT))"
            "::struct(x INT, child STRUCT(y INT, z TEXT)) AS s"
        )
        typ = t.schema.field("s").type
        assert pa.types.is_struct(typ.field("child").type)
        assert t["s"][0].as_py() == {"x": 10, "child": {"y": 20, "z": "y"}}

    def test_struct_with_array_field(self, run_query) -> None:
        t = run_query(
            "SELECT struct([1, 2, 3], 'w')::struct(xs ARRAY(INT), name TEXT) AS s"
        )
        assert pa.types.is_list(t.schema.field("s").type.field("xs").type)
        assert t["s"][0].as_py() == {"xs": [1, 2, 3], "name": "w"}

    def test_array_of_structs(self, run_query) -> None:
        t = run_query(
            "SELECT [struct(1, 'a')::struct(k INT, v TEXT), struct(2, 'b')::struct(k INT, v TEXT)] AS arr"
        )
        typ = t.schema.field("arr").type
        assert pa.types.is_list(typ)
        assert pa.types.is_struct(typ.value_type)
        assert t["arr"][0].as_py() == [{"k": 1, "v": "a"}, {"k": 2, "v": "b"}]

    def test_array_of_array_of_structs(self, run_query) -> None:
        t = run_query(
            "SELECT [[struct(1)::struct(m INT)], [struct(2)::struct(m INT)]] AS arr"
        )
        typ = t.schema.field("arr").type
        assert pa.types.is_list(typ.value_type)
        assert pa.types.is_struct(typ.value_type.value_type)
        assert t["arr"][0].as_py() == [[{"m": 1}], [{"m": 2}]]

    def test_empty_array_of_structs(self, run_query) -> None:
        t = run_query("SELECT []::ARRAY(STRUCT(k INT)) AS arr")
        assert t["arr"][0].as_py() == []

    def test_null_struct(self, run_query) -> None:
        t = run_query("SELECT NULL::struct(a INT, b TEXT) AS s")
        assert t["s"][0].as_py() is None

    def test_all_null_fields(self, run_query) -> None:
        t = run_query("SELECT struct(NULL, NULL)::struct(a INT, b TEXT) AS s")
        assert t["s"][0].as_py() == {"a": None, "b": None}

    def test_keyword_field_names(self, run_query) -> None:
        # `order` / `group` / `inner` are reserved words; they must survive as
        # quoted struct field names in both the type and the Arrow field names.
        t = run_query(
            """SELECT struct(1, 'g', 2)::struct("order" INT, "group" TEXT, "inner" INT) AS s"""
        )
        assert [f.name for f in t.schema.field("s").type] == ["order", "group", "inner"]
        assert t["s"][0].as_py() == {"order": 1, "group": "g", "inner": 2}

    def test_unicode_and_extreme_values(self, run_query) -> None:
        t = run_query(
            "SELECT struct(-2147483648, 'unicode: héllo')::struct(a INT, b TEXT) AS s"
        )
        assert t["s"][0].as_py() == {"a": -2147483648, "b": "unicode: héllo"}


class TestStructRetrievalExpressions:
    """Struct field access, unnest, filtering and aggregation — the paths where
    struct columns feed SQL expressions rather than being echoed back."""

    def test_field_access(self, run_query) -> None:
        t = run_query(
            "SELECT s.a AS a, s.child.z AS z FROM "
            "(SELECT struct(1, struct(2, 'deep')::struct(y INT, z TEXT))"
            "::struct(a INT, child STRUCT(y INT, z TEXT)) AS s) t"
        )
        assert t["a"][0].as_py() == 1
        assert t["z"][0].as_py() == "deep"

    def test_array_of_struct_element_field_access(self, run_query) -> None:
        t = run_query(
            "SELECT arr[1].k AS k, array_length(arr) AS n FROM "
            "(SELECT [struct(7, 'a')::struct(k INT, v TEXT), struct(8, 'b')::struct(k INT, v TEXT)] AS arr) t"
        )
        assert t["k"][0].as_py() == 7
        assert t["n"][0].as_py() == 2

    def test_unnest_array_of_structs(self, run_query) -> None:
        t = run_query(
            "SELECT e.k AS k, e.v AS v FROM "
            "(SELECT [struct(1, 'a')::struct(k INT, v TEXT), struct(2, 'b')::struct(k INT, v TEXT)] AS arr) t, "
            "unnest(t.arr) AS e ORDER BY k"
        )
        assert t["k"].to_pylist() == [1, 2]
        assert t["v"].to_pylist() == ["a", "b"]

    def test_aggregate_and_filter_over_struct_fields(
        self, run_query, scratch_table
    ) -> None:
        run_query(f"CREATE TABLE {scratch_table} (id INT, s STRUCT(a INT, b TEXT))")
        run_query(
            f"INSERT INTO {scratch_table} VALUES "
            "(1, struct(10, 'x')::struct(a INT, b TEXT)), "
            "(2, struct(NULL, 'y')::struct(a INT, b TEXT)), "
            "(3, NULL::struct(a INT, b TEXT))"
        )
        agg = run_query(
            f"SELECT count(*) AS n, count(s.a) AS n_a, sum(s.a) AS total FROM {scratch_table}"
        )
        assert agg["n"][0].as_py() == 3
        assert agg["n_a"][0].as_py() == 1
        assert agg["total"][0].as_py() == 10
        filtered = run_query(
            f"SELECT id FROM {scratch_table} WHERE s.a > 0 ORDER BY id"
        )
        assert filtered["id"].to_pylist() == [1]

    def test_struct_column_survives_multi_chunk_stream(self, run_query) -> None:
        # A large result is split across several Arrow record batches (one chunk
        # each after read_all); the nested child arrays have to be stitched back
        # per chunk.  The engine batches at ~65k rows for a narrow struct.
        n = 70_000
        t = run_query(
            "SELECT struct(i, 'v')::struct(a BIGINT, b TEXT) AS s "
            f"FROM generate_series(1, {n}) AS g(i)"
        )
        assert t.num_rows == n
        assert t["s"].num_chunks > 1, (
            "expected the result to span several record batches"
        )
        assert t["s"][0].as_py() == {"a": 1, "b": "v"}
        assert t["s"][n - 1].as_py() == {"a": n, "b": "v"}


class TestStructTableRoundtrip:
    """CREATE TABLE with STRUCT columns, INSERT ... VALUES, read back — the
    all-SQL round trip, independent of the ingest path."""

    def test_flat_and_nested_roundtrip(self, run_query, scratch_table) -> None:
        run_query(
            f"""CREATE TABLE {scratch_table} (
                    id       INT,
                    s        STRUCT(a INT, b TEXT),
                    s_nested STRUCT(x INT, child STRUCT(y INT, z TEXT))
                )"""
        )
        run_query(
            f"INSERT INTO {scratch_table} VALUES "
            "(1, struct(1, 'x')::struct(a INT, b TEXT), "
            "    struct(10, struct(20, 'y')::struct(y INT, z TEXT))"
            "      ::struct(x INT, child STRUCT(y INT, z TEXT))), "
            "(2, NULL::struct(a INT, b TEXT), "
            "    struct(30, NULL::struct(y INT, z TEXT))"
            "      ::struct(x INT, child STRUCT(y INT, z TEXT)))"
        )
        t = run_query(f"SELECT id, s, s_nested FROM {scratch_table} ORDER BY id")
        assert t.to_pylist() == [
            {
                "id": 1,
                "s": {"a": 1, "b": "x"},
                "s_nested": {"x": 10, "child": {"y": 20, "z": "y"}},
            },
            {"id": 2, "s": None, "s_nested": {"x": 30, "child": None}},
        ]

    def test_alternating_nesting_roundtrip(self, run_query, scratch_table) -> None:
        # ARRAY(STRUCT(a ARRAY(STRUCT(...)), b STRUCT(ARRAY(...)))) — struct and
        # array alternate in both orders within one column.
        run_query(
            f"""CREATE TABLE {scratch_table} (
                    id   INT,
                    deep ARRAY(STRUCT(a ARRAY(STRUCT(leaf INT)), b STRUCT(xs ARRAY(INT))))
                )"""
        )
        run_query(
            f"""INSERT INTO {scratch_table} VALUES (1, [
                    struct([struct(1)::struct(leaf INT), struct(NULL)::struct(leaf INT)],
                           struct([1, 2])::struct(xs ARRAY(INT)))
                      ::struct(a ARRAY(STRUCT(leaf INT)), b STRUCT(xs ARRAY(INT)))])"""
        )
        t = run_query(f"SELECT deep FROM {scratch_table}")
        assert t["deep"][0].as_py() == [
            {"a": [{"leaf": 1}, {"leaf": None}], "b": {"xs": [1, 2]}}
        ]


# --------------------------------------------------------------------------- #
# Insertion: Arrow struct → read_arrow()                                      #
# --------------------------------------------------------------------------- #

# (arrow type, per-row values, expected Firebolt column type) for one nested
# column, ingested with create mode alongside an `id` column.
_SHAPE_CASES = [
    pytest.param(
        _FLAT,
        [{"a": 1, "b": "x"}, {"a": None, "b": None}, None],
        'STRUCT("a" INTEGER, "b" TEXT)',
        id="flat_struct",
    ),
    pytest.param(
        _NESTED,
        [{"x": 10, "child": {"y": 2**40, "z": "deep"}}, {"x": 20, "child": None}, None],
        'STRUCT("x" INTEGER, "child" STRUCT("y" BIGINT, "z" TEXT))',
        id="nested_struct",
    ),
    pytest.param(
        pa.struct([("xs", pa.list_(pa.int32())), ("name", pa.string())]),
        [
            {"xs": [1, 2, 3], "name": "w1"},
            {"xs": [], "name": None},
            {"xs": None, "name": "w3"},
        ],
        'STRUCT("xs" ARRAY(INTEGER), "name" TEXT)',
        id="struct_with_array",
    ),
    pytest.param(
        _ARR_STRUCT,
        [[{"k": 1, "v": "a"}, {"k": 2, "v": "b"}], [], [None, {"k": 4, "v": None}]],
        'ARRAY(STRUCT("k" INTEGER, "v" TEXT))',
        id="array_of_structs",
    ),
    pytest.param(
        pa.struct(
            [("items", pa.list_(pa.struct([("k", pa.int32())]))), ("n", pa.int32())]
        ),
        [
            {"items": [{"k": 1}, {"k": 2}], "n": 2},
            {"items": [], "n": 0},
            {"items": None, "n": None},
        ],
        'STRUCT("items" ARRAY(STRUCT("k" INTEGER)), "n" INTEGER)',
        id="struct_of_array_of_structs",
    ),
    pytest.param(
        pa.list_(pa.list_(pa.struct([("m", pa.int32())]))),
        [[[{"m": 1}, {"m": 2}], [{"m": 3}]], [[], None, [None, {"m": 4}]], None],
        'ARRAY(ARRAY(STRUCT("m" INTEGER)))',
        id="array_of_array_of_structs",
    ),
    pytest.param(
        pa.list_(pa.list_(pa.list_(pa.struct([("d", pa.int32())])))),
        [[[[{"d": 1}]], [[{"d": 2}], []]], [[], None], None],
        'ARRAY(ARRAY(ARRAY(STRUCT("d" INTEGER))))',
        id="three_array_levels_around_a_struct",
    ),
    pytest.param(
        _ALTERNATING,
        [
            [
                {"a": [{"leaf": 1}, {"leaf": None}], "b": {"xs": [1, 2]}},
                {"a": [], "b": {"xs": None}},
            ],
            [None, {"a": None, "b": None}],
            None,
        ],
        'ARRAY(STRUCT("a" ARRAY(STRUCT("leaf" INTEGER)), "b" STRUCT("xs" ARRAY(INTEGER))))',
        id="alternating_nesting",
    ),
    pytest.param(
        # LARGE_LIST uses 64-bit offsets and maps to the same ARRAY type.
        pa.large_list(pa.struct([("k", pa.int32())])),
        [[{"k": 1}], [], None],
        'ARRAY(STRUCT("k" INTEGER))',
        id="large_list_of_structs",
    ),
    pytest.param(
        # Reserved words as field names must be emitted quoted, or the type
        # round-trip fails on the server.
        pa.struct(
            [("order", pa.int32()), ("group", pa.string()), ("inner", pa.int32())]
        ),
        [
            {"order": 1, "group": "g", "inner": 2},
            {"order": None, "group": "", "inner": None},
            None,
        ],
        'STRUCT("order" INTEGER, "group" TEXT, "inner" INTEGER)',
        id="keyword_field_names",
    ),
]


class TestStructIngestShapes:
    """create-mode ingest per shape: the synthesised DDL has to be accepted by
    the server and the data has to round-trip value-for-value."""

    @pytest.mark.parametrize("arrow_type, values, expected_type", _SHAPE_CASES)
    def test_shape_round_trips(
        self, ingest, run_query, scratch_table, arrow_type, values, expected_type
    ) -> None:
        tbl = pa.table(
            {
                "id": pa.array(range(len(values)), type=pa.int32()),
                "s": pa.array(values, type=arrow_type),
            }
        )
        ingest(scratch_table, tbl)
        got = run_query(f"SELECT id, s FROM {scratch_table} ORDER BY id")
        assert got.to_pylist() == tbl.to_pylist()
        assert _column_types(run_query, scratch_table)["s"] == expected_type

    def test_every_scalar_type_as_a_struct_field(
        self, ingest, run_query, scratch_table
    ) -> None:
        typ = pa.struct(
            [
                ("f_int8", pa.int8()),
                ("f_int16", pa.int16()),
                ("f_int32", pa.int32()),
                ("f_int64", pa.int64()),
                ("f_uint8", pa.uint8()),
                ("f_uint16", pa.uint16()),
                ("f_uint32", pa.uint32()),
                ("f_float32", pa.float32()),
                ("f_float64", pa.float64()),
                ("f_bool", pa.bool_()),
                ("f_string", pa.string()),
                ("f_large_string", pa.large_string()),
                ("f_binary", pa.binary()),
                ("f_date", pa.date32()),
                ("f_ts_ntz", pa.timestamp("us")),
                ("f_ts_tz", pa.timestamp("us", tz="UTC")),
                ("f_decimal", pa.decimal128(10, 3)),
            ]
        )
        row = {
            "f_int8": 1,
            "f_int16": 100,
            "f_int32": 1000,
            "f_int64": 10**9,
            "f_uint8": 1,
            "f_uint16": 100,
            "f_uint32": 1000,
            "f_float32": 1.5,
            "f_float64": 3.25,
            "f_bool": True,
            "f_string": "str",
            "f_large_string": "lstr",
            "f_binary": b"\x01\x02",
            "f_date": datetime.date(2020, 1, 2),
            "f_ts_ntz": datetime.datetime(2020, 1, 1, 1, 0, 0),  # noqa: DTZ001 -- tz-naive column
            "f_ts_tz": datetime.datetime(
                2020, 1, 1, 1, 0, 0, tzinfo=datetime.timezone.utc
            ),
            "f_decimal": Decimal("1.001"),
        }
        all_null = {name: None for name in row}
        tbl = pa.table(
            {
                "id": pa.array([1, 2, 3], type=pa.int32()),
                "s": pa.array([row, all_null, None], type=typ),
            }
        )
        ingest(scratch_table, tbl)
        got = run_query(f"SELECT id, s FROM {scratch_table} ORDER BY id")
        assert got["s"][0].as_py() == row
        assert got["s"][1].as_py() == all_null
        assert got["s"][2].as_py() is None
        # The narrow ints widen (Firebolt has no INT8/INT16), unsigned ints widen a
        # size, and the tz-aware timestamp keeps its zone.  The per-type mapping
        # itself is pinned by ArrowToFireboltTypeTest.
        column_type = _column_types(run_query, scratch_table)["s"]
        for expected in (
            '"f_int8" INTEGER',
            '"f_uint32" BIGINT',
            '"f_ts_tz" TIMESTAMPTZ',
        ):
            assert expected in column_type

    def test_multi_batch_stream(self, ingest, run_query, scratch_table) -> None:
        # Several record batches in one IPC stream: the server has to stitch the
        # nested child arrays per batch, not just the first one.
        schema = pa.schema([("id", pa.int32()), ("s", _FLAT), ("arr", _ARR_STRUCT)])
        batches = [
            pa.record_batch(
                {
                    "id": pa.array([i], type=pa.int32()),
                    "s": pa.array([{"a": i, "b": f"b{i}"}], type=_FLAT),
                    "arr": pa.array([[{"k": i, "v": f"v{i}"}]], type=_ARR_STRUCT),
                },
                schema=schema,
            )
            for i in range(1, 4)
        ]
        ingest(scratch_table, pa.RecordBatchReader.from_batches(schema, batches))
        got = run_query(f"SELECT id, s, arr FROM {scratch_table} ORDER BY id")
        assert got["id"].to_pylist() == [1, 2, 3]
        assert got["s"].to_pylist() == [{"a": i, "b": f"b{i}"} for i in (1, 2, 3)]
        assert got["arr"].to_pylist() == [[{"k": i, "v": f"v{i}"}] for i in (1, 2, 3)]

    def test_many_rows(self, ingest, run_query, scratch_table) -> None:
        # A multi-row nested payload through the multipart upload, read back as
        # aggregates over struct fields.
        n = 5_000
        tbl = pa.table(
            {
                "id": pa.array(range(n), type=pa.int32()),
                "s": pa.array(
                    [{"a": i, "b": None if i % 3 == 0 else f"b{i}"} for i in range(n)],
                    type=_FLAT,
                ),
            }
        )
        ingest(scratch_table, tbl)
        got = run_query(
            f"SELECT count(*) AS n, count(s.b) AS n_b, sum(s.a) AS total FROM {scratch_table}"
        )
        assert got["n"][0].as_py() == n
        assert got["n_b"][0].as_py() == n - len(range(0, n, 3))
        assert got["total"][0].as_py() == n * (n - 1) // 2


class TestStructIngestNulls:
    """NULLs at every level of the nesting.  ``IS NULL`` alone cannot separate a
    NULL array from an empty array from an array holding a NULL element, so the
    array levels are probed as ``(IS NULL, array_length, element IS NULL)``."""

    @pytest.fixture
    def nulls_table(self, ingest, run_query, scratch_table):
        rows = [
            # id, arr value — one perturbation per row, so a regression pins down
            # a single level.
            (1, [{"a": [{"leaf": 1}], "b": {"xs": [1]}}]),  # fully populated
            (2, None),  # NULL array
            (3, []),  # empty array
            (4, [None]),  # NULL struct element
            (5, [{"a": None, "b": {"xs": [1]}}]),  # NULL array field
            (6, [{"a": [], "b": {"xs": [1]}}]),  # empty array field
            (7, [{"a": [None], "b": {"xs": [1]}}]),  # NULL struct in inner array
            (8, [{"a": [{"leaf": None}], "b": {"xs": [1]}}]),  # NULL leaf field
            (9, [{"a": [{"leaf": 1}], "b": None}]),  # NULL nested struct
            (
                10,
                [{"a": [{"leaf": 1}], "b": {"xs": None}}],
            ),  # NULL array in nested struct
            (
                11,
                [{"a": [{"leaf": 1}], "b": {"xs": [None]}}],
            ),  # NULL element in that array
        ]
        tbl = pa.table(
            {
                "id": pa.array([r[0] for r in rows], type=pa.int32()),
                "deep": pa.array([r[1] for r in rows], type=_ALTERNATING),
            }
        )
        ingest(scratch_table, tbl)
        return scratch_table, tbl

    def test_values_round_trip_unchanged(self, run_query, nulls_table) -> None:
        table, tbl = nulls_table
        got = run_query(f"SELECT id, deep FROM {table} ORDER BY id")
        assert got.to_pylist() == tbl.to_pylist()

    def test_null_markers_at_every_level(self, run_query, nulls_table) -> None:
        # Read back through SQL rather than as values: this is what distinguishes
        # a NULL array (length NULL) from an empty one (length 0) at each level.
        table, _ = nulls_table
        got = run_query(
            f"""SELECT id,
                       deep IS NULL              AS d_null,
                       array_length(deep)        AS d_len,
                       deep[1] IS NULL           AS e_null,
                       deep[1].a IS NULL         AS a_null,
                       array_length(deep[1].a)   AS a_len,
                       deep[1].a[1] IS NULL      AS a1_null,
                       deep[1].a[1].leaf IS NULL AS leaf_null,
                       deep[1].b IS NULL         AS b_null,
                       deep[1].b.xs IS NULL      AS xs_null,
                       array_length(deep[1].b.xs) AS xs_len,
                       deep[1].b.xs[1] IS NULL   AS xs1_null
                FROM {table} ORDER BY id"""
        )
        by_id = {row["id"]: row for row in got.to_pylist()}

        # The top-level array: NULL / empty / present-with-NULL-element.
        assert (by_id[2]["d_null"], by_id[2]["d_len"]) == (True, None)
        assert (by_id[3]["d_null"], by_id[3]["d_len"]) == (False, 0)
        assert (by_id[4]["d_len"], by_id[4]["e_null"]) == (1, True)
        # The inner array of structs — NULL / empty / NULL element / NULL leaf
        # field, one per row.
        assert (by_id[5]["a_null"], by_id[5]["a_len"]) == (True, None)
        assert (by_id[6]["a_null"], by_id[6]["a_len"]) == (False, 0)
        assert (by_id[7]["a_len"], by_id[7]["a1_null"]) == (1, True)
        assert (by_id[8]["a1_null"], by_id[8]["leaf_null"]) == (False, True)
        # The nested struct and the array inside it.
        assert by_id[9]["b_null"] is True
        assert (by_id[10]["b_null"], by_id[10]["xs_null"], by_id[10]["xs_len"]) == (
            False,
            True,
            None,
        )
        assert (by_id[11]["xs_len"], by_id[11]["xs1_null"]) == (1, True)


class TestStructIngestModes:
    """The ingest modes other than plain create, with struct payloads."""

    def test_append_into_existing_struct_table(
        self, ingest, run_query, scratch_table
    ) -> None:
        # append synthesises no DDL — the nested types come from the target table.
        run_query(
            f"CREATE TABLE {scratch_table} (id INT, s STRUCT(a INT, b TEXT), arr ARRAY(STRUCT(k INT, v TEXT)))"
        )
        run_query(
            f"INSERT INTO {scratch_table} VALUES "
            "(0, struct(0, 'pre')::struct(a INT, b TEXT), "
            "    [struct(0, 'pre')::struct(k INT, v TEXT)])"
        )
        tbl = pa.table(
            {
                "id": pa.array([1, 2], type=pa.int32()),
                "s": pa.array([{"a": 1, "b": "x"}, None], type=_FLAT),
                "arr": pa.array([[{"k": 1, "v": "a"}], []], type=_ARR_STRUCT),
            }
        )
        ingest(scratch_table, tbl, "adbc.ingest.mode.append")
        got = run_query(f"SELECT id, s, arr FROM {scratch_table} ORDER BY id")
        assert got["id"].to_pylist() == [0, 1, 2]
        assert got["s"].to_pylist() == [{"a": 0, "b": "pre"}, {"a": 1, "b": "x"}, None]
        assert got["arr"].to_pylist() == [
            [{"k": 0, "v": "pre"}],
            [{"k": 1, "v": "a"}],
            [],
        ]

    def test_replace_swaps_a_struct_schema(
        self, ingest, run_query, scratch_table
    ) -> None:
        run_query(f"CREATE TABLE {scratch_table} (id INT)")
        run_query(f"INSERT INTO {scratch_table} VALUES (99)")
        tbl = pa.table({"s": pa.array([{"x": 1, "child": None}], type=_NESTED)})
        ingest(scratch_table, tbl, "adbc.ingest.mode.replace")
        got = run_query(f"SELECT s FROM {scratch_table}")
        assert got.column_names == ["s"]
        assert got["s"][0].as_py() == {"x": 1, "child": None}

    def test_zero_row_create_produces_the_struct_schema(
        self, ingest, run_query, scratch_table
    ) -> None:
        # Only the IPC schema message reaches the server; the table still has to
        # come out with the full nested type.
        tbl = pa.table(
            {"s": pa.array([], type=_NESTED), "arr": pa.array([], type=_ARR_STRUCT)}
        )
        ingest(scratch_table, tbl)
        got = run_query(f"SELECT s, arr FROM {scratch_table}")
        assert got.num_rows == 0
        assert got.schema.field("s").type == _NESTED
        assert got.schema.field("arr").type == _ARR_STRUCT

    def test_public_api_ingest(self, cursor, run_query, scratch_table) -> None:
        # The same payload through Cursor.adbc_ingest(), the path ADBC clients
        # actually use.  A pa.Table takes the wrapper's bind_stream path.
        tbl = pa.table(
            {
                "id": pa.array([1, 2], type=pa.int32()),
                "s": pa.array(
                    [{"x": 1, "child": {"y": 2, "z": "z"}}, None], type=_NESTED
                ),
                "arr": pa.array([[{"k": 1, "v": "a"}], None], type=_ARR_STRUCT),
            }
        )
        cursor.adbc_ingest(scratch_table, tbl, mode="create")
        got = run_query(f"SELECT id, s, arr FROM {scratch_table} ORDER BY id")
        assert got.to_pylist() == tbl.to_pylist()

    def test_public_api_ingest_record_batch(
        self, cursor, run_query, scratch_table
    ) -> None:
        # A RecordBatch takes the bind (not bind_stream) path inside the wrapper.
        batch = pa.record_batch(
            {
                "s": pa.array([{"a": 1, "b": "x"}], type=_FLAT),
                "arr": pa.array([[{"k": 1, "v": "a"}]], type=_ARR_STRUCT),
            }
        )
        cursor.adbc_ingest(scratch_table, batch, mode="create")
        got = run_query(f"SELECT s, arr FROM {scratch_table}")
        assert got["s"][0].as_py() == {"a": 1, "b": "x"}
        assert got["arr"][0].as_py() == [{"k": 1, "v": "a"}]


class TestStructIngestNullability:
    """Firebolt requires STRUCT fields to be nullable, while a column may be
    NOT NULL.  The two levels must not be conflated when the DDL is built."""

    def test_non_nullable_struct_field_is_widened(
        self, ingest, run_query, scratch_table
    ) -> None:
        # Emitting `STRUCT("a" INT NOT NULL)` is rejected by the server with
        # "STRUCT fields have to be nullable", so the field nullability is
        # dropped and the ingest succeeds.
        typ = pa.struct([pa.field("a", pa.int32(), nullable=False), ("b", pa.string())])
        tbl = pa.table({"s": pa.array([{"a": 1, "b": "x"}], type=typ)})
        ingest(scratch_table, tbl)
        assert run_query(f"SELECT s FROM {scratch_table}")["s"][0].as_py() == {
            "a": 1,
            "b": "x",
        }
        assert (
            _column_types(run_query, scratch_table)["s"]
            == 'STRUCT("a" INTEGER, "b" TEXT)'
        )

    def test_non_nullable_fields_deep_in_the_nesting_are_widened(
        self, ingest, run_query, scratch_table
    ) -> None:
        inner = pa.struct([pa.field("leaf", pa.int32(), nullable=False)])
        typ = pa.list_(
            pa.field(
                "item",
                pa.struct([pa.field("child", inner, nullable=False)]),
                nullable=False,
            )
        )
        tbl = pa.table({"arr": pa.array([[{"child": {"leaf": 1}}]], type=typ)})
        ingest(scratch_table, tbl)
        assert run_query(f"SELECT arr FROM {scratch_table}")["arr"][0].as_py() == [
            {"child": {"leaf": 1}}
        ]
        assert _column_types(run_query, scratch_table)["arr"] == (
            'ARRAY(STRUCT("child" STRUCT("leaf" INTEGER)))'
        )

    def test_non_nullable_struct_column_stays_not_null(
        self, ingest, run_query, scratch_table
    ) -> None:
        typ = pa.struct([("a", pa.int32())])
        tbl = pa.table(
            {"s": pa.array([{"a": 1}], type=typ)},
            schema=pa.schema([pa.field("s", typ, nullable=False)]),
        )
        ingest(scratch_table, tbl)
        got = run_query(
            "SELECT is_nullable FROM information_schema.columns "
            f"WHERE table_name = '{scratch_table}' AND column_name = 's'"
        )
        assert got["is_nullable"][0].as_py() == "NO"


class TestStructIngestUnsupported:
    """A struct shape the driver cannot map must fail before anything is
    uploaded.  Which shapes map is unit-tested; what matters here is that the
    failure reaches the caller as NotSupportedError and leaves no table
    behind."""

    def test_zero_field_struct_rejected(self, ingest, run_query, table_name) -> None:
        # Firebolt has no zero-field STRUCT; "STRUCT()" would be a syntax error.
        tbl = pa.table({"s": pa.array([{}], type=pa.struct([]))})
        with pytest.raises(
            (adbc_driver_manager.NotSupportedError, dbapi.NotSupportedError),
            match="cannot be mapped",
        ):
            ingest(table_name, tbl)
        # Nothing was uploaded, so the table was never created either.
        assert _column_types(run_query, table_name) == {}
