//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//

#pragma once

#include <mutex>
#include <shared_mutex>
#include <string>
#include <nlohmann/json.hpp>
#include <optional>

namespace sapo::runtime {

    class RuntimeContext {
    public:
        RuntimeContext() = default;

        // Prevent accidental copying to ensure thread/state integrity across execution workers
        RuntimeContext(const RuntimeContext&) = delete;
        RuntimeContext& operator=(const RuntimeContext&) = delete;

        /**
         * @brief Sets a variable inside the execution scope environment.
         * @param key The variable descriptor path (e.g., "userData" or "total_count")
         * @param value The nlohmann::json value payload data block
         */
        void setVariable(const std::string& key, const nlohmann::json& value) {
            std::unique_lock lock(m_context_mutex); // Blocks all readers and writers
            if (value.is_null()) {
                m_variables.erase(key);
            } else {
                m_variables[key] = value;
            }
        }

        /**
         * @brief Retrieves a variable from the execution scope environment.
         * @param key The variable token lookup name
         * @return std::optional containing the JSON value if found, or std::nullopt
         */
        std::optional<nlohmann::json> getVariable(const std::string& key) const {
            std::shared_lock lock(m_context_mutex); // Multiple threads can hold this at once
            auto it = m_variables.find(key);
            if (it != m_variables.end()) {
                return it.value();
            }
            return std::nullopt;
        }

        /**
         * @brief Evaluates whether a specific variable key exists in memory.
         */
        bool hasVariable(const std::string& key) const {
            std::shared_lock lock(m_context_mutex);
            return m_variables.contains(key);
        }

        /**
         * @brief Returns a copy of all variables in the environment.
         */
        nlohmann::json getAllVariables() const {
            std::shared_lock lock(m_context_mutex);
            return m_variables;
        }

        /**
         * @brief Serializes the entire workflow memory footprint into a JSON string.
         * This is vital when saving the state of a workflow to a database during a 'wait' task.
         */
        std::string serializeState() const {
            // Shared lock: Multiple threads can serialize/read data simultaneously!
            std::shared_lock<std::shared_mutex> lock(m_context_mutex);
            return m_variables.dump();
        }

        /**
         * @brief Hydrates and restores the memory footprint from a previously stored state.
         */
        void deserializeState(const std::string& json_state) {
            std::unique_lock lock(m_context_mutex);
            m_variables = nlohmann::json::parse(json_state);
        }

        /**
         * @brief Hydrates the memory state from an exported string using an exclusive write lock.
         */
        void loadState(const std::string& json_str) {
            // Exclusive lock: Overwriting data requires absolute isolation
            std::unique_lock<std::shared_mutex> lock(m_context_mutex);
            if (!json_str.empty()) {
                m_variables = nlohmann::json::parse(json_str);
            }
        }

        /**
         * @brief Wipes out the current scope environment clean.
         */
        void clear() {
            std::unique_lock lock(m_context_mutex);
            m_variables.clear();
        }

    private:
        // Internal state storage engine
        nlohmann::json m_variables = nlohmann::json::object();

        // Mutable read/write lock ensuring concurrency safety across parallel branches
        mutable std::shared_mutex m_context_mutex;
    };


}