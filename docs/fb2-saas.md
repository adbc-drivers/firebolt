<!--
Copyright (c) 2026 ADBC Drivers Contributors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# FB2 only: connecting to Firebolt 2.0 SaaS

Everything on this page applies **only to Firebolt 2.0 SaaS** engines. The rest of
the documentation describes the driver's main model and does not cover any of it.

The driver can reach an engine running in a Firebolt 2.0 SaaS account: you give it a
service account, an account name and an engine name, and it obtains a token and finds
the engine's URL for you.

**Requirements**

- The engine must run **Firebolt version 5 or later**. Older 2.0 engines do not serve
  the Arrow results this driver reads, and are refused with a message saying so.
- A **user engine** is required. The account's system engine is not a query target.
- The engine must be **running** when you query it. Connecting works while it is
  stopped, but the first query fails with a message telling you to start it.

## Quickstart

```python
import os
import adbc_driver_manager.dbapi as dbapi

conn = dbapi.connect(
    driver="firebolt",
    db_kwargs={
        "username": os.environ["FIREBOLT_CLIENT_ID"],  # service account ID
        "password": os.environ["FIREBOLT_CLIENT_SECRET"],  # service account secret
        "firebolt.account": "my_account",
        "firebolt.engine": "my_engine",
        "firebolt.database": "my_db",
    },
)
cur = conn.cursor()
cur.execute("SELECT * FROM my_table WHERE id = $1", (42,))
print(cur.fetch_arrow_table())
```

Everything else — Arrow results, query parameters, bulk ingest, transactions,
metadata — works as described in the [main README](../README.md). A runnable version
is [examples/python/fb2_only/quickstart.py](../examples/python/fb2_only/quickstart.py).

Do not pass `uri`. The engine's URL is resolved from the account and engine names,
and setting `uri` together with `firebolt.account` is an error. Setting
`firebolt.account` is what selects this mode; nothing else does.

## Options

All of these are database options (`db_kwargs` in Python), set before the database
is opened.

| Key | Required | Meaning |
|-----|----------|---------|
| `username` or `firebolt.client_id` | yes¹ | Service account ID. `username` is ADBC's standard key, so generic tools can pass it. |
| `password` or `firebolt.client_secret` | yes¹ | Service account secret. Never echoed in an error. |
| `firebolt.account` | yes | Firebolt 2.0 account name. |
| `firebolt.engine` | yes | Engine to run queries on. |
| `firebolt.database` | no | Database name. Checked when you connect, so a misspelt name fails there. |
| `firebolt.token` | no¹ | A token you obtained yourself, used instead of the service account. Never refreshed. |
| `firebolt.cache_connection` | no | `true` (default) or `false`. See [Caching](#caching). |
| `firebolt.timeout_sec` | no | Request timeout in whole seconds, also applied while connecting. `0` (default) disables it. |
| `firebolt.ssl_certificate_path` | no | PEM CA bundle, if the system one does not verify Firebolt's certificates. |

¹ Either the service account (both values) or `firebolt.token`, not both. Each
credential may be set under one of its two names, not both.

System settings are connection options, as for any engine: for example,
`firebolt.session.time_zone=UTC` set on the connection travels with every request.

## How it connects

Opening the database makes up to four requests, then queries go straight to the
engine:

1. Exchange the service account for a token at `https://id.app.firebolt.io/oauth/token`.
2. Look up the account's system engine at
   `https://api.app.firebolt.io/web/v3/account/<account>/engineUrl`.
3. Run `USE DATABASE "<database>"` on the system engine, if a database was given.
4. Run `USE ENGINE "<engine>"` there, which answers with the engine's URL.

A `USE ENGINE "<other>"` you run yourself moves that one connection to the other
engine. The driver only follows such a move within the account's own Firebolt
domain, and fails the statement otherwise.

### Caching

Tokens and resolved engine URLs are cached in memory for the life of the process,
so a second `connect()` with the same settings makes no requests until the first
query. Nothing is written to disk.

- A token is renewed a minute before it expires.
- If the engine answers `401`, the driver fetches a new token and retries the request
  once. A second `401` is returned to you.
- A stopped engine's cached URL is dropped, so the next connect looks it up again.

Set `firebolt.cache_connection=false` to skip both caches, for example if you
suspect a stale entry.

## Errors

| Situation | When | Status | What to do |
|-----------|------|--------|------------|
| A required option is missing, or `uri` is set too | connect | `ADBC_STATUS_INVALID_ARGUMENT` | The message names the option. |
| Service account rejected | connect | `ADBC_STATUS_UNAUTHENTICATED` | Check the ID and secret, and that the service account is enabled. |
| Account not found | connect | `ADBC_STATUS_NOT_FOUND` | Check `firebolt.account`, and that the service account belongs to it. |
| Database or engine not found | connect | `ADBC_STATUS_NOT_FOUND` | The message quotes the server and names the option. |
| Engine stopped | first query | `ADBC_STATUS_INVALID_ARGUMENT` | Start the engine (`START ENGINE "<name>"`, or in the Firebolt UI) and retry. |
| Engine older than version 5 | first query | `ADBC_STATUS_INVALID_ARGUMENT` | Use an engine on Firebolt 5 or later. |
| `401` after the one retry | any query | `ADBC_STATUS_UNAUTHORIZED` | The service account lost access to the engine. |
| Rate limited while connecting | connect | `ADBC_STATUS_IO` | Wait and retry; the message quotes `Retry-After` when the server sent one. |
| A server tried to move the connection outside Firebolt's domain | any query | `ADBC_STATUS_IO` | The connection keeps its endpoint; report it. |

## Not supported

- **Engines older than version 5**, and the system engine as a query target.
- **A URL or connection-string form.** The 2.0 options are key/value only.
- **PrivateLink** control-plane endpoints.
- **Firebolt 3.0 service accounts.** Credentials without `firebolt.account` are
  rejected; see [authentication.md](authentication.md) for the 3.0 model.
