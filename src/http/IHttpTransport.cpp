#include "http/IHttpTransport.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <random>
#include <sstream>

#include "http/CprTransport.hpp"
#include "runtime/SapoError.hpp"
#include "util/Encoding.hpp"

namespace sapo::http {

    namespace {
        /// Normalizes a route key: "GET https://host/path" (query stripped).
        std::string routeKey(const std::string &method, const std::string &url) {
            std::string normalized = method;
            std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            normalized += " ";
            normalized += url;
            return normalized;
        }

        /// "GET https://x/y" → "GET https://x/y" with an upper-cased method.
        std::string normalizeRouteKey(const std::string &method_url) {
            const size_t space = method_url.find(' ');
            if (space == std::string::npos) return method_url;
            std::string method = method_url.substr(0, space);
            std::transform(method.begin(), method.end(), method.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return method + " " + method_url.substr(space + 1);
        }

        std::string stripQuery(const std::string &url) {
            const size_t pos = url.find('?');
            return pos == std::string::npos ? url : url.substr(0, pos);
        }

        std::string appendQuery(const std::string &url, const nlohmann::json &query) {
            if (!query.is_object() || query.empty()) return url;
            std::string out = url;
            out.push_back(out.find('?') == std::string::npos ? '?' : '&');
            bool first = true;
            for (auto it = query.begin(); it != query.end(); ++it) {
                if (!first) out.push_back('&');
                first = false;
                out += sapo::util::percentEncode(it.key());
                out.push_back('=');
                if (it.value().is_string()) out += sapo::util::percentEncode(it.value().get<std::string>());
                else out += sapo::util::percentEncode(it.value().dump());
            }
            return out;
        }
    } // namespace

    std::string Request::describe() const {
        std::ostringstream out;
        out << method << " " << appendQuery(url, query);
        return out.str();
    }

    nlohmann::json Response::jsonBody() const {
        if (body.empty()) return nullptr;
        try {
            return nlohmann::json::parse(body);
        } catch (...) {
            return nullptr;
        }
    }

    Response NullTransport::send(const Request &request) {
        throw runtime::SapoError(runtime::ErrorCode::NotImplemented,
                                 "no HTTP transport is compiled in — rebuild with -DSAPO_ENABLE_CPR=ON "
                                 "or inject a transport into TaskServices (last requested: " +
                                     request.describe() + ")");
    }

    // -----------------------------------------------------------------------
    // MockTransport
    // -----------------------------------------------------------------------
    MockTransport::MockTransport() = default;
    MockTransport::~MockTransport() = default;

    void MockTransport::on(const std::string &method_url, Response response) {
        auto captured = std::make_shared<Response>(std::move(response));
        on(method_url, [captured](const Request &) { return *captured; });
    }

    void MockTransport::on(const std::string &method_url, Handler handler) {
        std::scoped_lock lock(m_mutex);
        m_routes[normalizeRouteKey(method_url)] = handler;
    }

    void MockTransport::jsonResponse(const std::string &method_url, const nlohmann::json &payload, int status) {
        Response response;
        response.status_code = status;
        response.body = payload.dump();
        response.headers["content-type"] = "application/json";
        on(method_url, std::move(response));
    }

    void MockTransport::failThenSucceed(const std::string &method_url, int failures, Response success) {
        auto counter = std::make_shared<int>(0);
        Response failure_response;
        failure_response.status_code = 503;
        failure_response.body = R"({"error":"upstream unavailable"})";
        on(method_url, [counter, failures, success, failure_response](const Request &) mutable -> Response {
            ++(*counter);
            if (*counter <= failures) return failure_response;
            return success;
        });
    }

    Response MockTransport::send(const Request &request) {
        Handler handler;
        {
            std::scoped_lock lock(m_mutex);
            m_calls.push_back(request);
            // Most specific route first: method + full URL, then method + URL,
            // then the bare URL (method-agnostic).
            const std::string full = routeKey(request.method, appendQuery(request.url, request.query));
            const std::string base = routeKey(request.method, stripQuery(request.url));
            const std::string bare = stripQuery(request.url);
            auto it = m_routes.find(full);
            if (it == m_routes.end()) it = m_routes.find(base);
            if (it == m_routes.end()) it = m_routes.find(bare);
            if (it != m_routes.end()) handler = it->second;
        }

        if (!handler) {
            Response response;
            response.status_code = 404;
            response.body = nlohmann::json{{"error", "MockTransport: no canned route for " + request.describe()}}.dump();
            response.transport_error = "";
            response.headers["content-type"] = "application/json";
            return response;
        }
        Response response = handler(request);
        if (response.status_code == 0 && response.transport_error.empty()) response.status_code = 200;
        return response;
    }

