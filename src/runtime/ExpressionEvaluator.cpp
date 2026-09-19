#include "runtime/ExpressionEvaluator.hpp"

#include "runtime/SapoError.hpp"
#include "runtime/expressions/Value.hpp"
#include "util/JsonPath.hpp"

#include <algorithm>
#include <cctype>

namespace sapo::runtime {

    namespace {
        bool g_strict_by_default = true;

        bool isNamespaced(const std::string &name, std::string &ns, std::string &key) {
            const size_t dot = name.find('.');
            if (dot == std::string::npos || dot == 0 || dot + 1 >= name.size()) return false;
            ns = name.substr(0, dot);
            key = name.substr(dot + 1);
            static const std::vector<std::string> kKnown = {"env", "secret", "config", "provider"};
            return std::find(kKnown.begin(), kKnown.end(), ns) != kKnown.end();
        }
    } // namespace

    bool ContextResolver::resolve(const std::string &name, nlohmann::json &out) const {
        if (m_scope.locals.is_object() && m_scope.locals.contains(name)) {
            out = m_scope.locals.at(name);
            return true;
        }
        // A local object can also carry the dotted remainder: `item.qty`.
        const size_t dot = name.find('.');
        if (m_scope.locals.is_object() && dot != std::string::npos) {
            const std::string head = name.substr(0, dot);
            if (m_scope.locals.contains(head)) {
                const nlohmann::json base = m_scope.locals.at(head);
                if (auto found = sapo::util::getPath(base, name.substr(dot + 1)); found.has_value()) {
                    out = *found;
                    return true;
                }
            }
        }

        if (m_scope.context != nullptr) {
            if (auto value = m_scope.context->getByPath(name); value.has_value()) {
                out = *value;
                return true;
            }
        }

        if (m_scope.bindings != nullptr) {
            std::string ns, key;
            if (isNamespaced(name, ns, key)) {
                nlohmann::json resolved;
                if (m_scope.bindings->lookup(ns, key, resolved)) {
                    out = std::move(resolved);
                    return true;
                }
                return false;
            }
        }
        return false;
    }

    void ExpressionEvaluator::setStrictByDefault(bool strict) { g_strict_by_default = strict; }

    bool ExpressionEvaluator::strictByDefault() { return g_strict_by_default; }

    nlohmann::json ExpressionEvaluator::resolveValue(const std::string &value, const RuntimeContext &ctx) {
        EvaluationScope scope(ctx);
        scope.options.strict = g_strict_by_default;
        scope.bindings = defaultBindingProvider().get();
        return resolveValue(value, scope);
    }

    nlohmann::json ExpressionEvaluator::resolveValue(const std::string &value, const EvaluationScope &scope) {
        if (value.empty()) return nlohmann::json(value);
        try {
            return parser::Expression(value).resolve(ContextResolver(scope), scope.options);
        } catch (const SapoError &error) {
            throw SapoError(error.code(),
                            scope.node_id.empty() ? error.message()
                                                  : "[" + scope.node_id + "] " + error.message(),
                            error.data(), scope.node_id);
        }
    }

    nlohmann::json ExpressionEvaluator::resolveMap(const std::map<std::string, std::string> &map,
                                                   const RuntimeContext &ctx) {
        nlohmann::json result = nlohmann::json::object();
        for (const auto &[key, val] : map) result[key] = resolveValue(val, ctx);
        return result;
    }

    nlohmann::json ExpressionEvaluator::resolveMap(const parser::ExpressionObject &map, const EvaluationScope &scope) {
        nlohmann::json result = nlohmann::json::object();
        for (const auto &[key, expression] : map) {
            if (expression.empty()) {
                result[key] = nlohmann::json("");
                continue;
            }
            result[key] = resolve(expression, scope);
        }
        return result;
    }

    nlohmann::json ExpressionEvaluator::resolveMap(const nlohmann::json &object, const EvaluationScope &scope) {
        if (!object.is_object()) return object;
        nlohmann::json result = nlohmann::json::object();
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (it.value().is_string()) result[it.key()] = resolveValue(it.value().get<std::string>(), scope);
            else if (it.value().is_object()) result[it.key()] = resolveMap(it.value(), scope);
            else result[it.key()] = it.value();
        }
        return result;
    }

    nlohmann::json ExpressionEvaluator::evaluate(const std::string &expression, const EvaluationScope &scope) {
        if (expression.empty()) return nullptr;
        try {
            return parser::Expression::fromExpression(expression).evaluate(ContextResolver(scope), scope.options);
        } catch (const SapoError &error) {
            throw SapoError(error.code(),
                            scope.node_id.empty() ? error.message() : "[" + scope.node_id + "] " + error.message(),
                            error.data(), scope.node_id);
        }
    }

    nlohmann::json ExpressionEvaluator::evaluate(const std::string &expression, const RuntimeContext &ctx, bool strict) {
        EvaluationScope scope(ctx);
        scope.options.strict = strict;
        scope.bindings = defaultBindingProvider().get();
        return evaluate(expression, scope);
    }

    bool ExpressionEvaluator::evaluateBool(const std::string &expression, const EvaluationScope &scope) {
        return sapo::v::truthy(evaluate(expression, scope));
    }

    bool ExpressionEvaluator::evaluateBool(const std::string &expression, const RuntimeContext &ctx, bool strict) {
        return sapo::v::truthy(evaluate(expression, ctx, strict));
    }

    nlohmann::json ExpressionEvaluator::evaluate(const parser::Expression &expression, const EvaluationScope &scope) {
        if (expression.empty()) return nullptr;
        try {
            return expression.evaluate(ContextResolver(scope), scope.options);
        } catch (const SapoError &error) {
            throw SapoError(error.code(),
                            scope.node_id.empty() ? error.message() : "[" + scope.node_id + "] " + error.message(),
                            error.data(), scope.node_id);
        }
    }

    nlohmann::json ExpressionEvaluator::resolve(const parser::Expression &expression, const EvaluationScope &scope) {
        if (expression.empty()) return nlohmann::json();
        try {
            return expression.resolve(ContextResolver(scope), scope.options);
        } catch (const SapoError &error) {
            throw SapoError(error.code(),
                            scope.node_id.empty() ? error.message() : "[" + scope.node_id + "] " + error.message(),
                            error.data(), scope.node_id);
        }
    }

    bool ExpressionEvaluator::evaluateBool(const parser::Expression &expression, const EvaluationScope &scope) {
        return sapo::v::truthy(evaluate(expression, scope));
    }

} // namespace sapo::runtime
