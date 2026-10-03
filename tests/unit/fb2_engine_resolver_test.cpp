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
#include <gtest/gtest.h>

#include "fb2/Fb2EngineResolver.h"

#include <string>

using namespace firebolt::adbc::fb2;
using firebolt::adbc::Status;

namespace
{

Fb2HttpResult Response(long status, std::string body)
{
    Fb2HttpResult r;
    r.transport_ok = true;
    r.status = status;
    r.body = std::move(body);
    return r;
}

// Real values observed on the packdb-5-preview staging account.
const std::string kSystem = "https://01kpc5qwc1t0m7s7rgxytym2tr.api.us-east-1.staging.firebolt.io";
const std::string kGatewayHeader = "packdb-5-preview-firebolt.api.us-east-1.staging.firebolt.io?engine=packdb5";

} // namespace

TEST(Fb2EngineResolverTest, ApiEndpointFollowsTheEnvironment)
{
    EXPECT_EQ(apiEndpointFor("app"), "https://api.app.firebolt.io");
    EXPECT_EQ(apiEndpointFor("staging"), "https://api.staging.firebolt.io");
}

TEST(Fb2EngineResolverTest, EngineUrlRequestEncodesTheAccount)
{
    EXPECT_EQ(
        engineUrlRequestUrl("https://api.staging.firebolt.io", "packdb-5-preview"),
        "https://api.staging.firebolt.io/web/v3/account/packdb-5-preview/engineUrl");
    // A slash or '?' in the name must not change the path.
    EXPECT_EQ(engineUrlRequestUrl("https://api.x", "a/b?c"), "https://api.x/web/v3/account/a%2Fb%3Fc/engineUrl");
    EXPECT_EQ(engineUrlRequestUrl("https://api.x/", "a"), "https://api.x/web/v3/account/a/engineUrl");
}

TEST(Fb2EngineResolverTest, EngineUrlResponseGetsAScheme)
{
    std::string url;
    ASSERT_TRUE(parseEngineUrlResponse(
                    Response(200, R"({"engineUrl":"01kpc5qwc1t0m7s7rgxytym2tr.api.us-east-1.staging.firebolt.io"})"), "acct", "https", url)
                    .ok());
    EXPECT_EQ(url, kSystem);
}

TEST(Fb2EngineResolverTest, UnknownAccountIsNotFound)
{
    std::string url;
    Status s = parseEngineUrlResponse(
        Response(404, R"({"detail":"Requested resource has not been found in our records","kind":"not_found","status":404})"),
        "no_such",
        "https",
        url);
    EXPECT_EQ(s.code, ADBC_STATUS_NOT_FOUND);
    EXPECT_NE(s.message.find("no_such"), std::string::npos) << s.message;
}

TEST(Fb2EngineResolverTest, EngineUrlAuthFailureIsUnauthenticated)
{
    std::string url;
    EXPECT_EQ(parseEngineUrlResponse(Response(401, ""), "acct", "https", url).code, ADBC_STATUS_UNAUTHENTICATED);
}

TEST(Fb2EngineResolverTest, EngineUrlWithoutHostIsIo)
{
    std::string url;
    for (const char * body : {"", "{}", R"({"engineUrl":""})", R"({"engineUrl":5})"})
        EXPECT_EQ(parseEngineUrlResponse(Response(200, body), "acct", "https", url).code, ADBC_STATUS_IO) << body;
}

TEST(Fb2EngineResolverTest, UseStatementQuotesTheIdentifier)
{
    EXPECT_EQ(useStatement("ENGINE", "packdb5"), "USE ENGINE \"packdb5\"");
    EXPECT_EQ(useStatement("DATABASE", "we\"ird"), "USE DATABASE \"we\"\"ird\"");
}

TEST(Fb2EngineResolverTest, UseRefusalIsNotFoundWithTheServerText)
{
    Status s = parseUseResponse(
        Response(400, R"({"errors":[{"description":"Engine 'nope' does not exist or not authorized."}]})"), "engine", "nope");
    EXPECT_EQ(s.code, ADBC_STATUS_NOT_FOUND);
    EXPECT_NE(s.message.find("does not exist or not authorized"), std::string::npos) << s.message;
    EXPECT_NE(s.message.find("firebolt.engine"), std::string::npos) << s.message;
}