    std::vector<Request> MockTransport::requests() const {
        std::scoped_lock lock(m_mutex);
        return m_calls;
    }

    size_t MockTransport::callCount() const {
        std::scoped_lock lock(m_mutex);
        return m_calls.size();
    }

    std::optional<Request> MockTransport::lastRequest() const {
        std::scoped_lock lock(m_mutex);
        if (m_calls.empty()) return std::nullopt;
        return m_calls.back();
    }

    void MockTransport::clear() {
        std::scoped_lock lock(m_mutex);
        m_calls.clear();
    }

    // -----------------------------------------------------------------------
    // RecordReplayTransport
    // -----------------------------------------------------------------------
    nlohmann::json RecordReplayTransport::requestToJson(const Request &request) {
        return {{"method", request.method},
                {"url", request.url},
                {"headers", request.headers},
                {"query", request.query},
                {"body", request.body.value_or(nlohmann::json(nullptr))},
                {"timeout_ms", request.timeout_ms}};
    }

    Request RecordReplayTransport::requestFromJson(const nlohmann::json &json) {
        Request request;
        request.method = json.value("method", "GET");
        request.url = json.value("url", "");
        request.headers = json.value("headers", nlohmann::json::object());
        request.query = json.value("query", nlohmann::json::object());
        if (json.contains("body") && !json["body"].is_null()) request.body = json["body"];
        request.timeout_ms = json.value("timeout_ms", 10000);
        return request;
    }

    nlohmann::json RecordReplayTransport::responseToJson(const Response &response) {
        return {{"status_code", response.status_code},
                {"body", response.body},
                {"headers", response.headers},
                {"elapsed_ms", response.elapsed_ms},
                {"transport_error", response.transport_error}};
    }

    Response RecordReplayTransport::responseFromJson(const nlohmann::json &json) {
        Response response;
        response.status_code = json.value("status_code", 0);
        response.body = json.value("body", "");
        response.headers = json.value("headers", nlohmann::json::object());
        response.elapsed_ms = json.value("elapsed_ms", 0.0);
        response.transport_error = json.value("transport_error", "");
        return response;
    }

    RecordReplayTransport::RecordReplayTransport(TransportPtr inner, std::string cassette_path, bool record)
        : m_inner(std::move(inner)), m_path(std::move(cassette_path)), m_record(record) {
        if (!m_record) {
            std::ifstream in(m_path);
            if (!in) {
                throw runtime::SapoError(runtime::ErrorCode::NotFound, "no cassette to replay from: " + m_path);
            }
            const auto document = nlohmann::json::parse(in, nullptr, false);
            if (document.is_discarded()) {
                throw runtime::SapoError(runtime::ErrorCode::Parse, "cassette is not valid JSON: " + m_path);
            }
            if (document.is_array()) m_tape = document;
        }
    }

    Response RecordReplayTransport::send(const Request &request) {
        if (m_record) {
            Response response = m_inner ? m_inner->send(request) : Response{};
            std::scoped_lock lock(m_mutex);
            m_tape.push_back({{"request", requestToJson(request)}, {"response", responseToJson(response)}});
            std::ofstream out(m_path, std::ios::trunc);
            out << nlohmann::json(m_tape).dump(2);
            return response;
        }

        std::scoped_lock lock(m_mutex);
        const std::string want = routeKey(request.method, appendQuery(request.url, request.query));
        for (size_t i = m_cursor; i < m_tape.size(); ++i) {
            const auto recorded = requestToJson(requestFromJson(m_tape[i].value("request", nlohmann::json::object())));
            if (routeKey(recorded.value("method", "GET"), appendQuery(recorded.value("url", ""),
                                                                      recorded.value("query", nlohmann::json::object()))) == want) {
                m_cursor = i + 1;
                return responseFromJson(m_tape[i].value("response", nlohmann::json::object()));
            }
        }
        if (m_cursor < m_tape.size()) {
            const auto entry = m_tape[m_cursor++];
            return responseFromJson(entry.value("response", nlohmann::json::object()));
        }
        throw runtime::SapoError(runtime::ErrorCode::NotFound,
                                 "cassette " + m_path + " has no recorded response for " + request.describe());
    }

    TransportPtr defaultTransport() {
        static TransportPtr transport = [] {
#if defined(SAPO_ENABLE_CPR)
            return makeCprTransport();
#else
            return std::make_shared<NullTransport>();
#endif
        }();
        return transport;
    }

} // namespace sapo::http
