#include "HttpClient.h"
#include "FireboltAdbcDatabase.h"

#include <cstring>
#include <sstream>
#include <stdexcept>

#include <curl/curl.h>

namespace firebolt::adbc
{

namespace
{

    std::vector<std::string> extractHeaderValues(const std::string & headers, const std::string & header_name)
    {
        std::vector<std::string> result;
        const std::string prefix = header_name + ":";
        size_t pos = 0;
        while (true)
        {
            size_t found = headers.find(prefix, pos);
            if (found == std::string::npos)
                break;
            size_t val_start = found + prefix.size();
            size_t val_end = headers.find('\n', val_start);
            if (val_end == std::string::npos)
                val_end = headers.size();
            std::string val = headers.substr(val_start, val_end - val_start);
            val.erase(0, val.find_first_not_of(" \t\r"));
            val.erase(val.find_last_not_of(" \t\r") + 1);
            if (!val.empty())
                result.push_back(val);
            pos = val_end;
        }
        return result;
    }

    std::unordered_map<std::string, std::string> parseUpdateParameters(const std::string & headers)
    {
        std::unordered_map<std::string, std::string> params;
        for (const auto & hval : extractHeaderValues(headers, "Firebolt-Update-Parameters"))
        {
            std::stringstream ss(hval);
            std::string pair;
            while (std::getline(ss, pair, ','))
            {
                pair.erase(0, pair.find_first_not_of(" \t"));
                pair.erase(pair.find_last_not_of(" \t") + 1);
                size_t eq = pair.find('=');
                if (eq != std::string::npos)
                {
                    std::string key = pair.substr(0, eq);
                    std::string val = pair.substr(eq + 1);
                    key.erase(0, key.find_first_not_of(" \t"));
                    key.erase(key.find_last_not_of(" \t") + 1);
                    val.erase(0, val.find_first_not_of(" \t"));
                    val.erase(val.find_last_not_of(" \t") + 1);
                    if (!key.empty())
                        params[key] = val;
                }
            }
        }
        return params;
    }

    std::vector<std::string> parseRemoveParameters(const std::string & headers)
    {
        std::vector<std::string> params;
        for (const auto & hval : extractHeaderValues(headers, "Firebolt-Remove-Parameters"))
        {
            std::stringstream ss(hval);
            std::string name;
            while (std::getline(ss, name, ','))
            {
                name.erase(0, name.find_first_not_of(" \t"));
                name.erase(name.find_last_not_of(" \t") + 1);
                if (!name.empty())
                    params.push_back(name);
            }
        }
        return params;
    }

    bool shouldResetSession(const std::string & headers)
    {
        return !extractHeaderValues(headers, "Firebolt-Reset-Session").empty();
    }

