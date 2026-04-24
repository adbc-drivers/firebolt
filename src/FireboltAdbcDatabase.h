#pragma once

#include <string>

namespace firebolt::adbc
{

struct FireboltDatabase
{
    std::string url; // "uri" — the HTTP query endpoint, e.g. "http://localhost:3473"
    std::string token; // "adbc.firebolt.token" — JWT bearer token (omit for no-auth)
    std::string database; // "adbc.firebolt.database" — database name (optional)
    long timeout_sec = 0; // "adbc.firebolt.timeout_sec" — 0 means no timeout (libcurl default)
    bool initialized = false;
};

} // namespace firebolt::adbc
