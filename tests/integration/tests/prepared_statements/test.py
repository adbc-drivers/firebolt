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

"""Prepared statement tests.

Firebolt has no server-side prepare: a parameterised statement is sent whole every
time, with `$N` resolved from the `query_parameters` setting.  `AdbcStatementPrepare`
therefore issues no request, and the one round-trip a prepared statement needs —
the placeholder types — happens in `AdbcStatementGetParameterSchema` via
`execution_mode=describe_parameters`.  The mock-server tests below pin that budget.
"""

import urllib.parse

import adbc_driver_manager
import pyarrow as pa
import pytest


def _query_param(captured_path: str, name: str) -> list[str]:
    """Return the values of `name` in the URL of a captured request, in order."""
    parsed = urllib.parse.urlparse(captured_path)
    pairs = urllib.parse.parse_qsl(parsed.query, keep_blank_values=True)
    return [v for (k, v) in pairs if k == name]


class TestParameterSchema:
    def test_schema_of_a_single_parameter(self, dbapi_conn, temp_table) -> None:
        with dbapi_conn.cursor() as cur:
            schema = cur.adbc_prepare(f"SELECT id FROM {temp_table} WHERE id = $1")
        assert schema is not None
        assert len(schema) == 1
        assert schema.field(0).name == "$1"
        # Inferred from the INT column it is compared against.
        assert pa.types.is_integer(schema.field(0).type)

    def test_schema_of_several_parameters(self, dbapi_conn, temp_table) -> None:
        with dbapi_conn.cursor() as cur:
            schema = cur.adbc_prepare(f"SELECT id FROM {temp_table} WHERE id = $1 AND label = $2")
        assert [field.name for field in schema] == ["$1", "$2"]
        assert pa.types.is_integer(schema.field(0).type)
        assert pa.types.is_string(schema.field(1).type)

    def test_parameters_are_ordered_by_ordinal_position(self, dbapi_conn) -> None:
        # Past nine, the parameter names sorted as text ("$10" < "$2") disagree with
        # the placeholders they describe.
        select = ", ".join(f"${i}" for i in range(1, 13))
        with dbapi_conn.cursor() as cur:
            schema = cur.adbc_prepare(f"SELECT {select}")
        assert [field.name for field in schema] == [f"${i}" for i in range(1, 13)]

    def test_statement_without_parameters_has_an_empty_schema(self, dbapi_conn) -> None:
        with dbapi_conn.cursor() as cur:
            schema = cur.adbc_prepare("SELECT 1")
        assert schema is not None
        assert len(schema) == 0

    def test_invalid_sql_is_reported(self, dbapi_conn) -> None:
        with dbapi_conn.cursor() as cur:
            with pytest.raises(Exception):
                cur.adbc_prepare("SELECT FROM WHERE $1")

    def test_prepare_then_execute(self, dbapi_conn, temp_table) -> None:
        with dbapi_conn.cursor() as cur:
            cur.execute(f"INSERT INTO {temp_table} VALUES (1, 'alice', 1.0)")
            schema = cur.adbc_prepare(f"SELECT label FROM {temp_table} WHERE id = $1")
            assert len(schema) == 1
            cur.execute(f"SELECT label FROM {temp_table} WHERE id = $1", (1,))
            assert cur.fetchone()[0] == "alice"


