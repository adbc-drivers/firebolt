// Copyright (c) 2026 ADBC Drivers Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//         http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "TlsConfig.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace firebolt::adbc
{

const std::vector<std::string> kStandardCaBundlePaths = {
    "/etc/ssl/certs/ca-certificates.crt", // Debian, Ubuntu, Alpine, Arch
    "/etc/pki/tls/certs/ca-bundle.crt", // RHEL, CentOS, Fedora, Amazon Linux
    "/etc/ssl/ca-bundle.pem", // openSUSE, SLES
    "/etc/ssl/cert.pem", // Alpine (alias), macOS-style layouts
};

CaBundleResult resolveCaBundle(const std::string & configured_path, const EnvLookup & get_env, const FileProbe & is_readable_file)
{
    CaBundleResult result;
    if (!configured_path.empty())
    {
        if (is_readable_file(configured_path))
            result.path = configured_path;
        else
            result.status
                = {ADBC_STATUS_INVALID_ARGUMENT,
                   "Option 'firebolt.ssl_certificate_path' names '" + configured_path + "', which is not a readable file"};
        return result;
    }

    const std::string env_path = get_env("SSL_CERT_FILE");
    if (!env_path.empty())
    {
        if (is_readable_file(env_path))
            result.path = env_path;
        else
            result.status = {ADBC_STATUS_INVALID_ARGUMENT, "SSL_CERT_FILE names '" + env_path + "', which is not a readable file"};
        return result;
    }

    std::string tried;
    for (const auto & candidate : kStandardCaBundlePaths)
    {
        if (is_readable_file(candidate))
        {
            result.path = candidate;
            return result;
        }
        tried += (tried.empty() ? "" : ", ") + candidate;
    }
    result.status
        = {ADBC_STATUS_INVALID_STATE,
           "No CA certificate bundle found for https:// (tried " + tried
               + "). Install the system CA certificates, or point firebolt.ssl_certificate_path at a PEM bundle"};
    return result;
}

CaBundleResult resolveCaBundle(const std::string & configured_path)
{
    const char * raw_env_path = std::getenv("SSL_CERT_FILE");
    const std::string env_path = raw_env_path ? raw_env_path : "";
#if defined(__APPLE__) || defined(_WIN32)
    // Secure Transport and Schannel use the native certificate store when
    // CURLOPT_CAINFO is not set. Explicit file configuration still wins.
    if (configured_path.empty() && env_path.empty())
        return {};
#endif
    return resolveCaBundle(
        configured_path,
        [&env_path](const char * name) { return std::string(name) == "SSL_CERT_FILE" ? env_path : std::string(); },
        [](const std::string & path) {
            std::error_code error;
            if (!std::filesystem::is_regular_file(path, error))
                return false;
            std::ifstream input(path, std::ios::binary);
            return input.good();
        });
}

} // namespace firebolt::adbc
