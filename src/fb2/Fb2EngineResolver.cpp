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
#include "fb2/Fb2EngineResolver.h"
#include "FireboltUri.h"

#include <nlohmann/json.hpp>

#include <arpa/inet.h>

namespace firebolt::adbc::fb2
{

namespace
{

    constexpr const char * kPrefix = "FB2 SaaS mode: ";

    std::string excerpt(const std::string & body)
    {
        constexpr size_t kMax = 300;
        return body.size() <= kMax ? body : body.substr(0, kMax) + "…";
    }

    // The engine's own description of a failed statement, else the raw body.
    std::string serverText(const std::string & body)
    {
        const nlohmann::json doc = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
        if (doc.is_object() && doc.contains("errors") && doc["errors"].is_array() && !doc["errors"].empty() && doc["errors"][0].is_object()
            && doc["errors"][0].contains("description") && doc["errors"][0]["description"].is_string())
            return doc["errors"][0]["description"].get<std::string>();
        return excerpt(body);
    }

    // libcurl brackets an IPv6 literal; an IPv4 one is whatever inet_pton accepts.
    bool isIpLiteral(const std::string & host)
    {
        in_addr v4{};
        return (!host.empty() && host.front() == '[') || inet_pton(AF_INET, host.c_str(), &v4) == 1;
    }

    Status refused(const std::string & value, const std::string & why)
    {
        return {ADBC_STATUS_IO, std::string(kPrefix) + "refused Firebolt-Update-Endpoint '" + value + "': " + why};
    }

} // namespace

std::string apiEndpointFor(const std::string & environment)
{
    return "https://api." + environment + ".firebolt.io";
}

std::string engineUrlRequestUrl(const std::string & api_endpoint, const std::string & account)
{
    std::string base = api_endpoint;
    while (!base.empty() && base.back() == '/')
        base.pop_back();
    return base + "/web/v3/account/" + formEncode(account) + "/engineUrl";
}

Status parseEngineUrlResponse(const Fb2HttpResult & result, const std::string & account, const std::string & scheme, std::string & url)
{
    if (!result.transport_ok)
        return {ADBC_STATUS_IO, std::string(kPrefix) + "could not reach the Firebolt control plane: " + result.transport_error};
    if (result.status == 401 || result.status == 403)
        return {
            ADBC_STATUS_UNAUTHENTICATED,
            std::string(kPrefix) + "the control plane refused the token while looking up account '" + account + "' (HTTP "
                + std::to_string(result.status) + ")"};
    if (result.status == 404)
        return {
            ADBC_STATUS_NOT_FOUND,
            std::string(kPrefix) + "account '" + account
                + "' was not found, or this service account has no access to it. Check firebolt.account"};
    if (result.status < 200 || result.status >= 300)
        return {
            ADBC_STATUS_IO,
            std::string(kPrefix) + "looking up account '" + account + "' failed with HTTP " + std::to_string(result.status) + ": "
                + excerpt(result.body)};

    const nlohmann::json doc = nlohmann::json::parse(result.body, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_object() || !doc.contains("engineUrl") || !doc["engineUrl"].is_string() || doc["engineUrl"].get<std::string>().empty())
        return {ADBC_STATUS_IO, std::string(kPrefix) + "the control plane returned no engineUrl for account '" + account + "'"};
    const std::string host = doc["engineUrl"].get<std::string>();
    // Like Firebolt-Update-Endpoint, the host normally comes without a scheme.
    std::string candidate = host;
    if (!parseUrl(candidate))
        candidate = scheme + "://" + host;
    if (!parseUrl(candidate))
        return {ADBC_STATUS_IO, std::string(kPrefix) + "the control plane returned an engineUrl that is not a host: '" + host + "'"};
    url = candidate;
    return {};
}

std::string useStatement(const char * object, const std::string & name)
{
    std::string quoted;
    for (char c : name)
        quoted += (c == '"') ? std::string("\"\"") : std::string(1, c);
    return std::string("USE ") + object + " \"" + quoted + "\"";
}

Status parseUseResponse(const Fb2HttpResult & result, const char * kind, const std::string & name)
{
    if (!result.transport_ok)
        return {ADBC_STATUS_IO, std::string(kPrefix) + "could not reach the system engine: " + result.transport_error};
    if (result.status == 401 || result.status == 403)
        return {
            ADBC_STATUS_UNAUTHENTICATED,
            std::string(kPrefix) + "the system engine refused the token (HTTP " + std::to_string(result.status) + ")"};
    if (result.status == 400 || result.status == 404)
        return {
            ADBC_STATUS_NOT_FOUND, std::string(kPrefix) + kind + " '" + name + "': " + serverText(result.body) + " Check firebolt." + kind};
    if (result.status < 200 || result.status >= 300)
        return {
            ADBC_STATUS_IO,
            std::string(kPrefix) + "USE " + kind + " failed with HTTP " + std::to_string(result.status) + ": " + serverText(result.body)};
    return {};
}

Status resolveUpdateEndpoint(const std::string & header_value, const std::string & system_engine_url, std::string & url)
{
    const auto system = parseUrl(system_engine_url);
    if (!system)
        return refused(header_value, "the system engine URL '" + system_engine_url + "' is not valid");

    // The header normally has no scheme (`host?engine=name`), which libcurl
    // does not accept as a URL; it is then read under the system engine's.
    std::string candidate = header_value;
    auto target = parseUrl(candidate);
    if (!target)
    {
        candidate = system->scheme + "://" + header_value;
        target = parseUrl(candidate);
    }
    if (!target)
        return refused(header_value, "not a URL");
    if (target->has_userinfo)
        return refused(header_value, "it carries user information");
    if (target->scheme != system->scheme)
        return refused(header_value, "it changes the scheme from " + system->scheme + " to " + target->scheme);
    if (target->port != system->port)
        return refused(header_value, "it changes the port");

    bool allowed = target->host == system->host;
    if (!allowed && !isIpLiteral(system->host))
    {
        // A sibling of the system engine: same parent domain, one label in front.
        const auto dot = system->host.find('.');
        if (dot != std::string::npos && dot + 1 < system->host.size())
        {
            const std::string parent = system->host.substr(dot); // ".api.us-east-1.app.firebolt.io"
            const std::string & to = target->host;
            allowed = to.size() > parent.size() && to.compare(to.size() - parent.size(), parent.size(), parent) == 0
                && to.substr(0, to.size() - parent.size()).find('.') == std::string::npos;
        }
    }
    if (!allowed)
        return refused(header_value, "host '" + target->host + "' is outside the system engine's domain (" + system->host + ")");

    url = candidate;
    return {};
}

std::string explainEngineError(long http_code, const std::string & message, bool & stopped)
{
    stopped = false;
    if (http_code != 404)
        return message;
    if (message.find("it is stopped") != std::string::npos)
    {
        stopped = true;
        return message + " " + kPrefix
            + "start the engine (START ENGINE on the system engine, or from the Firebolt UI) and retry. Engines that auto-stop when "
              "idle are refused this way too.";
    }
    if (message.find("Output format 'ArrowStream' is not supported") != std::string::npos)
        return message + " " + kPrefix
            + "the engine does not serve Arrow results; this driver needs an engine on Firebolt version 5 or later.";
    return message;
}

} // namespace firebolt::adbc::fb2