class TestRequestBudget:
    """What actually goes over the wire, measured against the mock server."""

    def test_execute_with_parameters_sends_one_request(self, mock_server, conn_to_mock) -> None:
        with adbc_driver_manager.AdbcStatement(conn_to_mock) as stmt:
            stmt.set_sql_query("SELECT $1")
            stmt.prepare()
            stmt.bind(pa.record_batch([[41]], names=["0"]))
            try:
                stmt.execute_query()
            except Exception:
                # The mock answers 200 with an empty body, so the driver reports an
                # Arrow parse error.  Only the captured request matters here.
                pass

        assert len(mock_server.captured) == 1, "Prepare must not add a round-trip"
        path = mock_server.captured[0].path
        assert _query_param(path, "query_parameters") == ['[{"name":"$1","value":41}]']
        assert _query_param(path, "execution_mode") == []
        assert mock_server.captured[0].body == b"SELECT $1"

    def test_prepare_alone_sends_nothing(self, mock_server, conn_to_mock) -> None:
        with adbc_driver_manager.AdbcStatement(conn_to_mock) as stmt:
            stmt.set_sql_query("SELECT $1")
            stmt.prepare()
        assert mock_server.captured == []

    def test_get_parameter_schema_asks_the_server_every_time(self, mock_server, conn_to_mock) -> None:
        """The answer depends on the objects the statement names, so DDL changes it
        while the text stays the same, and no event the driver sees marks that moment
        (set_sql_query is re-issued only on a text change).  So: no cache — the second
        describe reports a different type and the caller must see it."""
        mock_server.queue(
            status=200, body=_arrow_one_string_cell(b'{"result_columns":[],"parameter_types":{"$1":"integer"}}')
        )
        mock_server.queue(status=200, body=_arrow_one_string_cell(b'{"result_columns":[],"parameter_types":{"$1":"text"}}'))

        with adbc_driver_manager.AdbcStatement(conn_to_mock) as stmt:
            stmt.set_sql_query("SELECT $1")
            stmt.prepare()
            first = pa.schema(stmt.get_parameter_schema())
            second = pa.schema(stmt.get_parameter_schema())

        assert first.field(0).type == pa.int32()
        assert second.field(0).type == pa.string(), "a stale parameter type was served from a cache"
        assert len(mock_server.captured) == 2
        for request in mock_server.captured:
            assert _query_param(request.path, "execution_mode") == ["describe_parameters"]

    def test_describe_does_not_join_an_open_transaction(self, mock_server, conn_to_mock) -> None:
        """Type inference must not spend a transaction step on a statement the caller
        never ran, so the transaction session parameters are left off — the exclusion
        the server's own PostgreSQL handler makes for Describe."""
        # BEGIN, answered with the session parameters the engine sets for a
        # transaction.
        mock_server.queue(
            status=200,
            body=b"",
            headers=[("Firebolt-Update-Parameters", "transaction_id=tx-1,transaction_sequence_id=7")],
        )
        # The statement itself, then the describe: the queue is consumed in order.
        mock_server.queue(status=200, body=b"")
        describe = b'{"result_columns":[],"parameter_types":{"$1":"integer"}}'
        mock_server.queue(status=200, body=_arrow_one_string_cell(describe))

        conn_to_mock.set_options(**{"adbc.connection.autocommit": "false"})
        with adbc_driver_manager.AdbcStatement(conn_to_mock) as stmt:
            stmt.set_sql_query("SELECT 1")
            try:
                stmt.execute_query()
            except Exception:
                pass

            stmt.set_sql_query("SELECT $1")
            stmt.get_parameter_schema()

        # The BEGIN, the statement, then the describe.
        assert len(mock_server.captured) == 3
        statement_path = mock_server.captured[1].path
        assert _query_param(statement_path, "transaction_id") == ["tx-1"], "the statement must join the transaction"

        describe_path = mock_server.captured[2].path
        assert _query_param(describe_path, "execution_mode") == ["describe_parameters"]
        assert _query_param(describe_path, "transaction_id") == []
        assert _query_param(describe_path, "transaction_sequence_id") == []

    def test_bound_parameters_override_a_session_parameter_of_the_same_name(
        self, mock_server, conn_to_mock
    ) -> None:
        conn_to_mock.set_options(**{"query_parameters": '[{"name":"$1","value":"stale"}]'})
        with adbc_driver_manager.AdbcStatement(conn_to_mock) as stmt:
            stmt.set_sql_query("SELECT $1")
            stmt.bind(pa.record_batch([[41]], names=["0"]))
            try:
                stmt.execute_query()
            except Exception:
                pass

        assert len(mock_server.captured) == 1
        assert _query_param(mock_server.captured[0].path, "query_parameters") == ['[{"name":"$1","value":41}]']

    def test_executemany_sends_one_request_per_parameter_set(self, mock_server, conn_to_mock) -> None:
        with adbc_driver_manager.AdbcStatement(conn_to_mock) as stmt:
            stmt.set_sql_query("INSERT INTO t VALUES ($1)")
            stmt.bind(pa.record_batch([[1, 2, 3]], names=["0"]))
            stmt.execute_update()

        assert len(mock_server.captured) == 3
        sent = [_query_param(request.path, "query_parameters")[0] for request in mock_server.captured]
        assert sent == [
            '[{"name":"$1","value":1}]',
            '[{"name":"$1","value":2}]',
            '[{"name":"$1","value":3}]',
        ]

    def test_execute_query_runs_every_parameter_set(self, mock_server, conn_to_mock) -> None:
        """ADBC and dbapi's execute() both run multi-row Arrow data once per row,
        while always asking for a result set.  So all rows run, and the stream handed
        back is the last execution's — there being only one to hand back."""
        with adbc_driver_manager.AdbcStatement(conn_to_mock) as stmt:
            stmt.set_sql_query("SELECT $1")
            stmt.bind(pa.record_batch([[1, 2]], names=["0"]))
            try:
                stmt.execute_query()
            except Exception:
                # The mock answers 200 with an empty body, so importing the result
                # fails; the requests are what this test is about.
                pass

        assert len(mock_server.captured) == 2
        sent = [_query_param(request.path, "query_parameters")[0] for request in mock_server.captured]
        assert sent == ['[{"name":"$1","value":1}]', '[{"name":"$1","value":2}]']


def _arrow_one_string_cell(value: bytes) -> bytes:
    """Serialise a one-row, one-string-column Arrow IPC stream — the shape the
    server's describe_parameters mode returns."""
    batch = pa.record_batch([pa.array([value.decode()])], names=["describe"])
    sink = pa.BufferOutputStream()
    with pa.ipc.new_stream(sink, batch.schema) as writer:
        writer.write_batch(batch)
    return sink.getvalue().to_pybytes()
