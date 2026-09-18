#pragma once

#include <string>
#include <map>
#include <nlohmann/json.hpp>
#include "runtime/Context.hpp"

namespace sapo::runtime {

class ExpressionEvaluator {
public:
    /**
     * Resolves a single string value that may contain $var references
     * or inline math expressions (e.g. "$totalAmount + 1.8").
     * Falls back to the raw string if ExprTk cannot compile it.
     */
    static nlohmann::json resolveValue(const std::string &value, const RuntimeContext &ctx);

    /**
     * Resolves an entire string-map into a JSON object, applying
     * resolveValue() on each value field.
     */
    static nlohmann::json resolveMap(const std::map<std::string, std::string> &map, const RuntimeContext &ctx);
};

} // namespace sapo::runtime
