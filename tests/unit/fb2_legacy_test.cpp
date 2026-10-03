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

// FB2 SaaS (Legacy) mode. Keep isolated; see CLAUDE.md.
//
// Drives the mode through the public ADBC entry points, so these tests hold whatever the module looks like
// inside.
#include <gtest/gtest.h>

#include "Status.h"
#include "adbc.h"

#include <string>
#include <utility>
#include <vector>

extern "C" AdbcStatusCode AdbcDriverInit(int version, void * raw_driver, AdbcError * error);

namespace
{

using Options = std::vector<std::pair<std::string, std::string>>;

// Set every option, then DatabaseInit.  Options must be accepted when set: a bad
// value is reported by Init (see RejectOption in FireboltAdbcDriver.cpp).
firebolt::adbc::Status InitWith(const Options & options)
{
    AdbcDriver driver{};
    AdbcError error = ADBC_ERROR_INIT;
    EXPECT_EQ(AdbcDriverInit(ADBC_VERSION_1_1_0, &driver, &error), ADBC_STATUS_OK);
    AdbcDatabase db{};
    EXPECT_EQ(driver.DatabaseNew(&db, &error), ADBC_STATUS_OK);
    for (const auto & [k, v] : options)
        EXPECT_EQ(driver.DatabaseSetOption(&db, k.c_str(), v.c_str(), &error), ADBC_STATUS_OK) << k;
    firebolt::adbc::Status out{driver.DatabaseInit(&db, &error), ""};
    if (error.message)
        out.message = error.message;
    if (error.release)
        error.release(&error);
    driver.DatabaseRelease(&db, nullptr);
    return out;
}

// A complete, valid FB2 configuration; tests take one piece away or add a conflict.
Options Valid()
{
    return {
        {"username", "fbcid_test"},
        {"password", "fbsec_test"},
        {"firebolt.account", "my_account"},
        {"firebolt.engine", "my_engine"},
        // Unroutable on purpose: nothing in a unit test may reach the network.
        {"firebolt.api_endpoint", "http://127.0.0.1:9"},
        {"firebolt.auth_endpoint", "http://127.0.0.1:9/oauth/token"},
        {"firebolt.timeout_sec", "2"},
    };
}

Options Without(Options o, const std::string & key)
{
    std::erase_if(o, [&](const auto & kv) { return kv.first == key; });
    return o;
}

Options With(Options o, const std::string & key, const std::string & value)
{
    o.emplace_back(key, value);
    return o;
}

void ExpectInvalid(const Options & options, const std::string & must_mention)
{
    auto r = InitWith(options);
    EXPECT_EQ(r.code, ADBC_STATUS_INVALID_ARGUMENT) << r.message;
    EXPECT_NE(r.message.find(must_mention), std::string::npos) << "message should name '" << must_mention << "': " << r.message;
}

} // namespace

TEST(Fb2LegacyOptionsTest, UriConflictsWithAccount)
{
    ExpectInvalid(With(Valid(), "uri", "https://example.firebolt.io"), "uri");
}

TEST(Fb2LegacyOptionsTest, AccountIsRequired)
{
    ExpectInvalid(Without(Valid(), "firebolt.account"), "firebolt.account");
}

TEST(Fb2LegacyOptionsTest, CredentialsWithoutAccountNameTheMissingAccount)
{
    // Credentials alone select FB2 mode; Firebolt 3.0 service accounts are not supported yet.
    ExpectInvalid({{"username", "id"}, {"password", "secret"}, {"uri", "http://localhost:3473"}}, "firebolt.account");
}

TEST(Fb2LegacyOptionsTest, EngineIsRequired)
{
    ExpectInvalid(Without(Valid(), "firebolt.engine"), "firebolt.engine");
}

TEST(Fb2LegacyOptionsTest, CredentialsOrTokenAreRequired)
{
    ExpectInvalid(Without(Without(Valid(), "username"), "password"), "username");
}

TEST(Fb2LegacyOptionsTest, SecretWithoutIdIsRejected)
{
    ExpectInvalid(Without(Valid(), "username"), "username");
}

TEST(Fb2LegacyOptionsTest, IdWithoutSecretIsRejected)
{
    ExpectInvalid(Without(Valid(), "password"), "password");
}

TEST(Fb2LegacyOptionsTest, BothSpellingsOfOneCredentialAreRejected)
{
    ExpectInvalid(With(Valid(), "firebolt.client_id", "fbcid_other"), "firebolt.client_id");
    ExpectInvalid(With(Valid(), "firebolt.client_secret", "fbsec_other"), "firebolt.client_secret");
}

TEST(Fb2LegacyOptionsTest, TokenAndCredentialsTogetherAreRejected)
{
    ExpectInvalid(With(Valid(), "firebolt.token", "eyJ.raw.jwt"), "firebolt.token");
}

TEST(Fb2LegacyOptionsTest, EnvironmentCannotLeaveFireboltIo)
{
    // Spliced into https://id.<env>.firebolt.io, where the credentials are sent:
    // any value that moves the host elsewhere must be refused.
    for (const char * env : {"", "evil.example.com#", "evil.example.com/", "evil.example.com?", "user@evil.example.com", "x:1"})
        ExpectInvalid(With(Valid(), "firebolt.environment", env), "firebolt.environment");
}

TEST(Fb2LegacyOptionsTest, EnvironmentNamesAreAccepted)
{
    // Whatever Init reports next is about reaching the (unroutable) control
    // plane, not about the option.
    for (const char * env : {"app", "staging", "dev-2"})
    {
        auto r = InitWith(With(Valid(), "firebolt.environment", env));
        EXPECT_NE(r.code, ADBC_STATUS_INVALID_ARGUMENT) << env << ": " << r.message;
    }
}

TEST(Fb2LegacyOptionsTest, EndpointOverridesMustBeHttpOrHttpsUrls)
{
    for (const char * key : {"firebolt.api_endpoint", "firebolt.auth_endpoint"})
        for (const char * url : {"api.example.com", "ftp://api.example.com", "https://"})
            ExpectInvalid(With(Without(Valid(), key), key, url), key);
}

TEST(Fb2LegacyOptionsTest, CacheConnectionAcceptsOnlyTrueOrFalse)
{
    ExpectInvalid(With(Valid(), "firebolt.cache_connection", "yes"), "firebolt.cache_connection");
}

TEST(Fb2LegacyOptionsTest, SecretNeverAppearsInAnError)
{
    auto r = InitWith(With(Without(Valid(), "firebolt.engine"), "firebolt.client_secret", "fbsec_LEAKCHECK"));
    EXPECT_NE(r.code, ADBC_STATUS_OK);
    EXPECT_EQ(r.message.find("LEAKCHECK"), std::string::npos) << r.message;
}

TEST(Fb2LegacyOptionsTest, ClientIdSpellingWorksLikeUsername)
{
    // A complete configuration under the Firebolt spellings passes validation:
    // whatever Init reports next is about reaching the (unroutable) control
    // plane, not about the options.
    auto o = Without(Without(Valid(), "username"), "password");
    o = With(With(o, "firebolt.client_id", "fbcid_test"), "firebolt.client_secret", "fbsec_test");
    auto r = InitWith(o);
    EXPECT_NE(r.code, ADBC_STATUS_INVALID_ARGUMENT) << r.message;
}
