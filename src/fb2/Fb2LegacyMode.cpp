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
#include "fb2/Fb2LegacyMode.h"
#include "FireboltUri.h"
#include "fb2/Fb2EngineResolver.h"
#include "fb2/Fb2TokenClient.h"

#include <map>
#include <mutex>
#include <tuple>

namespace firebolt::adbc::fb2
{

namespace
{

    Status invalidOption(std::string message)
    {
        return {ADBC_STATUS_INVALID_ARGUMENT, "FB2 SaaS mode: " + std::move(message)};
    }

    // firebolt.environment is spliced into the host the credentials are sent
    // to, so libcurl must read that URL's host back as exactly
    // id.<env>.firebolt.io: a '/', '?', '#', '@' or ':' in the value would
    // move the request somewhere else.
    bool environmentStaysOnFirebolt(const std::string & environment)
    {
        if (environment.empty())
            return false;
        const auto url = parseUrl("https://id." + environment + ".firebolt.io/oauth/token");
        return url && !url->has_userinfo && url->host == "id." + environment + ".firebolt.io";
    }

    bool isHttpUrl(const std::string & url_text)
    {
        const auto url = parseUrl(url_text);
        return url && !url->has_userinfo && (url->scheme == "http" || url->scheme == "https");
    }

    // Resolved endpoints, process-wide like the token cache.  The identity (client
    // ID, or the pre-acquired token) is in the key so one principal's successful
    // USE ENGINE never vouches for another's access.
    struct ResolvedEndpoints
    {
        std::string system_engine;
        std::string engine_endpoint;
    };
    using EndpointCacheKey = std::tuple<std::string, std::string, std::string, std::string, std::string>;
    std::mutex g_endpoint_cache_mutex;
    std::map<EndpointCacheKey, ResolvedEndpoints> g_endpoint_cache;

} // namespace

struct Fb2LegacyMode::Impl
{
    std::mutex mutex;
    std::map<std::string, std::string, std::less<>> option_values; // key → value, as set

    // Resolved by init().
    Credentials service_account; // empty when a pre-acquired token was given
    bool token_is_pre_acquired = false; // firebolt.token: never refreshed
    std::string token_endpoint;
    bool caching_enabled = true;
    Transport control_plane_transport;
    std::string bearer_token; // the bearer token in use
    EndpointCacheKey endpoint_cache_key;
    std::string engine_name;
    std::string system_engine_url; // the parent domain Firebolt-Update-Endpoint must stay in

    // A new token after a 401.  Caller holds `mu`.
    bool refreshBearerTokenLocked()
    {
        if (token_is_pre_acquired)
            return false;
        const std::string rejected = bearer_token;
        if (caching_enabled)
        {
            std::string cached = TokenCache::lookup(token_endpoint, service_account, std::chrono::steady_clock::now());
            if (!cached.empty() && cached != rejected)
            {
                bearer_token = cached; // another connection already refreshed it
                return true;
            }
            TokenCache::invalidate(token_endpoint, service_account);
        }
        std::string fresh;
        if (!acquireToken(token_endpoint, service_account, control_plane_transport, caching_enabled, std::chrono::steady_clock::now, fresh)
                 .ok())
            return false;
        bearer_token = fresh;
        return true;
    }

    // A control-plane request, retried once with a fresh token on 401: a cached
    // token may have been revoked since it was issued.
    template <class Request>
    Fb2HttpResult sendWithTokenRefresh(Request && request)
    {
        Fb2HttpResult response = request();
        if (response.transport_ok && response.status == 401 && refreshBearerTokenLocked())
            response = request();
        return response;
    }

