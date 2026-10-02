#pragma once

#include "adbc.h"

#include <string>

namespace firebolt::adbc
{

struct FireboltDatabase
{
    std::string url; // "uri" — the HTTP query endpoint, e.g. "http://localhost:3473"
    std::string token; // "adbc.firebolt.token" — JWT bearer token (omit for no-auth)
    std::string database; // "adbc.firebolt.database" — database name (optional)
    long timeout_sec = 0; // "adbc.firebolt.timeout_sec" — 0 means no timeout (libcurl default)
    std::string ssl_certificate_path; // "adbc.firebolt.ssl_certificate_path" — CA bundle override
    std::string ca_bundle_path; // resolved at DatabaseInit for https://; see TlsConfig.h
    bool initialized = false;

    // First rejected option, held until DatabaseInit reports it.  See the note in
    // DatabaseSetOption for why a bad option is not refused on the spot.
    std::string option_error;
    AdbcStatusCode option_error_code = ADBC_STATUS_OK;
};

} // namespace firebolt::adbc
