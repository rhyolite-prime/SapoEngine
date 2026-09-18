#include "ExpressionEvaluator.hpp"
#include "third_party/exprtk.hpp"
#include <algorithm>
#include <cctype>

namespace sapo::runtime {

nlohmann::json ExpressionEvaluator::resolveValue(const std::string &value, const RuntimeContext &ctx) {
    if (value.empty() || value.find('$') == std::string::npos) {
        return value; // Plain literal — no resolution needed
    }

    // Strip '$' sigils so ExprTk sees clean identifiers
    std::string expr_string = value;
    expr_string.erase(std::remove(expr_string.begin(), expr_string.end(), '$'),
                      expr_string.end());

    // Build a symbol table from all numeric context variables
    exprtk::symbol_table<double> symbol_table;
    auto all_vars = ctx.getAllVariables();
    std::map<std::string, double> numeric_vars;

    for (auto &[k, v] : all_vars.items()) {
        if (v.is_number()) {
            numeric_vars[k] = v.get<double>();
        }
    }
    for (auto &[k, v] : numeric_vars) {
        symbol_table.add_variable(k, v);
    }
    symbol_table.add_constants();

    exprtk::expression<double> expression;
    expression.register_symbol_table(symbol_table);
    exprtk::parser<double> expr_parser;

    if (expr_parser.compile(expr_string, expression)) {
        return expression.value(); // Resolved as a number
    }

    // ExprTk failed — treat as an exact variable lookup ($key -> context["key"])
    if (value[0] == '$') {
        bool is_exact = true;
        for (size_t i = 1; i < value.length(); ++i) {
            if (!std::isalnum(value[i]) && value[i] != '_') {
                is_exact = false;
                break;
            }
        }
        if (is_exact) {
            std::string context_key = value.substr(1);
            auto var = ctx.getVariable(context_key);
            if (var.has_value()) {
                return var.value();
            }
        }
    }

    // String interpolation for embedded variables (e.g. "https://.../$post_id")
    std::string result_str = value;
    size_t pos = 0;
    while ((pos = result_str.find('$', pos)) != std::string::npos) {
        size_t end_pos = pos + 1;
        while (end_pos < result_str.length() && (std::isalnum(result_str[end_pos]) || result_str[end_pos] == '_')) {
            end_pos++;
        }
        std::string var_name = result_str.substr(pos + 1, end_pos - pos - 1);
        auto var_val = ctx.getVariable(var_name);

        if (var_val.has_value()) {
            std::string replacement;
            if (var_val->is_string()) {
                replacement = var_val->get<std::string>();
            } else if (var_val->is_number_integer()) {
                replacement = std::to_string(var_val->get<long long>());
            } else if (var_val->is_number_float()) {
                replacement = std::to_string(var_val->get<double>());
                // optional: strip trailing zeros for floats if desired
            } else {
                replacement = var_val->dump();
            }
            result_str.replace(pos, end_pos - pos, replacement);
            pos += replacement.length();
        } else {
            // Leave unresolved variable as-is
            pos = end_pos;
        }
    }

    return result_str;
}

nlohmann::json ExpressionEvaluator::resolveMap(const std::map<std::string, std::string> &map,
                                               const RuntimeContext &ctx) {
    nlohmann::json result = nlohmann::json::object();
    for (const auto &[key, val] : map) {
        result[key] = resolveValue(val, ctx);
    }
    return result;
}

} // namespace sapo::runtime
