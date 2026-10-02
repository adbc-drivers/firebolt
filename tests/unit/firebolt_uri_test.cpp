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

#include "FireboltUri.h"

#include <string>
#include <variant>

using firebolt::adbc::FireboltUri;
using firebolt::adbc::FireboltUriError;
using firebolt::adbc::NotFireboltUri;
using firebolt::adbc::parseFireboltUri;

namespace
{

struct Parsed
{
    AdbcStatusCode code;
    FireboltUri uri;
    std::string message;
};

// Flattens the result for the tests below, which all pass a firebolt:// URI.
Parsed parse(const std::string & uri)
{
    Parsed p{};
    const auto result = parseFireboltUri(uri);
    EXPECT_FALSE(std::holds_alternative<NotFireboltUri>(result)) << uri;
    if (const auto * error = std::get_if<FireboltUriError>(&result))
    {
        p.code = error->code;
        p.message = error->message;
    }
    else if (const auto * parsed = std::get_if<FireboltUri>(&result))
    {
        p.code = ADBC_STATUS_OK;
        p.uri = *parsed;
    }
    return p;
}

bool isFireboltUri(const std::string & uri)
{
    return !std::holds_alternative<NotFireboltUri>(parseFireboltUri(uri));
}

} // namespace

TEST(FireboltUriTest, RecognisesSchemeCaseInsensitively)
{
    EXPECT_TRUE(isFireboltUri("firebolt://localhost"));
    EXPECT_TRUE(isFireboltUri("FIREBOLT://localhost"));
    EXPECT_FALSE(isFireboltUri("http://localhost:3473"));
    EXPECT_FALSE(isFireboltUri("firebolt:localhost"));
}

TEST(FireboltUriTest, TlsByDefault)
{
    auto p = parse("firebolt://engine.example.com/analytics");
    ASSERT_EQ(p.code, ADBC_STATUS_OK) << p.message;
    EXPECT_EQ(p.uri.endpoint, "https://engine.example.com");
    EXPECT_EQ(p.uri.database, "analytics");
}

TEST(FireboltUriTest, SslModeSelectsTransport)
{
    auto p = parse("firebolt://localhost:3473/playground?ssl_mode=disable");
    ASSERT_EQ(p.code, ADBC_STATUS_OK) << p.message;
    EXPECT_EQ(p.uri.endpoint, "http://localhost:3473");
    EXPECT_EQ(p.uri.database, "playground");

    p = parse("firebolt://localhost:3473?ssl_mode=verify-full");
    ASSERT_EQ(p.code, ADBC_STATUS_OK) << p.message;
    EXPECT_EQ(p.uri.endpoint, "https://localhost:3473");
}

TEST(FireboltUriTest, DatabaseIsOptional)
{
    for (const char * uri : {"firebolt://localhost:3473", "firebolt://localhost:3473/", "firebolt://localhost:3473/?ssl_mode=disable"})
    {
        auto p = parse(uri);
        ASSERT_EQ(p.code, ADBC_STATUS_OK) << uri << ": " << p.message;
        EXPECT_EQ(p.uri.database, "") << uri;
    }
}

TEST(FireboltUriTest, DatabaseIsPercentDecoded)
{
    auto p = parse("firebolt://localhost/my%20db%2Fx");
    ASSERT_EQ(p.code, ADBC_STATUS_OK) << p.message;
    EXPECT_EQ(p.uri.database, "my db/x");
}

TEST(FireboltUriTest, Ipv6HostKept)
{
    auto p = parse("firebolt://[::1]:3473/db?ssl_mode=disable");
    ASSERT_EQ(p.code, ADBC_STATUS_OK) << p.message;
    EXPECT_EQ(p.uri.endpoint, "http://[::1]:3473");
}

TEST(FireboltUriTest, MissingHostRejected)
{
    EXPECT_EQ(parse("firebolt://").code, ADBC_STATUS_INVALID_ARGUMENT);
    EXPECT_EQ(parse("firebolt:///db").code, ADBC_STATUS_INVALID_ARGUMENT);
}

TEST(FireboltUriTest, NestedPathRejected)
{
    // The path is a database name, not an HTTP path: a second segment has no meaning.
    auto p = parse("firebolt://localhost/db/extra");
    EXPECT_EQ(p.code, ADBC_STATUS_INVALID_ARGUMENT);
    EXPECT_NE(p.message.find("db/extra"), std::string::npos) << p.message;
}

