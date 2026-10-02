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

#include "fb2/Fb2TokenClient.h"

#include <chrono>
#include <string>

using namespace firebolt::adbc::fb2;
using std::chrono::seconds;
using std::chrono::steady_clock;

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

const std::string kEndpoint = "https://id.staging.firebolt.io/oauth/token";
const Credentials kCreds{"fbcid_test", "fbsec_SECRET"};

} // namespace

TEST(Fb2TokenClientTest, TokenEndpointFollowsTheEnvironment)
{
    EXPECT_EQ(tokenEndpointFor("app"), "https://id.app.firebolt.io/oauth/token");
    EXPECT_EQ(tokenEndpointFor("staging"), kEndpoint);
}

TEST(Fb2TokenClientTest, RequestBodyIsAFormWithTheFixedAudience)
{
    const std::string body = tokenRequestBody({"id one", "s&c=r+t"});
    EXPECT_NE(body.find("grant_type=client_credentials"), std::string::npos) << body;
    EXPECT_NE(body.find("audience=https%3A%2F%2Fapi.firebolt.io"), std::string::npos) << body;
    EXPECT_NE(body.find("client_id=id%20one"), std::string::npos) << body;
    // Every reserved character escaped, so a secret cannot add form fields.
    EXPECT_NE(body.find("client_secret=s%26c%3Dr%2Bt"), std::string::npos) << body;
}

TEST(Fb2TokenClientTest, GrantParsed)
{
    TokenGrant grant;
    Status s = parseTokenResponse(
        Response(200, R"({"access_token":"eyJ.tok","expires_in":7199,"scope":"service-account","token_type":"bearer"})"),
        kEndpoint,
        kCreds.client_id,
        grant);
    ASSERT_TRUE(s.ok()) << s.message;
    EXPECT_EQ(grant.access_token, "eyJ.tok");
    EXPECT_EQ(grant.expires_in, seconds(7199));
}

TEST(Fb2TokenClientTest, InvalidClientIsUnauthenticatedAndNamesTheClientId)
{
    TokenGrant grant;
    Status s = parseTokenResponse(
        Response(401, R"({"error":"invalid_client","error_description":"Client authentication failed"})"),
        kEndpoint,
        kCreds.client_id,
        grant);
    EXPECT_EQ(s.code, ADBC_STATUS_UNAUTHENTICATED);
    EXPECT_NE(s.message.find("fbcid_test"), std::string::npos) << s.message;
    EXPECT_NE(s.message.find("invalid_client"), std::string::npos) << s.message;
    EXPECT_EQ(s.message.find("SECRET"), std::string::npos) << s.message;
}

TEST(Fb2TokenClientTest, RateLimitIsIoAndCarriesRetryAfter)
{
    TokenGrant grant;
    auto r = Response(429, R"({"error":"temporarily_unavailable"})");
    r.retry_after = "30";
    Status s = parseTokenResponse(r, kEndpoint, kCreds.client_id, grant);
    EXPECT_EQ(s.code, ADBC_STATUS_IO);
    EXPECT_NE(s.message.find("30"), std::string::npos) << s.message;
}

TEST(Fb2TokenClientTest, MalformedSuccessBodiesAreRejected)
{
    TokenGrant grant;
    for (const char * body : {"", "not json", R"({"expires_in":10})", R"({"access_token":"","expires_in":10})", R"({"access_token":7})"})
    {
        Status s = parseTokenResponse(Response(200, body), kEndpoint, kCreds.client_id, grant);
        EXPECT_EQ(s.code, ADBC_STATUS_IO) << body;
    }
}

TEST(Fb2TokenClientTest, MissingExpiresInMeansDoNotCache)
{
    TokenGrant grant;
    ASSERT_TRUE(parseTokenResponse(Response(200, R"({"access_token":"eyJ.tok"})"), kEndpoint, kCreds.client_id, grant).ok());
    EXPECT_EQ(grant.expires_in, seconds(0));
}

TEST(Fb2TokenClientTest, TransportFailureIsIo)
{
    Fb2HttpResult r;
    r.transport_error = "Couldn't connect to server";
    TokenGrant grant;
    Status s = parseTokenResponse(r, kEndpoint, kCreds.client_id, grant);
    EXPECT_EQ(s.code, ADBC_STATUS_IO);
    EXPECT_NE(s.message.find(kEndpoint), std::string::npos) << s.message;
}

TEST(Fb2TokenCacheTest, ValidUntilTheMarginBeforeExpiry)
{
    TokenCache::clearForTesting();
    const auto t0 = steady_clock::now();
    TokenCache::store(kEndpoint, kCreds, {"tok", seconds(7200)}, t0);
    EXPECT_EQ(TokenCache::lookup(kEndpoint, kCreds, t0 + seconds(7200) - kExpiryMargin - seconds(1)), "tok");
    EXPECT_EQ(TokenCache::lookup(kEndpoint, kCreds, t0 + seconds(7200) - kExpiryMargin), "");
}

TEST(Fb2TokenCacheTest, KeyedByEndpointAndBothCredentials)
{
    TokenCache::clearForTesting();
    const auto t0 = steady_clock::now();
    TokenCache::store(kEndpoint, kCreds, {"tok", seconds(7200)}, t0);
    EXPECT_EQ(TokenCache::lookup("https://id.app.firebolt.io/oauth/token", kCreds, t0), "");
    EXPECT_EQ(TokenCache::lookup(kEndpoint, {"other_id", kCreds.client_secret}, t0), "");
    EXPECT_EQ(TokenCache::lookup(kEndpoint, {kCreds.client_id, "rotated"}, t0), "");
}

TEST(Fb2TokenCacheTest, ShortLivedOrUndatedGrantsAreNotCached)
{
    TokenCache::clearForTesting();
    const auto t0 = steady_clock::now();
    TokenCache::store(kEndpoint, kCreds, {"tok", seconds(0)}, t0);
    EXPECT_EQ(TokenCache::lookup(kEndpoint, kCreds, t0), "");
    TokenCache::store(kEndpoint, kCreds, {"tok", kExpiryMargin}, t0);
    EXPECT_EQ(TokenCache::lookup(kEndpoint, kCreds, t0), "");
}

TEST(Fb2TokenCacheTest, InvalidateDropsTheEntry)
{
    TokenCache::clearForTesting();
    const auto t0 = steady_clock::now();
    TokenCache::store(kEndpoint, kCreds, {"tok", seconds(7200)}, t0);
    TokenCache::invalidate(kEndpoint, kCreds);
    EXPECT_EQ(TokenCache::lookup(kEndpoint, kCreds, t0), "");
}