    Status resolveEngineEndpoint(const InitInputs & inputs, std::string & engine_url)
    {
        const std::string account = *findOption("firebolt.account");
        const std::string api_endpoint
            = optionOrDefault("firebolt.api_endpoint", apiEndpointFor(optionOrDefault("firebolt.environment", "app")));

        const auto api_url = parseUrl(api_endpoint); // validateOptions() checked it; a built-in default always parses
        if (!api_url)
            return {ADBC_STATUS_INVALID_ARGUMENT, "FB2 SaaS mode: '" + api_endpoint + "' is not a valid control-plane URL"};

        Fb2HttpResult response = sendWithTokenRefresh(
            [&] { return getJson(engineUrlRequestUrl(api_endpoint, account), bearer_token, control_plane_transport); });
        if (Status status = parseEngineUrlResponse(response, account, api_url->scheme, system_engine_url); !status.ok())
            return status;

        // Status and headers only: the body is JSON_Compact because the system
        // engine's ArrowStream support is not a contract.
        const std::string system_query_url = system_engine_url + "/?output_format=JSON_Compact";
        if (!inputs.database.empty())
        {
            response = sendWithTokenRefresh([&] {
                return postSql(system_query_url, useStatement("DATABASE", inputs.database), bearer_token, control_plane_transport);
            });
            if (Status status = parseUseResponse(response, "database", inputs.database); !status.ok())
                return status;
        }
        response = sendWithTokenRefresh(
            [&] { return postSql(system_query_url, useStatement("ENGINE", engine_name), bearer_token, control_plane_transport); });
        if (Status status = parseUseResponse(response, "engine", engine_name); !status.ok())
            return status;
        if (response.update_endpoint.empty())
            return {ADBC_STATUS_IO, "FB2 SaaS mode: the system engine accepted USE ENGINE but returned no Firebolt-Update-Endpoint"};
        return resolveUpdateEndpoint(response.update_endpoint, system_engine_url, engine_url);
    }

    std::string optionOrDefault(std::string_view key, std::string fallback) const
    {
        const std::string * value = findOption(key);
        return value ? *value : fallback;
    }

    // The option's value as set, or nullptr when it was never set.
    const std::string * findOption(std::string_view key) const
    {
        auto it = option_values.find(key);
        return it == option_values.end() ? nullptr : &it->second;
    }

    // One credential under either of its two spellings.  Setting both is an error
    // even when they agree: two places to look for a secret is one too many.
    Status readCredential(std::string_view adbc_key, std::string_view firebolt_key, std::string & value) const
    {
        const std::string * adbc_value = findOption(adbc_key);
        const std::string * firebolt_value = findOption(firebolt_key);
        if (adbc_value && firebolt_value)
            return invalidOption(
                "set the credential as '" + std::string(adbc_key) + "' or as '" + std::string(firebolt_key) + "', not both");
        value = adbc_value ? *adbc_value : firebolt_value ? *firebolt_value : std::string();
        return {};
    }