TEST(Fb2EngineResolverTest, UseSuccess)
{
    EXPECT_TRUE(parseUseResponse(Response(200, "{}"), "database", "test").ok());
}

TEST(Fb2EngineResolverTest, UpdateEndpointToASiblingGatewayIsAccepted)
{
    std::string url;
    ASSERT_TRUE(resolveUpdateEndpoint(kGatewayHeader, kSystem, url).ok());
    EXPECT_EQ(url, "https://" + kGatewayHeader);
}

TEST(Fb2EngineResolverTest, UpdateEndpointToTheSameHostIsAccepted)
{
    std::string url;
    ASSERT_TRUE(resolveUpdateEndpoint("01kpc5qwc1t0m7s7rgxytym2tr.api.us-east-1.staging.firebolt.io?engine=e", kSystem, url).ok());
}

TEST(Fb2EngineResolverTest, UpdateEndpointElsewhereIsRefused)
{
    std::string url;
    for (const char * header : {
             "evil.example.com?engine=x",
             "api.us-east-1.staging.firebolt.io.evil.com?engine=x", // suffix trick
             "x.api.us-east-2.staging.firebolt.io?engine=x", // another region's parent
             "http://packdb-5-preview-firebolt.api.us-east-1.staging.firebolt.io?engine=x", // downgrade
             "user@evil.com?engine=x",
             "",
         })
    {
        Status s = resolveUpdateEndpoint(header, kSystem, url);
        EXPECT_EQ(s.code, ADBC_STATUS_IO) << header;
    }
}

TEST(Fb2EngineResolverTest, UpdateEndpointPortMustMatch)
{
    std::string url;
    EXPECT_TRUE(resolveUpdateEndpoint("engine.localhost:4000?engine=e", "http://sys.localhost:4000", url).ok());
    EXPECT_EQ(url, "http://engine.localhost:4000?engine=e");
    EXPECT_FALSE(resolveUpdateEndpoint("engine.localhost:4001?engine=e", "http://sys.localhost:4000", url).ok());
}

TEST(Fb2EngineResolverTest, IpLiteralSystemEngineAllowsOnlyItself)
{
    std::string url;
    EXPECT_TRUE(resolveUpdateEndpoint("127.0.0.1:4000?engine=e", "http://127.0.0.1:4000", url).ok());
    EXPECT_FALSE(resolveUpdateEndpoint("127.0.0.2:4000?engine=e", "http://127.0.0.1:4000", url).ok());
}

TEST(Fb2EngineResolverTest, Ipv6LiteralSystemEngineAllowsOnlyItself)
{
    std::string url;
    EXPECT_TRUE(resolveUpdateEndpoint("[::1]:4000?engine=e", "http://[::1]:4000", url).ok());
    EXPECT_FALSE(resolveUpdateEndpoint("[::2]:4000?engine=e", "http://[::1]:4000", url).ok());
}

TEST(Fb2EngineResolverTest, StoppedEngineIsExplained)
{
    bool stopped = false;
    std::string msg = explainEngineError(
        404,
        "You are attempting to run a query on engine 'packdb5', but either it does not exist, it is stopped, or you don't have "
        "permission to access it.",
        stopped);
    EXPECT_TRUE(stopped);
    EXPECT_NE(msg.find("START ENGINE"), std::string::npos) << msg;
}

TEST(Fb2EngineResolverTest, PreV5EngineIsExplained)
{
    bool stopped = false;
    std::string msg = explainEngineError(404, R"({"errors":[{"description":"Output format 'ArrowStream' is not supported."}]})", stopped);
    EXPECT_FALSE(stopped);
    EXPECT_NE(msg.find("version 5"), std::string::npos) << msg;
}

TEST(Fb2EngineResolverTest, OtherErrorsAreLeftAlone)
{
    bool stopped = false;
    EXPECT_EQ(explainEngineError(400, "syntax error", stopped), "syntax error");
    EXPECT_FALSE(stopped);
}
