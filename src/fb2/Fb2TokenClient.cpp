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
#include "fb2/Fb2TokenClient.h"

#include <nlohmann/json.hpp>

#include <map>
#include <mutex>
#include <tuple>

namespace firebolt::adbc::fb2
{

namespace
{

    constexpr const char * kPrefix = "FB2 SaaS mode: ";

    // A server error body, trimmed to something that fits in a message.
    std::string excerpt(const std::string & body)
    {
        constexpr size_t max_length = 300;
        return body.size() <= max_length ? body : body.substr(0, max_length) + "…";
    }

    struct CacheEntry
    {
        std::string token;
        std::chrono::steady_clock::time_point refresh_at;
    };

    // The secret is part of the key so a rotated secret never reuses the old
    // one's token.  It sits in process memory either way: the options hold it.
    using CacheKey = std::tuple<std::string, std::string, std::string>;

    std::mutex g_cache_mu;
    std::map<CacheKey, CacheEntry> g_cache;

    CacheKey keyOf(const std::string & endpoint, const Credentials & c)
    {
        return {endpoint, c.client_id, c.client_secret};
    }

} // namespace

std::string tokenEndpointFor(const std::string & environment)
{
    return "https://id." + environment + ".firebolt.io/oauth/token";
}

std::string tokenRequestBody(const Credentials & credentials)
{
    return "grant_type=client_credentials&audience=" + formEncode(kAudience) + "&client_id=" + formEncode(credentials.client_id)
        + "&client_secret=" + formEncode(credentials.client_secret);
}

Status parseTokenResponse(const Fb2HttpResult & result, const std::string & endpoint, const std::string & client_id, TokenGrant & grant)
{
    if (!result.transport_ok)
        return {ADBC_STATUS_IO, std::string(kPrefix) + "could not reach the token endpoint " + endpoint + ": " + result.transport_error};

    const nlohmann::json body = nlohmann::json::parse(result.body, nullptr, /*allow_exceptions=*/false);

    if (result.status == 429)
        return {
            ADBC_STATUS_IO,
            std::string(kPrefix) + "the token endpoint " + endpoint + " is rate limiting requests (HTTP 429)"
                + (result.retry_after.empty() ? "" : "; retry after " + result.retry_after + " s")};

    if (result.status < 200 || result.status >= 300)
    {
        // RFC 6749 §5.2: a refused client is 400 or 401 with error=invalid_client.
        std::string oauth_error
            = body.is_object() && body.contains("error") && body["error"].is_string() ? body["error"].get<std::string>() : "";
        if (result.status == 401 || oauth_error == "invalid_client" || oauth_error == "unauthorized_client")
            return {
                ADBC_STATUS_UNAUTHENTICATED,
                std::string(kPrefix) + "service account '" + client_id + "' was rejected by " + endpoint + " (HTTP "
                    + std::to_string(result.status) + (oauth_error.empty() ? "" : ", " + oauth_error)
                    + "). Check the client ID and secret, and that the service account is enabled"};
        return {
            ADBC_STATUS_IO,
            std::string(kPrefix) + "the token endpoint " + endpoint + " answered HTTP " + std::to_string(result.status) + ": "
                + excerpt(result.body)};
    }

    if (!body.is_object() || !body.contains("access_token") || !body["access_token"].is_string()
        || body["access_token"].get<std::string>().empty())
        return {ADBC_STATUS_IO, std::string(kPrefix) + "the token endpoint " + endpoint + " returned no access_token"};

    grant.access_token = body["access_token"].get<std::string>();
    // A grant without a usable lifetime is used once and not cached.
    grant.expires_in = std::chrono::seconds(
        body.contains("expires_in") && body["expires_in"].is_number_integer() ? body["expires_in"].get<long long>() : 0);
    return {};
}

std::string TokenCache::lookup(const std::string & endpoint, const Credentials & credentials, std::chrono::steady_clock::time_point now)
{
    std::lock_guard lock(g_cache_mu);
    auto it = g_cache.find(keyOf(endpoint, credentials));
    if (it == g_cache.end() || now >= it->second.refresh_at)
        return {};
    return it->second.token;
}

void TokenCache::store(
    const std::string & endpoint, const Credentials & credentials, const TokenGrant & grant, std::chrono::steady_clock::time_point now)
{
    if (grant.expires_in <= kExpiryMargin)
        return;
    std::lock_guard lock(g_cache_mu);
    g_cache[keyOf(endpoint, credentials)] = {grant.access_token, now + grant.expires_in - kExpiryMargin};
}

void TokenCache::invalidate(const std::string & endpoint, const Credentials & credentials)
{
    std::lock_guard lock(g_cache_mu);
    g_cache.erase(keyOf(endpoint, credentials));
}

void TokenCache::clearForTesting()
{
    std::lock_guard lock(g_cache_mu);
    g_cache.clear();
}

Status acquireToken(
    const std::string & endpoint,
    const Credentials & credentials,
    const Transport & transport,
    bool use_cache,
    const Clock & clock,
    std::string & token)
{
    if (use_cache)
    {
        token = TokenCache::lookup(endpoint, credentials, clock());
        if (!token.empty())
            return {};
    }
    const auto requested_at = clock();
    TokenGrant grant;
    Status status
        = parseTokenResponse(postForm(endpoint, tokenRequestBody(credentials), transport), endpoint, credentials.client_id, grant);
    if (!status.ok())
        return status;
    if (use_cache)
        TokenCache::store(endpoint, credentials, grant, requested_at);
    token = grant.access_token;
    return {};
}

} // namespace firebolt::adbc::fb2
