//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//

#pragma once

#include "Context.hpp"
#include <string>
#include <chrono>

namespace sapo::runtime {

    enum class VMStatus {
        Idle,
        Running,
        Success,
        ExecutionError,
        ParseError
    };

    struct VMMetrics {
        std::chrono::milliseconds execution_duration{0};
        size_t steps_executed{0};
    };

    class VirtualMachine {
    public:
        VirtualMachine() = default;

        /**
         * @brief Loads and executes a raw JSON Sapo DSL string blueprint.
         * @param json_blueprint The structural JSON task pipeline string.
         * @param context The shared variable scope tracking repository.
         * @return VMStatus The terminating operational condition of the engine.
         */
        VMStatus runBlueprint(const std::string& json_blueprint, RuntimeContext& context);

        /**
         * @brief Resumes execution of a loaded blueprint from a suspended state.
         * @param json_blueprint The structural JSON task pipeline string.
         * @param context The shared variable scope tracking repository, previously restored from Redis.
         * @return VMStatus The terminating operational condition of the engine.
         */
        VMStatus resumeBlueprint(const std::string& json_blueprint, RuntimeContext& context);

        // Metadata Accessors
        [[nodiscard]] VMStatus getStatus() const { return m_status; }
        [[nodiscard]] VMMetrics getMetrics() const { return m_metrics; }

    private:
        VMStatus m_status{VMStatus::Idle};
        VMMetrics m_metrics;
    };

} // namespace sapo