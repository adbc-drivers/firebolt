#include <gtest/gtest.h>

#include "TlsConfig.h"

#include <map>
#include <set>
#include <string>

using firebolt::adbc::kStandardCaBundlePaths;
using firebolt::adbc::resolveCaBundle;

namespace
{

// A fake environment and filesystem: only the listed variables are set and only
// the listed files exist.
struct FakeHost
{
    std::map<std::string, std::string> env;
    std::set<std::string> files;

    firebolt::adbc::CaBundleResult resolve(const std::string & configured) const
    {
        return resolveCaBundle(
            configured,
            [this](const char * name) {
                auto it = env.find(name);
                return it == env.end() ? std::string() : it->second;
            },
            [this](const std::string & path) { return files.count(path) > 0; });
    }
};

} // namespace

TEST(CaBundleTest, ConfiguredPathWins)
{
    FakeHost host{{{"SSL_CERT_FILE", "/env/ca.pem"}}, {"/opt/ca.pem", "/env/ca.pem", kStandardCaBundlePaths.front()}};
    auto r = host.resolve("/opt/ca.pem");
    EXPECT_EQ(r.path, "/opt/ca.pem");
    EXPECT_TRUE(r.error.empty());
}

TEST(CaBundleTest, MissingConfiguredPathIsAConfigurationErrorNotAFallThrough)
{
    FakeHost host{{}, {kStandardCaBundlePaths.front()}};
    auto r = host.resolve("/no/such/ca.pem");
    EXPECT_TRUE(r.path.empty());
    EXPECT_TRUE(r.configuration_error);
    EXPECT_NE(r.error.find("/no/such/ca.pem"), std::string::npos) << r.error;
    EXPECT_NE(r.error.find("adbc.firebolt.ssl_certificate_path"), std::string::npos) << r.error;
}

TEST(CaBundleTest, SslCertFileBeatsStandardPaths)
{
    FakeHost host{{{"SSL_CERT_FILE", "/env/ca.pem"}}, {"/env/ca.pem", kStandardCaBundlePaths.front()}};
    EXPECT_EQ(host.resolve("").path, "/env/ca.pem");
}

TEST(CaBundleTest, MissingSslCertFileIsAConfigurationError)
{
    FakeHost host{{{"SSL_CERT_FILE", "/env/missing.pem"}}, {kStandardCaBundlePaths.front()}};
    auto r = host.resolve("");
    EXPECT_TRUE(r.path.empty());
    EXPECT_TRUE(r.configuration_error);
    EXPECT_NE(r.error.find("SSL_CERT_FILE"), std::string::npos) << r.error;
}

TEST(CaBundleTest, FirstExistingStandardPathIsUsed)
{
    ASSERT_GE(kStandardCaBundlePaths.size(), 2u);
    // Only the second location exists, as on RHEL where the Debian path is absent.
    FakeHost host{{}, {kStandardCaBundlePaths[1]}};
    EXPECT_EQ(host.resolve("").path, kStandardCaBundlePaths[1]);
}

TEST(CaBundleTest, StandardPathsCoverDebianAndRhel)
{
    std::set<std::string> paths(kStandardCaBundlePaths.begin(), kStandardCaBundlePaths.end());
    EXPECT_TRUE(paths.count("/etc/ssl/certs/ca-certificates.crt"));
    EXPECT_TRUE(paths.count("/etc/pki/tls/certs/ca-bundle.crt"));
}

TEST(CaBundleTest, NothingFoundNamesEveryPathTriedAndTheFix)
{
    FakeHost host;
    auto r = host.resolve("");
    EXPECT_TRUE(r.path.empty());
    EXPECT_FALSE(r.configuration_error);
    for (const auto & p : kStandardCaBundlePaths)
        EXPECT_NE(r.error.find(p), std::string::npos) << "missing " << p << " in: " << r.error;
    EXPECT_NE(r.error.find("adbc.firebolt.ssl_certificate_path"), std::string::npos) << r.error;
}