    std::string urlEncode(CURL * handle, const std::string & s)
    {
        char * encoded = curl_easy_escape(handle, s.c_str(), static_cast<int>(s.size()));
        std::string result(encoded ? encoded : s);
        if (encoded)
            curl_free(encoded);
        return result;
    }

} // namespace

HttpClient::HttpClient(const FireboltDatabase & db) : db_(db)
{
    handle_ = curl_easy_init();
    if (!handle_)
        throw std::runtime_error("Failed to initialize curl handle");
}

HttpClient::~HttpClient()
{
    if (handle_)
        curl_easy_cleanup(handle_);
}

size_t HttpClient::writeBodyCallback(char * ptr, size_t size, size_t nmemb, void * userdata)
{
    auto * body = static_cast<std::vector<uint8_t> *>(userdata);
    const size_t total = size * nmemb;
    body->insert(body->end(), ptr, ptr + total);
    return total;
}

size_t HttpClient::writeHeaderCallback(char * ptr, size_t size, size_t nmemb, void * userdata)
{
    auto * headers = static_cast<std::string *>(userdata);
    const size_t total = size * nmemb;
    headers->append(ptr, total);
    return total;
}

std::string HttpClient::buildUrl(const std::unordered_map<std::string, std::string> & session_params) const
{
    // Use the endpoint URL verbatim — do not append a trailing slash so that
    // paths specified by the user (e.g. "http://host/query") are preserved.
    std::string url = db_.url;

    bool first = (url.find('?') == std::string::npos);
    auto addParam = [&](const std::string & k, const std::string & v) {
        url += (first ? '?' : '&');
        first = false;
        url += k + "=" + urlEncode(handle_, v);
    };

    if (!db_.database.empty())
        addParam("database", db_.database);

    for (const auto & [k, v] : session_params)
        addParam(k, v);

    return url;
}

curl_slist * HttpClient::buildAuthHeader() const
{
    curl_slist * headers = nullptr;
    if (!db_.token.empty())
    {
        std::string auth = "Authorization: Bearer " + db_.token;
        headers = curl_slist_append(headers, auth.c_str());
    }
    headers = curl_slist_append(headers, "Firebolt-Protocol-Version: 2.4");
    return headers;
}

void HttpClient::parseResponseHeaders(HttpResponse & resp) const
{
    if (shouldResetSession(raw_headers_))
        resp.reset_session = true;
    resp.update_params = parseUpdateParameters(raw_headers_);
    resp.remove_params = parseRemoveParameters(raw_headers_);
}

void HttpClient::resetResponseState()
{
    raw_headers_.clear();
}

HttpResponse HttpClient::executeQuery(const std::string & sql, const std::unordered_map<std::string, std::string> & session_params)
{
    resetResponseState();
    HttpResponse resp;

    std::string url = buildUrl(session_params);
    // Append output_format for SELECT queries
    url += (url.find('?') == std::string::npos ? '?' : '&');
    url += "output_format=ArrowStream";

    curl_easy_reset(handle_);
    curl_easy_setopt(handle_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle_, CURLOPT_POST, 1L);
    curl_easy_setopt(handle_, CURLOPT_POSTFIELDS, sql.c_str());
    curl_easy_setopt(handle_, CURLOPT_POSTFIELDSIZE, static_cast<long>(sql.size()));
    curl_easy_setopt(handle_, CURLOPT_WRITEFUNCTION, writeBodyCallback);
    curl_easy_setopt(handle_, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(handle_, CURLOPT_HEADERFUNCTION, writeHeaderCallback);
    curl_easy_setopt(handle_, CURLOPT_HEADERDATA, &raw_headers_);
    curl_easy_setopt(handle_, CURLOPT_TIMEOUT, db_.timeout_sec);

    curl_slist * auth_headers = buildAuthHeader();
    // Content-Type: text/plain so the server knows body is raw SQL
    auth_headers = curl_slist_append(auth_headers, "Content-Type: text/plain; charset=utf-8");
    curl_easy_setopt(handle_, CURLOPT_HTTPHEADER, auth_headers);

    resp.curl_code = curl_easy_perform(handle_);
    curl_slist_free_all(auth_headers);

    curl_easy_getinfo(handle_, CURLINFO_RESPONSE_CODE, &resp.http_code);

    if (resp.curl_code != CURLE_OK)
        resp.error_message = curl_easy_strerror(resp.curl_code);
    else if (!resp.isSuccess())
        resp.error_message = std::string(resp.body.begin(), resp.body.end());

    parseResponseHeaders(resp);
    return resp;
}

HttpResponse HttpClient::executeInsert(
    const std::string & sql, std::vector<uint8_t> arrow_ipc_bytes, const std::unordered_map<std::string, std::string> & session_params)
{
    resetResponseState();
    HttpResponse resp;

    std::string url = buildUrl(session_params);

    curl_easy_reset(handle_);
    curl_easy_setopt(handle_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle_, CURLOPT_WRITEFUNCTION, writeBodyCallback);
    curl_easy_setopt(handle_, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(handle_, CURLOPT_HEADERFUNCTION, writeHeaderCallback);
    curl_easy_setopt(handle_, CURLOPT_HEADERDATA, &raw_headers_);
    curl_easy_setopt(handle_, CURLOPT_TIMEOUT, db_.timeout_sec);

    // Set auth header
    curl_slist * auth_headers = buildAuthHeader();
    curl_easy_setopt(handle_, CURLOPT_HTTPHEADER, auth_headers);

    // Build multipart/form-data using curl_mime
    curl_mime * mime = curl_mime_init(handle_);

    // SQL part
    curl_mimepart * sql_part = curl_mime_addpart(mime);
    curl_mime_name(sql_part, "sql");
    curl_mime_type(sql_part, "text/plain; charset=utf-8");
    curl_mime_data(sql_part, sql.c_str(), sql.size());

    // Arrow IPC part
    curl_mimepart * arrow_part = curl_mime_addpart(mime);
    curl_mime_name(arrow_part, "data.arrow");
    curl_mime_filename(arrow_part, "data.arrow");
    curl_mime_type(arrow_part, "application/octet-stream");
    curl_mime_data(arrow_part, reinterpret_cast<const char *>(arrow_ipc_bytes.data()), arrow_ipc_bytes.size());

    curl_easy_setopt(handle_, CURLOPT_MIMEPOST, mime);

    resp.curl_code = curl_easy_perform(handle_);
    curl_slist_free_all(auth_headers);
    curl_mime_free(mime);

    curl_easy_getinfo(handle_, CURLINFO_RESPONSE_CODE, &resp.http_code);

    if (resp.curl_code != CURLE_OK)
        resp.error_message = curl_easy_strerror(resp.curl_code);
    else if (!resp.isSuccess())
        resp.error_message = std::string(resp.body.begin(), resp.body.end());

    parseResponseHeaders(resp);
    return resp;
}

} // namespace firebolt::adbc
