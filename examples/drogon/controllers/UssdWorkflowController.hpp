#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/Coroutine.h>
#include "runtime/VirtualMachine.hpp"

namespace drogon_sapo {

    /**
     * @brief Drogon HTTP Controller exposing Sapo Engine workflows and USSD orchestration.
     *
     * Demonstrates:
     * - Asynchronous offloading with `drogon::async_run` to prevent blocking Drogon I/O loops.
     * - Interactive USSD session flows (initiation vs continuation).
     * - RESTful workflow executions and session resume.
     * - Mapping Sapo `ExecutionOutcome` to standard HTTP status codes.
     */
    class UssdWorkflowController : public drogon::HttpController<UssdWorkflowController> {
    public:
        METHOD_LIST_BEGIN
        // REST API endpoints
        ADD_METHOD_TO(UssdWorkflowController::runWorkflow, "/api/v1/workflows/{1}/runs", drogon::Post);
        ADD_METHOD_TO(UssdWorkflowController::resumeWorkflow, "/api/v1/sessions/{1}/input", drogon::Post);
        ADD_METHOD_TO(UssdWorkflowController::getSession, "/api/v1/sessions/{1}", drogon::Get);
        ADD_METHOD_TO(UssdWorkflowController::getMetrics, "/metrics", drogon::Get);
        ADD_METHOD_TO(UssdWorkflowController::getHealth, "/healthz", drogon::Get);

        // USSD Gateway single-endpoint (Nalo / Hubtel / standard telco style)
        ADD_METHOD_TO(UssdWorkflowController::handleUssd, "/ussd", drogon::Post);
        METHOD_LIST_END

        // Dependency injection of VirtualMachine instance
        static void setVirtualMachine(std::shared_ptr<sapo::runtime::VirtualMachine> vm);

        drogon::Task<drogon::HttpResponsePtr> runWorkflow(drogon::HttpRequestPtr req, std::string workflow_id);
        drogon::Task<drogon::HttpResponsePtr> resumeWorkflow(drogon::HttpRequestPtr req, std::string session_id);
        drogon::Task<drogon::HttpResponsePtr> getSession(drogon::HttpRequestPtr req, std::string session_id);
        drogon::Task<drogon::HttpResponsePtr> getMetrics(drogon::HttpRequestPtr req);
        drogon::Task<drogon::HttpResponsePtr> getHealth(drogon::HttpRequestPtr req);

        drogon::Task<drogon::HttpResponsePtr> handleUssd(drogon::HttpRequestPtr req);

    private:
        static std::shared_ptr<sapo::runtime::VirtualMachine> s_vm;
    };

} // namespace drogon_sapo
