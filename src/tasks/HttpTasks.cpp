//
//  Sapo Engine — `command` task: HTTP verbs plus capability dispatch.
//
//  Replaces the old `std::system("curl …")` path (plan §0, P0-1/P0-2). Requests go
//  through the injected `IHttpTransport`, so tests use the mock/replay transports
//  and a deployment can install its own (proxy, mTLS, retries at the transport
//  layer). Non-2xx responses become structured errors, which is what makes
//  `on_error` / `try` / `retry` meaningful for HTTP nodes (T2.6).
//
#include "tasks/Tasks.hpp"
#include "util/JsonPath.hpp"

#include <cctype>

using json = nlohmann::json;

namespace sapo::tasks {

    namespace {

        std::string upper(std::string value) {
            for (char &c : value) {
                if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
            }
            return value;
        }

        std::string asText(const json &value) {
            if (value.is_string()) return value.get<std::string>();
            return value.dump();
        }

    } // namespace

    sapo::http::Request CommandTask::buildRequest(const parser::CommandNode &node, ExecutionContext &execution) {
        sapo::http::Request request;
        const auto &http = *node.http_request;
        request.method = upper(node.command.substr(5));   // "http.post" → "POST"
        request.url = asText(runtime::ExpressionEvaluator::resolve(http.url, execution.scope()));
        request.timeout_ms = http.timeout.value_or(10000);
        request.follow_redirects = http.follow_redirects;

        if (http.headers.is_object()) {
            const json headers = execution.resolve(http.headers);
            for (auto it = headers.begin(); it != headers.end(); ++it) {
                if (it.value().is_null()) continue;   // an unresolved header is dropped, not sent as "null"
                request.headers[it.key()] = asText(it.value());
            }
        }
        if (http.query.is_object()) {
            const json query = execution.resolve(http.query);
            for (auto it = query.begin(); it != query.end(); ++it) {
                if (it.value().is_null()) continue;
                request.query[it.key()] = asText(it.value());
            }
        }
        if (!http.body.is_null()) {
            const json body = execution.resolve(http.body);
            request.body = body;
            if (!http.content_type.has_value() && !body.is_string()) {
                request.headers["Content-Type"] = "application/json";
            }
        }
        if (http.content_type.has_value()) request.headers["Content-Type"] = *http.content_type;

        if (http.auth.has_value()) {
            if (http.auth->type == "basic") {
                request.basic_auth_user = asText(runtime::ExpressionEvaluator::resolve(http.auth->username,
                                                                                       execution.scope()));
                request.basic_auth_password = asText(runtime::ExpressionEvaluator::resolve(http.auth->password,
                                                                                            execution.scope()));
            } else if (http.auth->type == "bearer") {
                request.bearer_token = asText(
                    runtime::ExpressionEvaluator::resolve(http.auth->token, execution.scope()));
            }
            // Secrets used for auth must never be echoed by the logger.
            auto redact = [&execution](const std::optional<std::string> &secret) {
                if (secret.has_value() && !secret->empty() && execution.services.logger) {
                    execution.services.logger->addSecret(*secret);
                }
            };
            redact(request.basic_auth_password);
            redact(request.bearer_token);
        }
        if (!request.url.empty() && request.url.rfind("http", 0) != 0) {
            throw runtime::SapoError(runtime::ErrorCode::Validation,
                                     "http_request.url must be an absolute http(s) URL, got '" + request.url + "'",
                                     json{{"url", request.url}}, node.id);
        }
        return request;
    }

    json CommandTask::performHttp(const parser::CommandNode &node, ExecutionContext &execution) {
        if (!node.http_request.has_value()) {
            throw runtime::SapoError(runtime::ErrorCode::Validation, "command node has no http_request block",
                                     json::object(), node.id);
        }
        const sapo::http::Request request = buildRequest(node, execution);
        if (execution.services.transport == nullptr) {
            throw runtime::SapoError(runtime::ErrorCode::Internal, "no HTTP transport is installed", json::object(),
                                     node.id);
        }

        if (execution.services.metrics) {
            execution.services.metrics->increment("sapo.http.requests");
        }
        const sapo::http::Response response = execution.services.transport->send(request);
        if (execution.services.metrics) {
            execution.services.metrics->observe("sapo.http.duration_ms", response.elapsed_ms);
        }

        if (!response.transport_error.empty()) {
            throw runtime::SapoError(runtime::ErrorCode::Http,
                                     "HTTP " + request.method + " " + request.describe() + " failed: " +
                                         response.transport_error,
                                     json{{"url", request.url}, {"transport_error", response.transport_error}}, node.id);
        }
        json envelope{{"status", response.status_code},
                      {"ok", response.ok()},
                      {"url", request.url},
                      {"method", request.method},
                      {"elapsed_ms", response.elapsed_ms}};
        if (response.body.empty()) {
            envelope["body"] = json();
        } else {
            json parsed = json::parse(response.body, nullptr, false);
            envelope["body"] = parsed.is_discarded() ? json(response.body) : parsed;
        }
        envelope["headers"] = response.headers.is_null() ? json::object() : response.headers;

        if (!response.ok()) {
            // Captured as a typed error so `$error.response` is available to catch
            // blocks, and so retry policies can look at the status.
            json data{{"status", response.status_code}, {"body", envelope["body"]}, {"url", request.url}};
            throw runtime::SapoError(runtime::ErrorCode::HttpStatus,
                                     "HTTP " + request.method + " " + request.url + " returned " +
                                         std::to_string(response.status_code),
                                     data, node.id);
        }
        return envelope;
    }

    ControlSignal CommandTask::execute(ExecutionContext &execution) const {
        const auto &node = execution.as<parser::CommandNode>();
        const bool is_http = node.command.rfind("http.", 0) == 0;

        json envelope;
        if (is_http) {
            envelope = performHttp(node, execution);
        } else {
            if (execution.services.capabilities == nullptr) {
                throw runtime::SapoError(runtime::ErrorCode::Internal, "no capability registry is installed",
                                         json::object(), node.id);
            }
            sapo::capabilities::CapabilityCall call;
            call.name = node.command;
            call.node_id = node.id;
            call.context = &execution.context;
            call.inputs = execution.resolve(node.inputs);
            const auto result = execution.services.capabilities->dispatch(call);
            if (!result.ok) {
                json data{{"capability", node.command}, {"error_code", result.error_code}};
                throw runtime::SapoError(runtime::ErrorCode::Capability,
                                         "capability '" + node.command + "' failed: " + result.error_message, data,
                                         node.id);
            }
            envelope = result.value;
        }

        // Output binding: one context key, a list of extracted paths, or a map.
        if (!node.output.empty()) {
            execution.write(node.output, envelope);
        }
        if (node.output_paths.has_value()) {
            for (const auto &path : *node.output_paths) {
                execution.write(path, extractField(envelope, path, execution));
            }
        }
        if (node.outputs.is_object()) {
            for (auto it = node.outputs.begin(); it != node.outputs.end(); ++it) {
                const std::string reference = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
                execution.write(it.key(), extractField(envelope, reference, execution));
            }
        }
        if (node.output.empty() && !node.output_paths.has_value() && node.outputs.empty()) {
            // Nothing bound: keep the response under the node id so a later
            // expression can still read it (`$fetch.body.id`).
            execution.write(node.id, envelope);
        }
        return Continue{};
    }

} // namespace sapo::tasks
