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
    EXPECT_TRUE(r.status.ok()) << r.status.message;
}

TEST(CaBundleTest, MissingConfiguredPathIsAConfigurationErrorNotAFallThrough)
{
    FakeHost host{{}, {kStandardCaBundlePaths.front()}};
    auto r = host.resolve("/no/such/ca.pem");
    EXPECT_TRUE(r.path.empty());
    EXPECT_EQ(r.status.code, ADBC_STATUS_INVALID_ARGUMENT);
    EXPECT_NE(r.status.message.find("/no/such/ca.pem"), std::string::npos) << r.status.message;
    EXPECT_NE(r.status.message.find("firebolt.ssl_certificate_path"), std::string::npos) << r.status.message;
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
    EXPECT_EQ(r.status.code, ADBC_STATUS_INVALID_ARGUMENT);
    EXPECT_NE(r.status.message.find("SSL_CERT_FILE"), std::string::npos) << r.status.message;
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
    EXPECT_EQ(r.status.code, ADBC_STATUS_INVALID_STATE);
    for (const auto & p : kStandardCaBundlePaths)
        EXPECT_NE(r.status.message.find(p), std::string::npos) << "missing " << p << " in: " << r.status.message;
    EXPECT_NE(r.status.message.find("firebolt.ssl_certificate_path"), std::string::npos) << r.status.message;
}
