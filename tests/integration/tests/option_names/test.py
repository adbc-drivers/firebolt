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

"""Driver options are named `firebolt.*`, with no `adbc.` prefix.

This goes through adbc_driver_manager rather than the C API directly because the
manager buffers options set before the driver is loaded and replays them inside
Init. The mock server answers any query, so no engine is involved.
"""

import adbc_driver_manager

from conftest import ADBC_DRIVER_PATH


def test_new_names_are_accepted(mock_server):
    with adbc_driver_manager.AdbcDatabase(
        driver=ADBC_DRIVER_PATH,
        uri=mock_server.url,
        **{"firebolt.database": "analytics", "firebolt.timeout_sec": "30", "firebolt.token": "db_token"},
    ) as db:
        with adbc_driver_manager.AdbcConnection(db, **{"firebolt.token": "conn_token"}) as conn:
            with adbc_driver_manager.AdbcStatement(conn) as stmt:
                stmt.set_sql_query("SELECT 1")
                try:
                    stmt.execute_query()
                except adbc_driver_manager.OperationalError:
                    pass  # the mock's empty 200 body is not an Arrow stream; only the request matters

    request = mock_server.last_request
    assert "database=analytics" in request.path
    assert request.headers.get("Authorization") == "Bearer conn_token"

