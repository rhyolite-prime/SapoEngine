#include "UssdWorkflowController.hpp"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace drogon_sapo {

    std::shared_ptr<sapo::runtime::VirtualMachine> UssdWorkflowController::s_vm = nullptr;

    void UssdWorkflowController::setVirtualMachine(std::shared_ptr<sapo::runtime::VirtualMachine> vm) {
        s_vm = std::move(vm);
    }

    namespace {
        drogon::HttpResponsePtr jsonResponse(const json &body, drogon::HttpStatusCode code = drogon::k200OK) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(code);
            resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
            resp->setBody(body.dump());
            return resp;
        }

        drogon::HttpResponsePtr outcomeResponse(const sapo::runtime::ExecutionOutcome &outcome) {
            drogon::HttpStatusCode code = drogon::k200OK;
            if (outcome.status == "awaiting_input" || outcome.status == "suspended") {
                code = drogon::k202Accepted;
            } else if (outcome.status == "failed") {
                code = (outcome.error_code == "WORKFLOW_NOT_FOUND" || outcome.error_code == "SESSION_NOT_FOUND")
                           ? drogon::k404NotFound
                           : drogon::k500InternalServerError;
            }
            return jsonResponse(outcome.toJson(), code);
        }
    } // namespace

    drogon::Task<drogon::HttpResponsePtr> UssdWorkflowController::runWorkflow(drogon::HttpRequestPtr req,
                                                                              std::string workflow_id) {
        if (!s_vm) {
            co_return jsonResponse({{"error", "VirtualMachine not initialized"}}, drogon::k500InternalServerError);
        }

        json body = json::object();
        if (!req->getBody().empty()) {
            try {
                body = json::parse(req->getBody());
            } catch (const json::exception &e) {
                co_return jsonResponse({{"error", "Invalid JSON payload: " + std::string(e.what())}},
                                       drogon::k400BadRequest);
            }
        }

        sapo::runtime::StartSessionOptions options;
        options.session_id = body.value("session_id", "");
        options.correlation_id = body.value("correlation_id", "");
        json input = body.value("input", json::object());

        sapo::runtime::ExecutionOutcome outcome;
        // Offload execution to Drogon worker thread to avoid blocking IO threads
        co_await drogon::async_run([&] {
            outcome = s_vm->startSession(workflow_id, input, options);
        });

        co_return outcomeResponse(outcome);
    }

    drogon::Task<drogon::HttpResponsePtr> UssdWorkflowController::resumeWorkflow(drogon::HttpRequestPtr req,
                                                                                 std::string session_id) {
        if (!s_vm) {
            co_return jsonResponse({{"error", "VirtualMachine not initialized"}}, drogon::k500InternalServerError);
        }

        json input = json::object();
        if (!req->getBody().empty()) {
            try {
                auto body = json::parse(req->getBody());
                input = body.contains("input") ? body["input"] : body;
            } catch (const json::exception &e) {
                co_return jsonResponse({{"error", "Invalid JSON payload: " + std::string(e.what())}},
                                       drogon::k400BadRequest);
            }
        }

        sapo::runtime::ExecutionOutcome outcome;
        co_await drogon::async_run([&] {
            outcome = s_vm->resumeSession(session_id, input);
        });

        co_return outcomeResponse(outcome);
    }

    drogon::Task<drogon::HttpResponsePtr> UssdWorkflowController::getSession(drogon::HttpRequestPtr req,
                                                                             std::string session_id) {
        if (!s_vm) {
            co_return jsonResponse({{"error", "VirtualMachine not initialized"}}, drogon::k500InternalServerError);
        }

        std::optional<sapo::runtime::SessionSnapshot> snap;
        co_await drogon::async_run([&] {
            snap = s_vm->session(session_id);
        });

        if (!snap.has_value()) {
            co_return jsonResponse({{"error", "Session '" + session_id + "' not found"}}, drogon::k404NotFound);
        }

        co_return jsonResponse(snap->toJson(), drogon::k200OK);
    }

    drogon::Task<drogon::HttpResponsePtr> UssdWorkflowController::getMetrics(drogon::HttpRequestPtr req) {
        if (!s_vm) {
            co_return jsonResponse({{"error", "VirtualMachine not initialized"}}, drogon::k500InternalServerError);
        }

        json metrics_json;
        co_await drogon::async_run([&] {
            metrics_json = s_vm->metrics();
        });

        co_return jsonResponse(metrics_json, drogon::k200OK);
    }

    drogon::Task<drogon::HttpResponsePtr> UssdWorkflowController::getHealth(drogon::HttpRequestPtr req) {
        const bool healthy = (s_vm != nullptr && s_vm->running());
        json status = {
            {"status", healthy ? "pass" : "fail"},
            {"version", "0.4.0"},
            {"running", healthy}
        };
        co_return jsonResponse(status, healthy ? drogon::k200OK : drogon::k503ServiceUnavailable);
    }

    drogon::Task<drogon::HttpResponsePtr> UssdWorkflowController::handleUssd(drogon::HttpRequestPtr req) {
        if (!s_vm) {
            co_return jsonResponse({{"error", "VirtualMachine not initialized"}}, drogon::k500InternalServerError);
        }

        json payload;
        try {
            payload = json::parse(req->getBody());
        } catch (const json::exception &) {
            co_return jsonResponse({{"error", "Invalid JSON payload"}}, drogon::k400BadRequest);
        }

        // Standard gateway attributes (e.g. Nalo, Hubtel, Africa's Talking)
        const std::string session_id = payload.value("SessionId", payload.value("sessionId", ""));
        const std::string mobile     = payload.value("Mobile", payload.value("msisdn", ""));
        const std::string message    = payload.value("Message", payload.value("userData", ""));
        const std::string type       = payload.value("Type", payload.value("type", ""));
        const std::string service_id = payload.value("ServiceCode", payload.value("workflow_id", "default"));

        if (session_id.empty()) {
            co_return jsonResponse({{"error", "SessionId is required"}}, drogon::k400BadRequest);
        }

        const bool is_initiation = (type == "Initiation" || (!message.empty() && message.front() == '*'));

        sapo::runtime::ExecutionOutcome outcome;
        co_await drogon::async_run([&] {
            if (is_initiation) {
                sapo::runtime::StartSessionOptions options;
                options.session_id = session_id;
                options.correlation_id = session_id;

                json input = {
                    {"msisdn", mobile},
                    {"service_code", message},
                    {"session_id", session_id}
                };
                outcome = s_vm->startSession(service_id, input, options);
            } else {
                outcome = s_vm->resumeSession(session_id, message);
            }
        });

        // Translate ExecutionOutcome to Telco USSD response
        json ussd_response;
        ussd_response["SessionId"] = session_id;

        if (outcome.status == "awaiting_input") {
            ussd_response["Type"] = "Response"; // Keep USSD session active
            ussd_response["Message"] = outcome.prompt.value("message", "Please reply to continue:");
        } else if (outcome.status == "completed" || outcome.status == "terminated") {
            ussd_response["Type"] = "Release";  // Disconnect USSD session
            std::string final_msg = "Thank you.";
            if (outcome.output.is_object() && outcome.output.contains("message")) {
                final_msg = outcome.output["message"].get<std::string>();
            } else if (outcome.output.is_string()) {
                final_msg = outcome.output.get<std::string>();
            }
            ussd_response["Message"] = final_msg;
        } else {
            ussd_response["Type"] = "Release";
            ussd_response["Message"] = "An error occurred. Please try again later.";
        }

        co_return jsonResponse(ussd_response, drogon::k200OK);
    }

} // namespace drogon_sapo
