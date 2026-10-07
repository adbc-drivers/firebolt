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

"""FB2 SaaS (Legacy) mode against a real Firebolt 2.0 account.

FB2 only, opt-in.  Skipped unless these are set (runner.py forwards them into
the test container):

    FIREBOLT_FB2_CLIENT_ID, FIREBOLT_FB2_CLIENT_SECRET   service account
    FIREBOLT_FB2_ACCOUNT, FIREBOLT_FB2_ENGINE            a running v5+ engine
    FIREBOLT_FB2_ENVIRONMENT                             optional, default "app"

Run with: ./tests/integration/runner.py tests/fb2_legacy_live
To run every engine suite against the same engine instead of the local one, also
set FIREBOLT_TEST_TARGET=fb2 (see conftest.py).  The engine must be running; an auto-stopped engine fails every test with the
driver's "start the engine" message.
"""

import os
import uuid

import adbc_driver_manager
import adbc_driver_manager.dbapi
import pyarrow as pa
import pytest
from conftest import ADBC_DRIVER_PATH, FB2_REQUIRED_ENV, fb2_configured, fb2_db_kwargs

pytestmark = pytest.mark.skipif(
    not fb2_configured(), reason="FB2 live suite: set " + ", ".join(FB2_REQUIRED_ENV)
)


def _db_kwargs(**overrides):
    return fb2_db_kwargs(**overrides)


@pytest.fixture
def cursor(fb2_database):
    """A cursor in the session's own database (fb2_database, see conftest.py)."""
    with (
        adbc_driver_manager.dbapi.connect(
            driver=ADBC_DRIVER_PATH,
            db_kwargs=_db_kwargs(**{"firebolt.database": fb2_database}),
        ) as conn,
        conn.cursor() as cur,
    ):
        yield cur


def test_query_runs_on_the_configured_engine(cursor, fb2_database):
    cursor.execute("SELECT current_engine(), current_database()")
    assert cursor.fetchall() == [(os.environ["FIREBOLT_FB2_ENGINE"], fb2_database)]


def test_query_parameters(cursor):
    cursor.execute("SELECT $1 + 1, $2", (41, "text"))
    assert cursor.fetchall() == [(42, "text")]


def test_bulk_ingest_round_trip(cursor):
    table = f"adbc_fb2_live_{uuid.uuid4().hex[:12]}"
    try:
        cursor.adbc_ingest(
            table, pa.table({"a": [1, 2, 3], "s": ["x", "y", "z"]}), mode="create"
        )
        cursor.execute(f'SELECT a, s FROM "{table}" ORDER BY a')
        assert cursor.fetch_arrow_table().to_pydict() == {
            "a": [1, 2, 3],
            "s": ["x", "y", "z"],
        }
    finally:
        cursor.execute(f'DROP TABLE IF EXISTS "{table}"')


def test_use_engine_mid_session_stays_usable(cursor):
    cursor.execute(f'USE ENGINE "{os.environ["FIREBOLT_FB2_ENGINE"]}"')
    cursor.execute("SELECT 1")
    assert cursor.fetchall() == [(1,)]


def test_wrong_secret_is_unauthenticated():
    with pytest.raises(adbc_driver_manager.Error) as exc:
        adbc_driver_manager.dbapi.connect(
            driver=ADBC_DRIVER_PATH, db_kwargs=_db_kwargs(password="fbsec_wrong")
        ).close()
    assert exc.value.status_code == adbc_driver_manager.AdbcStatusCode.UNAUTHENTICATED


def test_unknown_engine_is_not_found():
    with pytest.raises(adbc_driver_manager.Error) as exc:
        adbc_driver_manager.dbapi.connect(
            driver=ADBC_DRIVER_PATH,
            db_kwargs=_db_kwargs(
                **{"firebolt.engine": f"no_such_{uuid.uuid4().hex[:8]}"}
            ),
        ).close()
    assert exc.value.status_code == adbc_driver_manager.AdbcStatusCode.NOT_FOUND
