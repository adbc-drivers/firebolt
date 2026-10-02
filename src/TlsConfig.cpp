#include "TlsConfig.h"

#include <cstdlib>

#include <unistd.h>
#include <sys/stat.h>

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
        {
            result.configuration_error = true;
            result.error = "Option 'adbc.firebolt.ssl_certificate_path' names '" + configured_path + "', which is not a readable file";
        }
        return result;
    }

    const std::string env_path = get_env("SSL_CERT_FILE");
    if (!env_path.empty())
    {
        if (is_readable_file(env_path))
            result.path = env_path;
        else
        {
            result.configuration_error = true;
            result.error = "SSL_CERT_FILE names '" + env_path + "', which is not a readable file";
        }
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
    result.error = "No CA certificate bundle found for https:// (tried " + tried
        + "). Install the system CA certificates, or point adbc.firebolt.ssl_certificate_path at a PEM bundle";
    return result;
}

CaBundleResult resolveCaBundle(const std::string & configured_path)
{
    return resolveCaBundle(
        configured_path,
        [](const char * name) {
            const char * value = std::getenv(name);
            return std::string(value ? value : "");
        },
        [](const std::string & path) {
            struct stat st = {};
            return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(path.c_str(), R_OK) == 0;
        });
}

} // namespace firebolt::adbc
