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

import adbc_driver_manager.dbapi
import pytest


def test_package() -> None:
    """The installed manifest resolves and loads the packaged driver."""
    # Connecting successfully proves that the installed manifest resolved and
    # the driver manager loaded the packaged shared library. The deliberately
    # unreachable endpoint must then fail while executing through Firebolt's
    # HTTP client, not while loading the driver.
    with (
        adbc_driver_manager.dbapi.connect(
            driver="firebolt",
            db_kwargs={"uri": "http://127.0.0.1:1"},
        ) as conn,
        conn.cursor() as cursor,
        pytest.raises(
            adbc_driver_manager.dbapi.Error,
            match=r"curl error: Could not connect to server",
        ),
    ):
        cursor.execute("SELECT 1")
