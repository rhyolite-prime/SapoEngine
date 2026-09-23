#pragma once

#include <drogon/HttpClient.h>
#include <drogon/drogon.h>
#include "http/IHttpTransport.hpp"

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace drogon_sapo {

    /**
     * @brief Bridges Sapo's `IHttpTransport` seam to Drogon's native `HttpClient`.
     *
     * Blueprint `http.*` tasks run synchronously on Sapo worker threads.
     * This transport parses the destination URL, gets or creates a Drogon
     * HttpClient for the host/origin, and sends the request using Drogon's
     * synchronous `sendRequest()` client API.
     *
     * Benefits:
     * - Eliminates the need for CPR and external libcurl in SapoEngine.
     * - Reuses Drogon's connection pool, TLS context, and thread model.
     * - Respects timeouts and custom headers configured in the blueprint.
     */
    class DrogonHttpTransport final : public sapo::http::IHttpTransport {
    public:
        DrogonHttpTransport() = default;
        ~DrogonHttpTransport() override = default;

        [[nodiscard]] sapo::http::Response send(const sapo::http::Request &request) override;
        [[nodiscard]] std::string name() const override { return "drogon"; }

    private:
        [[nodiscard]] std::shared_ptr<drogon::HttpClient> getClient(const std::string &origin);

        std::mutex m_clients_mutex;
        std::map<std::string, std::shared_ptr<drogon::HttpClient>> m_clients;
    };

} // namespace drogon_sapo
