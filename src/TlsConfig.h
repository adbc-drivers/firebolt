#pragma once

#include <functional>
#include <string>
#include <vector>

namespace firebolt::adbc
{

// Where libcurl finds the CA certificates that verify an https:// peer.
//
// curl's configure step bakes in the *build machine's* bundle path, which is
// wrong on any distribution laid out differently from the builder image, so the
// build disables that default (CURL_CA_BUNDLE=none) and the driver picks a
// bundle at DatabaseInit instead, in this order:
//
//   1. the adbc.firebolt.ssl_certificate_path option, when set;
//   2. the SSL_CERT_FILE environment variable, the OpenSSL convention;
//   3. the first readable file among kStandardCaBundlePaths.
//
// A source that is configured but unreadable is an error rather than a reason to
// fall through: the caller asked for that file, and silently trusting another
// one instead would be surprising.
struct CaBundleResult
{
    std::string path; // the bundle to hand to CURLOPT_CAINFO; empty on failure
    std::string error; // why no bundle was found; empty on success
    bool configuration_error = false; // true when an explicit option or env var is at fault
};

// Distribution bundle locations, probed in order.
extern const std::vector<std::string> kStandardCaBundlePaths;

// Injection points so tests can describe a filesystem and environment.
using EnvLookup = std::function<std::string(const char * name)>;
using FileProbe = std::function<bool(const std::string & path)>;

CaBundleResult resolveCaBundle(const std::string & configured_path, const EnvLookup & get_env, const FileProbe & is_readable_file);

// The real environment and filesystem.
CaBundleResult resolveCaBundle(const std::string & configured_path);

} // namespace firebolt::adbc
