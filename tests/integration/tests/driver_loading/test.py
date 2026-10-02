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

"""How a driver manager finds the driver's entry point.

The Foundry naming rule derives the entry point from the driver name, so a
manager asked for the Firebolt driver looks up `AdbcDriverFireboltInit`; the
generic `AdbcDriverInit` stays as the fallback. Neither needs a running engine:
opening a database and a connection sends no request.
"""

import adbc_driver_manager
import pytest

from conftest import ADBC_DRIVER_PATH


@pytest.mark.parametrize("entrypoint", ["AdbcDriverFireboltInit", "AdbcDriverInit"])
def test_entrypoint_loads(mock_server, entrypoint):
    with adbc_driver_manager.AdbcDatabase(
        driver=ADBC_DRIVER_PATH, entrypoint=entrypoint, uri=mock_server.url
    ) as db:
        with adbc_driver_manager.AdbcConnection(db):
            pass


def test_non_adbc_entrypoint_is_not_exported(mock_server):
    with pytest.raises(adbc_driver_manager.Error, match="FireboltAdbcDriverInit"):
        adbc_driver_manager.AdbcDatabase(
            driver=ADBC_DRIVER_PATH, entrypoint="FireboltAdbcDriverInit", uri=mock_server.url
        )
