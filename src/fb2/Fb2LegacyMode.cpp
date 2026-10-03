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

// FB2 SaaS (Legacy) mode. Keep isolated; see AGENTS.md.
#include "fb2/Fb2LegacyMode.h"
#include "FireboltUri.h"
#include "fb2/Fb2TokenClient.h"

#include <map>
#include <mutex>

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

Status Fb2LegacyMode::init(const InitInputs & inputs, std::string & /*endpoint*/)
{
    std::lock_guard lock(impl->mutex);
    Impl & state = *impl;
    if (Status status = state.validateOptions(inputs); !status.ok())
        return status;

    state.control_plane_transport = {inputs.ca_bundle_path, inputs.timeout_sec};
    state.caching_enabled = state.optionOrDefault("firebolt.cache_connection", "true") == "true";
    state.token_endpoint
        = state.optionOrDefault("firebolt.auth_endpoint", tokenEndpointFor(state.optionOrDefault("firebolt.environment", "app")));

    // Assigned on both branches: an Init retried after a failure may switch from
    // a pre-acquired token to credentials.
    state.token_is_pre_acquired = !inputs.token.empty();
    if (state.token_is_pre_acquired)
    {
        state.bearer_token = inputs.token;
    }
    else
    {
        // validateOptions() guaranteed exactly one spelling of each.
        state.service_account
            = {state.optionOrDefault("username", state.optionOrDefault("firebolt.client_id", "")),
               state.optionOrDefault("password", state.optionOrDefault("firebolt.client_secret", ""))};
        if (Status status = acquireToken(
                state.token_endpoint, state.service_account, state.control_plane_transport, state.caching_enabled, state.bearer_token);
            !status.ok())
            return status;
    }
    return {ADBC_STATUS_NOT_IMPLEMENTED, "FB2 SaaS mode: engine resolution is not implemented yet"};
}

std::string Fb2LegacyMode::bearerToken()
{
    std::lock_guard lock(impl->mutex);
    return impl->bearer_token;
}

bool Fb2LegacyMode::reauthenticate(const std::string & rejected_token)
{
    // Under the lock, so connections that hit 401 together trigger one exchange
    // after another rather than racing; the second finds the token it sent
    // already replaced.
    std::lock_guard lock(impl->mutex);
    Impl & state = *impl;
    if (state.token_is_pre_acquired)
        return false;
    if (state.bearer_token != rejected_token)
        return true; // another connection of this database already refreshed it
    if (state.caching_enabled)
    {
        std::string cached = TokenCache::lookup(state.token_endpoint, state.service_account, std::chrono::steady_clock::now());
        if (!cached.empty() && cached != rejected_token)
        {
            state.bearer_token = cached; // another database already refreshed it
            return true;
        }
        TokenCache::invalidate(state.token_endpoint, state.service_account);
    }
    std::string fresh;
    if (!acquireToken(state.token_endpoint, state.service_account, state.control_plane_transport, state.caching_enabled, fresh).ok())
        return false;
    state.bearer_token = fresh;
    return true;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static): uses the mode's state once engines are resolved
ResponseEffect Fb2LegacyMode::onResponse(CURL * /*handle*/, long /*http_code*/, bool /*success*/, std::string & /*error_message*/)
{
    return {};
}

} // namespace firebolt::adbc::fb2
