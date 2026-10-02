#!/usr/bin/env python3
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

"""FB2 only: connect to an engine in a Firebolt 2.0 SaaS account and query it.

Uses a service account plus account and engine names.  See docs/fb2-saas.md.

    FIREBOLT_CLIENT_ID=... FIREBOLT_CLIENT_SECRET=... \\
    FIREBOLT_ACCOUNT=my_account FIREBOLT_ENGINE=my_engine FIREBOLT_DATABASE=my_db \\
        python examples/python/fb2_only/quickstart.py
"""

import argparse
import os

from adbc_driver_manager import dbapi

DEFAULT_DRIVER = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "..",
    "..",
    "..",
    "build",
    "libadbc_driver_firebolt.so",
)


def connection_settings() -> tuple[str, dict]:
    """Driver path plus the FB2 database options, read from the environment."""
    driver = os.environ.get("FIREBOLT_ADBC_DRIVER", DEFAULT_DRIVER)
    db_kwargs = {
        "username": os.environ["FIREBOLT_CLIENT_ID"],
        "password": os.environ["FIREBOLT_CLIENT_SECRET"],
        "firebolt.account": os.environ["FIREBOLT_ACCOUNT"],
        "firebolt.engine": os.environ["FIREBOLT_ENGINE"],
    }
    if os.environ.get("FIREBOLT_DATABASE"):
        db_kwargs["firebolt.database"] = os.environ["FIREBOLT_DATABASE"]
    return driver, db_kwargs


def main() -> None:
    argparse.ArgumentParser(description=__doc__.splitlines()[0]).parse_args()

    driver, db_kwargs = connection_settings()
    print(
        f"account: {db_kwargs['firebolt.account']}  engine: {db_kwargs['firebolt.engine']}\n"
    )

    # Opening the connection exchanges the service account for a token and looks
    # up the engine's URL; both are cached for the rest of the process.
    with dbapi.connect(driver=driver, db_kwargs=db_kwargs) as conn:
        with conn.cursor() as cur:
            cur.execute("SELECT current_engine() AS engine, current_database() AS db")
            print(cur.fetch_arrow_table())

            cur.execute("SELECT $1 + 1 AS answer", (41,))
            print(cur.fetchall())


if __name__ == "__main__":
    main()
