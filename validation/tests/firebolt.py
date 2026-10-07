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

import functools
from pathlib import Path

from adbc_drivers_validation import model, quirks


class FireboltQuirks(model.DriverQuirks):
    name = "firebolt"
    driver = "adbc_driver_firebolt"
    driver_name = "ADBC Driver for Firebolt"
    vendor_name = "Firebolt"
    # The driver does not report ADBC_INFO_VENDOR_VERSION.
    vendor_version = ""
    short_version = "5.0.0"
    features = model.DriverFeatures(
        connection_get_table_schema=True,
        connection_transactions=True,
        current_catalog="firebolt",
        current_schema="public",
        get_objects=True,
        statement_bind=True,
        statement_bulk_ingest=True,
        statement_bulk_ingest_catalog=True,
        statement_bulk_ingest_schema=True,
        statement_bulk_ingest_temporary=False,
        statement_execute_schema=False,
        statement_get_parameter_schema=True,
        statement_prepare=True,
        statement_rows_affected=False,
        statement_rows_affected_ddl=False,
        secondary_catalog="firebolt_validation_secondary",
        secondary_catalog_schema="firebolt_validation_secondary",
        secondary_schema="firebolt_validation_secondary",
        supported_xdbc_fields=[],
    )
    setup = model.DriverSetup(
        database={
            "uri": model.FromEnv("FIREBOLT_URI"),
        },
        connection={},
        statement={},
    )

    @property
    def queries_paths(self) -> tuple[Path]:
        return (Path(__file__).parent.parent / "queries",)

    def bind_parameter(self, index: int) -> str:
        return f"${index}"

    def is_table_not_found(self, table_name: str | None, error: Exception) -> bool:
        message = str(error).lower()
        return (
            "does not exist" in message
            or "not found" in message
            or "did not find a table with name" in message
            or "unknown table" in message
        ) and (table_name is None or table_name.lower() in message)

    def split_statement(self, statement: str) -> list[str]:
        return quirks.split_statement(statement)


@functools.cache
def get_quirks(version: str, *, vendor: str = "Firebolt") -> FireboltQuirks:
    firebolt_quirks = FireboltQuirks()
    if version != firebolt_quirks.short_version:
        raise ValueError(f"Unsupported Firebolt version: {version}")
    if vendor not in {firebolt_quirks.name, firebolt_quirks.vendor_name}:
        raise ValueError(f"Unsupported Firebolt vendor: {vendor}")
    return firebolt_quirks
