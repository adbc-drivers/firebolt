#pragma once

#include <memory>
#include <string>
#include <unordered_map>

namespace firebolt::adbc
{

struct FireboltDatabase;
class HttpClient;

struct FireboltConnection
{
    FireboltDatabase * db = nullptr; // borrowed
    std::unique_ptr<HttpClient> http;
    std::unordered_map<std::string, std::string> session_params;
    bool autocommit = true; // ADBC_CONNECTION_OPTION_AUTOCOMMIT
    bool in_transaction = false; // true between BEGIN and COMMIT/ROLLBACK
};

} // namespace firebolt::adbc