TEST(FireboltUriTest, MalformedPercentEscapeKeptLiterally)
{
    // As in libcurl and browsers, a '%' that does not start a valid escape is
    // kept as it is rather than refused.
    auto p = parse("firebolt://localhost/db%zz");
    ASSERT_EQ(p.code, ADBC_STATUS_OK) << p.message;
    EXPECT_EQ(p.uri.database, "db%zz");
    p = parse("firebolt://localhost/db%2");
    ASSERT_EQ(p.code, ADBC_STATUS_OK) << p.message;
    EXPECT_EQ(p.uri.database, "db%2");
}

TEST(FireboltUriTest, UndecodableDatabaseRejected)
{
    // An encoded NUL cannot be part of a database name; it must not be cut short
    // or carried through into the request.
    auto p = parse("firebolt://localhost/db%00x");
    EXPECT_EQ(p.code, ADBC_STATUS_INVALID_ARGUMENT) << p.message;
}

TEST(FireboltUriTest, InvalidPortRejected)
{
    for (const char * uri : {"firebolt://localhost:abc/db", "firebolt://localhost:99999/db"})
    {
        auto p = parse(uri);
        EXPECT_EQ(p.code, ADBC_STATUS_INVALID_ARGUMENT) << uri;
        EXPECT_NE(p.message.find("ort"), std::string::npos) << uri << ": " << p.message;
    }
}

TEST(FireboltUriTest, MalformedUriRejected)
{
    EXPECT_EQ(parse("firebolt://loc alhost/db").code, ADBC_STATUS_INVALID_ARGUMENT);
}

TEST(FireboltUriTest, UnknownSslModeRejected)
{
    auto p = parse("firebolt://localhost?ssl_mode=sometimes");
    EXPECT_EQ(p.code, ADBC_STATUS_INVALID_ARGUMENT);
    EXPECT_NE(p.message.find("sometimes"), std::string::npos) << p.message;
}

TEST(FireboltUriTest, UnverifiedTlsModesNotImplemented)
{
    // Certificate verification has no off switch in this driver.
    EXPECT_EQ(parse("firebolt://localhost?ssl_mode=require").code, ADBC_STATUS_NOT_IMPLEMENTED);
    EXPECT_EQ(parse("firebolt://localhost?ssl_mode=verify-ca").code, ADBC_STATUS_NOT_IMPLEMENTED);
}

TEST(FireboltUriTest, CredentialsNotImplemented)
{
    // Credentials in the URI mean OAuth client_credentials, which this driver
    // does not do yet.  The message must not echo the secret.
    auto p = parse("firebolt://svc:s3cr3t@localhost/db");
    EXPECT_EQ(p.code, ADBC_STATUS_NOT_IMPLEMENTED);
    EXPECT_EQ(p.message.find("s3cr3t"), std::string::npos) << p.message;
}

TEST(FireboltUriTest, CredentialsWithUnencodedDelimitersNotEchoed)
{
    // An unencoded '/' or '?' in the password ends the authority early, so the
    // rest of the secret lands in the path or the query.  It must still be
    // recognised as credentials rather than reported back as a bad path or an
    // unknown parameter.
    for (const char * uri :
         {"firebolt://svc:s3c/r3t@localhost/db", "firebolt://svc:s3c?r3t=x@localhost/db", "firebolt://svc:s3c?r%zz3t@localhost"})
    {
        auto p = parse(uri);
        EXPECT_EQ(p.code, ADBC_STATUS_NOT_IMPLEMENTED) << uri << ": " << p.message;
        EXPECT_EQ(p.message.find("s3c"), std::string::npos) << uri << ": " << p.message;
        EXPECT_EQ(p.message.find("r3t"), std::string::npos) << uri << ": " << p.message;
    }
}

TEST(FireboltUriTest, PercentEncodedAtSignInDatabaseAccepted)
{
    // A literal '@' in a database name is written %40, and is not credentials.
    auto p = parse("firebolt://localhost/team%40analytics?ssl_mode=disable");
    ASSERT_EQ(p.code, ADBC_STATUS_OK) << p.message;
    EXPECT_EQ(p.uri.database, "team@analytics");
}

TEST(FireboltUriTest, UnknownQueryParameterRejected)
{
    // A typo here would otherwise be ignored as silently as a misspelled option.
    auto p = parse("firebolt://localhost/db?engine=main");
    EXPECT_EQ(p.code, ADBC_STATUS_NOT_FOUND);
    EXPECT_NE(p.message.find("engine"), std::string::npos) << p.message;
}
