#include "DrogonHttpTransport.hpp"
#include "util/Encoding.hpp"

#include <chrono>
#include <regex>

namespace drogon_sapo {

    namespace {
        drogon::HttpMethod methodFromString(const std::string &method) {
            if (method == "GET") return drogon::Get;
            if (method == "POST") return drogon::Post;
            if (method == "PUT") return drogon::Put;
            if (method == "DELETE") return drogon::Delete;
            if (method == "PATCH") return drogon::Patch;
            if (method == "HEAD") return drogon::Head;
            if (method == "OPTIONS") return drogon::Options;
            return drogon::Get;
        }

        struct ParsedUrl {
            std::string origin;
            std::string path_and_query;
        };

        ParsedUrl parseUrl(const std::string &url) {
            ParsedUrl result;
            static const std::regex url_regex(R"(^(https?://[^/]+)(/.*)?$)");
            std::smatch match;
            if (std::regex_match(url, match, url_regex)) {
                result.origin = match[1].str();
                result.path_and_query = match[2].matched ? match[2].str() : "/";
            } else {
                result.origin = url;
                result.path_and_query = "/";
            }
            return result;
        }
    } // namespace

    std::shared_ptr<drogon::HttpClient> DrogonHttpTransport::getClient(const std::string &origin) {
        std::lock_guard<std::mutex> lock(m_clients_mutex);
        auto it = m_clients.find(origin);
        if (it != m_clients.end()) {
            return it->second;
        }
        auto client = drogon::HttpClient::newHttpClient(origin);
        m_clients[origin] = client;
        return client;
    }

    sapo::http::Response DrogonHttpTransport::send(const sapo::http::Request &request) {
        sapo::http::Response out;
        const auto start_time = std::chrono::steady_clock::now();

        auto parsed = parseUrl(request.url);
        auto client = getClient(parsed.origin);

        auto drogon_req = drogon::HttpRequest::newHttpRequest();
        drogon_req->setMethod(methodFromString(request.method));

        // Construct query string if present
        std::string full_path = parsed.path_and_query;
        if (!request.query.empty() && request.query.is_object()) {
            std::string query_str;
            for (auto it = request.query.begin(); it != request.query.end(); ++it) {
                if (!query_str.empty()) query_str += "&";
                query_str += it.key() + "=";
                query_str += it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
            }
            if (!query_str.empty()) {
                full_path += (full_path.find('?') == std::string::npos ? "?" : "&") + query_str;
            }
        }
        drogon_req->setPath(full_path);

        // Headers
        if (request.headers.is_object()) {
            for (auto it = request.headers.begin(); it != request.headers.end(); ++it) {
                const std::string val = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
                drogon_req->addHeader(it.key(), val);
            }
        }

        // Bearer Token
        if (request.bearer_token.has_value()) {
            drogon_req->addHeader("Authorization", "Bearer " + *request.bearer_token);
        }

        // Basic Auth
        if (request.basic_auth_user.has_value()) {
            const std::string user = *request.basic_auth_user;
            const std::string pass = request.basic_auth_password.value_or("");
            drogon_req->addHeader("Authorization", "Basic " + sapo::util::base64Encode(user + ":" + pass));
        }

        // Body
        if (request.body.has_value()) {
            if (request.body->is_string()) {
                drogon_req->setBody(request.body->get<std::string>());
            } else {
                drogon_req->setBody(request.body->dump());
                if (!request.headers.contains("Content-Type") && !request.headers.contains("content-type")) {
                    drogon_req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
                }
            }
        }

        const double timeout_sec = static_cast<double>(request.timeout_ms) / 1000.0;
        const auto [req_result, resp] = client->sendRequest(drogon_req, timeout_sec);

        const auto end_time = std::chrono::steady_clock::now();
        out.elapsed_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

        if (req_result != drogon::ReqResult::Ok || !resp) {
            switch (req_result) {
                case drogon::ReqResult::Timeout:
                    out.transport_error = "Connection or read timeout after " + std::to_string(request.timeout_ms) + "ms";
                    break;
                case drogon::ReqResult::BadServerAddress:
                    out.transport_error = "Bad server address: " + parsed.origin;
                    break;
                default:
                    out.transport_error = "HTTP transport failure: result code " + std::to_string(static_cast<int>(req_result));
                    break;
            }
            return out;
        }

        out.status_code = static_cast<int>(resp->getStatusCode());
        out.body = std::string(resp->getBody());

        for (const auto &[key, val] : resp->getHeaders()) {
            out.headers[key] = val;
        }

        return out;
    }

} // namespace drogon_sapo
