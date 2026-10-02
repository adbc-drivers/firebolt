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

#include "HttpClient.h"

#include <string>
#include <unordered_map>

namespace
{

firebolt::adbc::HttpResponse makeOk()
{
    firebolt::adbc::HttpResponse r;
    r.curl_code = CURLE_OK;
    r.http_code = 200;
    return r;
}

firebolt::adbc::HttpResponse makeError(long http_code)
{
    firebolt::adbc::HttpResponse r;
    r.curl_code = CURLE_OK;
    r.http_code = http_code;
    return r;
}

} // namespace

// Sanity: legitimate 2xx responses still apply update_params.
TEST(SessionStateTest, UpdateParamsAppliedOnSuccess)
{
    std::unordered_map<std::string, std::string> session;
    auto resp = makeOk();
    resp.update_params["database"] = "ok";
    EXPECT_TRUE(firebolt::adbc::applySessionUpdatesIfSuccess(session, resp));
    EXPECT_EQ(session["database"], "ok");
}

TEST(SessionStateTest, RemoveParamsAppliedOnSuccess)
{
    std::unordered_map<std::string, std::string> session{{"foo", "bar"}};
    auto resp = makeOk();
    resp.remove_params.push_back("foo");
    EXPECT_TRUE(firebolt::adbc::applySessionUpdatesIfSuccess(session, resp));
    EXPECT_EQ(session.count("foo"), 0u);
}

TEST(SessionStateTest, ResetSessionAppliedOnSuccess)
{
    std::unordered_map<std::string, std::string> session{{"foo", "bar"}};
    auto resp = makeOk();
    resp.reset_session = true;
    EXPECT_TRUE(firebolt::adbc::applySessionUpdatesIfSuccess(session, resp));
    EXPECT_TRUE(session.empty());
}

// The actual hijack scenario: a 5xx response with attacker-injected
// Firebolt-Update-Parameters MUST NOT mutate session state.
TEST(SessionStateTest, NoSessionMutationOn5xx)
{
    std::unordered_map<std::string, std::string> session{{"legitimate", "yes"}};
    auto resp = makeError(500);
    resp.update_params["database"] = "attacker_owned_db";
    EXPECT_FALSE(firebolt::adbc::applySessionUpdatesIfSuccess(session, resp));
    EXPECT_EQ(session["legitimate"], "yes");
    EXPECT_EQ(session.count("database"), 0u)
        << "5xx response injected a session param; subsequent queries would route to attacker_owned_db";
}

TEST(SessionStateTest, NoSessionMutationOn4xx)
{
    std::unordered_map<std::string, std::string> session;
    auto resp = makeError(401);
    resp.update_params["account_id"] = "attacker";
    EXPECT_FALSE(firebolt::adbc::applySessionUpdatesIfSuccess(session, resp));
    EXPECT_EQ(session.count("account_id"), 0u);
}

TEST(SessionStateTest, ResetIgnoredOnError)
{
    std::unordered_map<std::string, std::string> session{{"existing", "yes"}};
    auto resp = makeError(503);
    resp.reset_session = true;
    EXPECT_FALSE(firebolt::adbc::applySessionUpdatesIfSuccess(session, resp));
    EXPECT_EQ(session["existing"], "yes") << "5xx + Firebolt-Reset-Session must NOT clear pre-existing session state";
}

TEST(SessionStateTest, NoSessionMutationOnCurlError)
{
    std::unordered_map<std::string, std::string> session{{"legitimate", "yes"}};
    firebolt::adbc::HttpResponse resp;
    resp.curl_code = CURLE_COULDNT_CONNECT;
    resp.http_code = 0;
    resp.update_params["database"] = "evil";
    EXPECT_FALSE(firebolt::adbc::applySessionUpdatesIfSuccess(session, resp));
    EXPECT_EQ(session.count("database"), 0u);
}
