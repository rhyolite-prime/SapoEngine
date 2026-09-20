//
//  cpr implementation of IHttpTransport. Compiled only when SAPO_ENABLE_CPR=ON.
//
#include "http/CprTransport.hpp"

#if defined(SAPO_ENABLE_CPR)

#include <algorithm>
#include <chrono>

#include <cpr/cpr.h>

#include "runtime/SapoError.hpp"
#include "util/Encoding.hpp"

namespace sapo::http {

    namespace {
        cpr::Header toHeaders(const nlohmann::json &json_headers) {
            cpr::Header headers;
            if (!json_headers.is_object()) return headers;
            for (auto it = json_headers.begin(); it != json_headers.end(); ++it) {
                if (it.value().is_string()) headers[it.key()] = it.value().get<std::string>();
                else headers[it.key()] = it.value().dump();
            }
            return headers;
        }

        cpr::Parameters toParameters(const nlohmann::json &json_query) {
            cpr::Parameters parameters;
            if (!json_query.is_object()) return parameters;
            for (auto it = json_query.begin(); it != json_query.end(); ++it) {
                if (it.value().is_string()) parameters.Add({it.key(), it.value().get<std::string>()});
                else parameters.Add({it.key(), it.value().dump()});
            }
            return parameters;
        }
    } // namespace

    Response CprTransport::send(const Request &request) {
        cpr::Session session;
        session.SetUrl(cpr::Url{request.url});
        session.SetHeader(toHeaders(request.headers));
        session.SetParameters(toParameters(request.query));
        if (request.body.has_value()) {
            session.SetBody(cpr::Body{request.body->is_string() ? request.body->get<std::string>()
                                                                 : request.body->dump()});
        }
        if (request.timeout_ms > 0) session.SetTimeout(std::chrono::milliseconds(request.timeout_ms));
        // cpr::Redirect replaces the removed Session::SetEnableRedirects (cpr >= 1.11).
        cpr::Redirect redirect;
        redirect.follow = request.follow_redirects;
        session.SetRedirect(redirect);
        if (request.basic_auth_user.has_value()) {
            // Session::SetAuth/cpr::Authentication replaced SetAuthentication/cpr::Authenticate in cpr 1.8;
            // the mode enum is the top-level cpr::AuthMode in cpr 1.10+ (incl. Homebrew's 1.12.x).
            session.SetAuth(cpr::Authentication{
                request.basic_auth_user.value_or(""),
                request.basic_auth_password.value_or(""),
                cpr::AuthMode::BASIC});
        }
        if (request.bearer_token.has_value()) {
            cpr::Header bearer = toHeaders(request.headers);
            bearer["Authorization"] = "Bearer " + request.bearer_token.value();
            session.SetHeader(bearer);
        }

        const auto started = std::chrono::steady_clock::now();
        cpr::Response raw;
        const std::string method = [&] {
            std::string m = request.method;
            std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return m;
        }();
        if (method == "GET") raw = session.Get();
        else if (method == "POST") raw = session.Post();
        else if (method == "PUT") raw = session.Put();
        else if (method == "PATCH") raw = session.Patch();
        else if (method == "DELETE") raw = session.Delete();
        else if (method == "HEAD") raw = session.Head();
        else throw runtime::SapoError(runtime::ErrorCode::Validation, "unsupported HTTP method '" + request.method + "'");

        Response response;
        response.status_code = raw.status_code;
        response.body = raw.text;
        response.elapsed_ms = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - started).count() / 1000.0;
        response.headers = nlohmann::json::object();
        for (const auto &[key, value] : raw.header) response.headers[key] = value;
        if (raw.error) response.transport_error = raw.error.message;
        return response;
    }

    TransportPtr makeCprTransport() { return std::make_shared<CprTransport>(); }

} // namespace sapo::http

#endif // SAPO_ENABLE_CPR
