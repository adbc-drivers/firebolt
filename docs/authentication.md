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

# Authentication

Short version: this driver can talk to an engine with authentication **disabled**
over plaintext HTTP, and it can send a bearer token you obtained elsewhere. It
does not yet implement Firebolt's discovery-based authentication, so it cannot
obtain a token itself. The gaps are listed at the bottom.

## What works today

(For engines in a Firebolt 2.0 SaaS account, which use service accounts and a
different flow, see [fb2-saas.md](fb2-saas.md) — FB2 only.)

### Authentication disabled

The common local and CI setup. Supply no token and the driver sends no
`Authorization` header:

```python
import adbc_driver_manager.dbapi as dbapi

conn = dbapi.connect(
    driver="/path/to/libadbc_driver_firebolt.so",
    db_kwargs={"uri": "http://localhost:3473"},
)
```

A Firebolt engine started with no configuration has authentication disabled, so
this is what you get out of the box locally.

### A bearer token you already have

If your engine accepts a bearer token, pass it as `firebolt.token` and the
driver sends `Authorization: Bearer <token>` on every request:

```python
import os

conn = dbapi.connect(
    driver="/path/to/libadbc_driver_firebolt.so",
    db_kwargs={
        "uri": "http://engine.internal:3473",
        "firebolt.token": os.environ["FIREBOLT_TOKEN"],
    },
)
```

The token can also be set per connection, so two connections sharing one
`AdbcDatabase` can act as different principals — see
[OPTIONS.md](../OPTIONS.md#connection-options).

Two caveats:

- **Use `https://` for a token.** Over a plaintext `http://` endpoint the token is
  exposed on the wire to anything other than a loopback address. Firebolt's SDK
  specification forbids sending a token over an unencrypted transport for exactly
  this reason. The driver does not enforce it yet.
- **`firebolt.token` is temporary.** The specification has no
  connection-string field for a raw JWT; it comes from the environment instead.
  This option will be removed when the driver reads `FIREBOLT_TOKEN` itself.

### Getting a token

The `firebolt` CLI mints one for a given engine and prints it on stdout:

```bash
firebolt login <engine-url>          # once, interactively
export FIREBOLT_TOKEN="$(firebolt token <engine-url>)"
```

`firebolt token` is designed as a credentials process for exactly this purpose,
with stable exit codes (`0` ok, `2` retriable, `3` re-authentication required).

## What Firebolt's model actually is

Worth reading before you wire up anything permanent, because it is not the model
older Firebolt SDKs used.

The rule is that **a client must not hardcode or assume anything about how to
authenticate**. Everything is discovered from the engine at connect time:

1. **Discovery.** `GET <host>/.well-known/firebolt`, unauthenticated. The
   response's `instance.auth` is either `null` — authentication is disabled, send
   no header — or it carries `oauth.protectedResource` (including the RFC 8707
   `resource` identifier for this instance) plus a list of
   `authorizationServers[]` and an optional `preferredAuthorizationServer`.
2. **Pick an authorization server.** An explicit `authorization_server=<name>`
   wins; otherwise `preferredAuthorizationServer`; otherwise the only one
   advertised. The embedded server has the reserved name `_local`.
3. **Get a token.** The only credential exchange a client implements is the OAuth
   2.0 `client_credentials` grant against that server's `token_endpoint`, mapping
   `username` → `client_id` and `password` → `client_secret`, and sending
   `resource=<protectedResource.resource>` so the token is bound to this
   instance.
4. **Query.** `Authorization: Bearer <token>`, with the engine and database
   conveyed as query parameters. On a `401`, drop the cached token, get one fresh
   token, retry once.

Token sources, highest precedence first: the `FIREBOLT_TOKEN` environment
variable (one-shot, never cached, no retry on `401`), then credentials on the
connection, then — optionally — shelling out to `firebolt token <host>`.

Transport is a separate `ssl_mode` parameter rather than part of the scheme:
`verify-full` (the default), `verify-ca`, `require`, `disable`. A token must
never be sent when `ssl_mode=disable`.

### Things that no longer exist

If you have used a Firebolt SDK before, these are gone. They belong to the
control-plane resolution model and must not appear in the new connection format:

- service account IDs and secrets as a distinct concept, and `logins`
- `account` / `account_name` / `account_id`
- `api_endpoint` / `environment` (the `app` / `staging` selector)
- `engine_url` / `engine_name` resolved through a control-plane API

A connection now identifies an instance by **host alone**. There is no
control-plane call, and no system-engine step that rewrites your endpoint. What
used to be a service account is just a `client_id` / `client_secret` pair passed
as `username` / `password`, against an authorization server the engine tells you
about.

## Gaps in this driver

Tracked separately; none of it is implemented here yet.

| Gap | Consequence |
|-----|-------------|
| `ssl_mode` only inside a `firebolt://` URI | There it takes `verify-full` (the default) or `disable`; `verify-ca` and `require` are refused. With an `http(s)://` `uri`, TLS follows the scheme: `https://` always verifies the peer (`verify-full`), `http://` is plaintext. A token is not refused over plaintext. |
| No `/.well-known/firebolt` discovery | The driver cannot tell whether an engine wants authentication; you have to know. |
| No `client_credentials` grant | No `username` / `password`; you must obtain a token out of band. |
| No `FIREBOLT_TOKEN` support | You have to read the variable yourself and pass `firebolt.token`. |
| No token cache or `401` retry | An expired token surfaces as an error; nothing re-acquires it. |
| No `engine` selector | You cannot choose an engine behind a gateway. |

## References

These live in the [packdb](https://github.com/firebolt-db/packdb) repository and
are authoritative — this page only summarises them.

| Spec | Covers |
|------|--------|
| `specs/sdk-authentication.md` | How SDKs and drivers connect and authenticate. Names ADBC explicitly. |
| `specs/metadata-discovery.md` | The `/.well-known/firebolt` document. |
| `specs/authentication.md` | The server-side model: auth modes, embedded authorization server, token validation, TLS. |
| `specs/firebolt-cli-authentication.md` | The CLI, including `firebolt token` and the `FIREBOLT_TOKEN` override. |
| `specs/schemas/connection-parameters.v1.json` | Canonical connection parameter names, types, and defaults. |