    Status validateOptions(const InitInputs & inputs) const
    {
        if (!inputs.uri.empty())
            return invalidOption(
                "'uri' cannot be combined with firebolt.account: the engine URL is resolved from the account and engine names. "
                "Remove 'uri', or remove the FB2 options to connect to the uri directly");
        if (const std::string * account = findOption("firebolt.account"); !account || account->empty())
            return invalidOption(
                "firebolt.account is required (the Firebolt 2.0 account name). "
                "Service-account credentials without an account are not supported for Firebolt 3.0 yet");
        if (const std::string * engine = findOption("firebolt.engine"); !engine || engine->empty())
            return invalidOption("firebolt.engine is required: queries run on a user engine, not the system engine");

        std::string client_id, client_secret;
        if (Status status = readCredential("username", "firebolt.client_id", client_id); !status.ok())
            return status;
        if (Status status = readCredential("password", "firebolt.client_secret", client_secret); !status.ok())
            return status;
        if (!inputs.token.empty() && (!client_id.empty() || !client_secret.empty()))
            return invalidOption("firebolt.token cannot be combined with service-account credentials; pass one or the other");
        if (inputs.token.empty())
        {
            if (client_id.empty() && client_secret.empty())
                return invalidOption(
                    "service-account credentials are required: set 'username' (the client ID) and 'password' "
                    "(the client secret), or firebolt.token");
            if (client_id.empty())
                return invalidOption("'username' (the service-account client ID) is required with the secret");
            if (client_secret.empty())
                return invalidOption("'password' (the service-account client secret) is required with the client ID");
        }

        if (const std::string * environment = findOption("firebolt.environment"); environment && !environmentStaysOnFirebolt(*environment))
            return invalidOption("firebolt.environment must name a Firebolt environment such as 'app'; got '" + *environment + "'");
        for (std::string_view key : {"firebolt.api_endpoint", "firebolt.auth_endpoint"})
            if (const std::string * url = findOption(key); url && !isHttpUrl(*url))
                return invalidOption(std::string(key) + " must be an http:// or https:// URL with a host; got '" + *url + "'");
        if (const std::string * cache = findOption("firebolt.cache_connection"); cache && *cache != "true" && *cache != "false")
            return invalidOption(R"(firebolt.cache_connection must be exactly "true" or "false"; got ')" + *cache + "'");
        return {};
    }
};

Fb2LegacyMode::Fb2LegacyMode() : impl(std::make_unique<Impl>())
{
}

Fb2LegacyMode::~Fb2LegacyMode() = default;

Status Fb2LegacyMode::setOption(const std::string & key, const std::string & value)
{
    std::lock_guard lock(impl->mutex);
    impl->option_values[key] = value;
    return {};
}

bool Fb2LegacyMode::requested() const
{
    std::lock_guard lock(impl->mutex);
    return !impl->option_values.empty();
}

Status Fb2LegacyMode::init(const InitInputs & inputs, std::string & endpoint)
{
    std::lock_guard lock(impl->mutex);
    Impl & state = *impl;
    if (Status status = state.validateOptions(inputs); !status.ok())
        return status;

    state.control_plane_transport = {inputs.ca_bundle_path, inputs.timeout_sec};
    state.caching_enabled = state.optionOrDefault("firebolt.cache_connection", "true") == "true";
    state.token_endpoint
        = state.optionOrDefault("firebolt.auth_endpoint", tokenEndpointFor(state.optionOrDefault("firebolt.environment", "app")));

    if (!inputs.token.empty())
    {
        state.token_is_pre_acquired = true;
        state.bearer_token = inputs.token;
    }
    else
    {
        // validateOptions() guaranteed exactly one spelling of each.
        state.service_account
            = {state.optionOrDefault("username", state.optionOrDefault("firebolt.client_id", "")),
               state.optionOrDefault("password", state.optionOrDefault("firebolt.client_secret", ""))};
        if (Status status = acquireToken(
                state.token_endpoint,
                state.service_account,
                state.control_plane_transport,
                state.caching_enabled,
                std::chrono::steady_clock::now,
                state.bearer_token);
            !status.ok())
            return status;
    }

    state.engine_name = *state.findOption("firebolt.engine");
    state.endpoint_cache_key
        = {state.optionOrDefault("firebolt.api_endpoint", state.optionOrDefault("firebolt.environment", "app")),
           *state.findOption("firebolt.account"),
           state.engine_name,
           state.token_is_pre_acquired ? state.bearer_token : state.service_account.client_id,
           inputs.database};
    if (state.caching_enabled)
    {
        std::lock_guard cache_lock(g_endpoint_cache_mutex);
        if (auto it = g_endpoint_cache.find(state.endpoint_cache_key); it != g_endpoint_cache.end())
        {
            state.system_engine_url = it->second.system_engine;
            endpoint = it->second.engine_endpoint;
            return {};
        }
    }
    if (Status status = state.resolveEngineEndpoint(inputs, endpoint); !status.ok())
        return status;
    if (state.caching_enabled)
    {
        std::lock_guard cache_lock(g_endpoint_cache_mutex);
        g_endpoint_cache[state.endpoint_cache_key] = {state.system_engine_url, endpoint};
    }
    return {};
}

std::string Fb2LegacyMode::bearerToken()
{
    std::lock_guard lock(impl->mutex);
    return impl->bearer_token;
}

bool Fb2LegacyMode::reauthenticate()
{
    // Under the lock, so connections that hit 401 together trigger one exchange
    // after another rather than racing; the second finds the fresh token cached.
    std::lock_guard lock(impl->mutex);
    return impl->refreshBearerTokenLocked();
}

ResponseEffect Fb2LegacyMode::onResponse(CURL * handle, long http_code, bool success, std::string & error_message)
{
    std::lock_guard lock(impl->mutex);
    Impl & state = *impl;
    ResponseEffect effect;
    if (success)
    {
        // A USE ENGINE run by the caller moves this connection only.
        struct curl_header * header = nullptr;
        if (curl_easy_header(handle, "Firebolt-Update-Endpoint", 0, CURLH_HEADER, -1, &header) == CURLHE_OK && header && header->value)
        {
            Status status = resolveUpdateEndpoint(header->value, state.system_engine_url, effect.new_endpoint);
            if (!status.ok())
            {
                effect.new_endpoint.clear();
                effect.endpoint_error = status.message;
            }
        }
        return effect;
    }
    bool stopped = false;
    error_message = explainEngineError(http_code, error_message, stopped);
    if (stopped)
    {
        // The engine may come back elsewhere; resolve afresh next time.
        std::lock_guard cache_lock(g_endpoint_cache_mutex);
        g_endpoint_cache.erase(state.endpoint_cache_key);
    }
    return effect;
}

} // namespace firebolt::adbc::fb2
