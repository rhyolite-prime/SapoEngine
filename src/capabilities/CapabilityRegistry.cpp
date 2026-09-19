//
// Created by Emmanuel Addo-Odame on 13/06/2026.
//
#include "capabilities/CapabilityRegistry.hpp"
#include "runtime/SapoError.hpp"

#include <algorithm>
#include <mutex>
#include <unordered_map>

using json = nlohmann::json;

namespace sapo::capabilities {

    bool ICapabilityProvider::supports(const std::string &name) const {
        for (const auto &prefix : namespaces()) {
            if (name == prefix) return true;
            if (name.size() > prefix.size() + 1 && name.compare(0, prefix.size(), prefix) == 0 &&
                name[prefix.size()] == '.') {
                return true;
            }
        }
        return false;
    }

    namespace {

        std::string rootNamespace(const std::string &name) {
            auto dot = name.find('.');
            return dot == std::string::npos ? name : name.substr(0, dot);
        }
    } // namespace

    void CapabilityRegistry::addProvider(ProviderPtr provider) {
        if (provider == nullptr) return;
        for (const auto &existing : m_providers) {
            if (existing->providerId() == provider->providerId()) {
                m_providers.erase(std::remove(m_providers.begin(), m_providers.end(), existing), m_providers.end());
                break;
            }
        }
        m_providers.push_back(std::move(provider));
    }

    void CapabilityRegistry::clear() { m_providers.clear(); }

    ICapabilityProvider *CapabilityRegistry::providerFor(const std::string &name) const {
        // 1) An exact advertised match always wins: the capability list is the
        //    provider's contract, so `data.uuid2` must not resolve to a provider
        //    that only implements `data.uuid`.
        for (const auto &provider : m_providers) {
            for (const auto &capability : provider->capabilities()) {
                if (capability.name == name) return provider.get();
            }
        }
        // 2) Otherwise only *dynamic* providers (no advertised list: plugins that
        //    answer arbitrary names in their namespace) may take the call.
        for (const auto &provider : m_providers) {
            if (provider->capabilities().empty() && provider->supports(name)) return provider.get();
        }
        return nullptr;
    }

    bool CapabilityRegistry::knows(const std::string &name) const { return providerFor(name) != nullptr; }

    bool CapabilityRegistry::has(const std::string &name) const {
        const CapabilityDescriptor *descriptor = describe(name);
        if (descriptor != nullptr) return !descriptor->deferred;
        // No descriptor: only a dynamic provider can serve the name.
        return providerFor(name) != nullptr;
    }

    const CapabilityDescriptor *CapabilityRegistry::describe(const std::string &name) const {
        const CapabilityDescriptor *fallback = nullptr;
        for (const auto &provider : m_providers) {
            for (const auto &descriptor : provider->capabilities()) {
                if (descriptor.name == name) {
                    if (!descriptor.deferred) return &descriptor;
                    fallback = &descriptor;
                }
            }
        }
        return fallback;
    }

    std::string CapabilityRegistry::describeNamespace(const std::string &name) { return rootNamespace(name); }

    std::vector<CapabilityDescriptor> CapabilityRegistry::all() const {
        std::vector<CapabilityDescriptor> out;
        for (const auto &provider : m_providers) {
            auto capabilities = provider->capabilities();
            out.insert(out.end(), capabilities.begin(), capabilities.end());
        }
        std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.name < b.name; });
        out.erase(std::unique(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.name == b.name; }),
                  out.end());
        return out;
    }

    std::vector<std::string> CapabilityRegistry::providerIds() const {
        std::vector<std::string> ids;
        for (const auto &provider : m_providers) ids.push_back(provider->providerId());
        return ids;
    }

    CapabilityResult CapabilityRegistry::dispatch(const CapabilityCall &call) const {
        ICapabilityProvider *provider = providerFor(call.name);
        if (provider == nullptr) {
            throw runtime::SapoError(runtime::ErrorCode::NotFound,
                                     "no capability provider is registered for '" + call.name + "'. Sapo does not fall "
                                     "back to a shell; register the provider or use http.* — see docs/CAPABILITIES.md",
                                     json{{"capability", call.name}, {"namespace", rootNamespace(call.name)}},
                                     call.node_id);
        }
        if (const CapabilityDescriptor *descriptor = describe(call.name); descriptor != nullptr && descriptor->deferred) {
            throw runtime::SapoError(runtime::ErrorCode::NotImplemented,
                                     "capability '" + call.name + "' is declared but intentionally deferred" +
                                         (descriptor->description.empty() ? "" : " (" + descriptor->description + ")"),
                                     json{{"capability", call.name}}, call.node_id);
        }
        return provider->execute(call);
    }

    std::vector<std::string> CapabilityRegistry::audit() const {
        std::vector<std::string> problems;
        std::unordered_map<std::string, std::string> seen;
        for (const auto &provider : m_providers) {
            if (provider->providerId().empty()) problems.push_back("provider with empty id registered");
            for (const auto &capability : provider->capabilities()) {
                auto [it, inserted] = seen.emplace(capability.name, provider->providerId());
                if (!inserted) {
                    problems.push_back("capability '" + capability.name + "' is registered twice ('" + it->second +
                                       "' and '" + provider->providerId() + "')");
                }
                if (!capability.deferred && capability.input_schema.is_null()) {
                    problems.push_back("capability '" + capability.name + "' has no input schema");
                }
                if (capability.description.empty()) {
                    problems.push_back("capability '" + capability.name + "' has no description");
                }
                if (!provider->supports(capability.name)) {
                    problems.push_back("provider '" + provider->providerId() + "' advertises '" + capability.name +
                                       "' but does not support its namespace");
                }
            }
            for (const auto &prefix : provider->namespaces()) {
                if (prefix.empty() || prefix.find('.') != std::string::npos) {
                    problems.push_back("provider '" + provider->providerId() + "' declares namespace '" + prefix +
                                       "'; namespaces must be a single bare segment");
                }
            }
        }
        return problems;
    }

    CapabilityRegistry &CapabilityRegistry::global() {
        static CapabilityRegistry registry;
        return registry;
    }

    // ---------------------------------------------------------------------
    // JSON-Schema subset
    // ---------------------------------------------------------------------
    namespace {

        bool typeMatches(const std::string &type, const json &value) {
            if (type == "object") return value.is_object();
            if (type == "array") return value.is_array();
            if (type == "string") return value.is_string();
            if (type == "number") return value.is_number();
            if (type == "integer") return value.is_number_integer() || (value.is_number() && value.get<double>() == std::floor(value.get<double>()));
            if (type == "boolean") return value.is_boolean();
            if (type == "null") return value.is_null();
            if (type == "any" || type.empty()) return true;
            return true; // unknown type keyword: don't invent rejections
        }

        void check(const json &schema, json &value, const std::string &path, std::vector<SchemaIssue> &issues) {
            if (!schema.is_object()) return;

            if (schema.contains("default") && value.is_null()) {
                value = schema["default"];
            }
            if (value.is_null() && !schema.contains("required")) {
                // A null is only meaningful when explicitly required below.
            }

            if (schema.contains("type") && !value.is_null()) {
                const json &types = schema["type"];
                bool ok = false;
                if (types.is_string()) {
                    ok = typeMatches(types.get<std::string>(), value);
                } else if (types.is_array()) {
                    for (const auto &type : types) {
                        if (type.is_string() && typeMatches(type.get<std::string>(), value)) {
                            ok = true;
                            break;
                        }
                    }
                }
                if (!ok) {
                    issues.push_back({path, "expected type " + types.dump() + ", got " + std::string(value.type_name())});
                    return;
                }
            }

            if (schema.contains("enum") && schema["enum"].is_array() && !value.is_null()) {
                const auto &allowed = schema["enum"];
                if (std::find(allowed.begin(), allowed.end(), value) == allowed.end()) {
                    issues.push_back({path, "value " + value.dump() + " is not one of " + allowed.dump()});
                }
            }

            if (value.is_object() && schema.contains("properties") && schema["properties"].is_object()) {
                const auto &properties = schema["properties"];
                for (auto it = properties.begin(); it != properties.end(); ++it) {
                    if (it.value().is_object() && it.value().contains("default") && !value.contains(it.key())) {
                        value[it.key()] = it.value()["default"];
                    }
                }
                for (auto it = value.begin(); it != value.end(); ++it) {
                    if (properties.contains(it.key())) {
                        check(properties[it.key()], it.value(), path.empty() ? it.key() : path + "." + it.key(), issues);
                    } else if (schema.contains("additionalProperties") && schema["additionalProperties"].is_boolean() &&
                               !schema["additionalProperties"].get<bool>()) {
                        issues.push_back({(path.empty() ? it.key() : path + "." + it.key()), "unknown property"});
                    }
                }
            }

            if (schema.contains("required") && schema["required"].is_array() && value.is_object()) {
                for (const auto &name : schema["required"]) {
                    if (!name.is_string()) continue;
                    if (!value.contains(name.get<std::string>())) {
                        issues.push_back({path.empty() ? name.get<std::string>() : path + "." + name.get<std::string>(),
                                          "required property is missing"});
                    }
                }
            }

            if (value.is_string() && schema.contains("minLength") && schema["minLength"].is_number_integer() &&
                value.get<std::string>().size() < schema["minLength"].get<size_t>()) {
                issues.push_back({path, "string is shorter than minLength"});
            }

            if (value.is_array() && schema.contains("items")) {
                size_t index = 0;
                for (auto &item : value) {
                    check(schema["items"], item, path + "[" + std::to_string(index++) + "]", issues);
                }
            }
        }

    } // namespace

    std::vector<SchemaIssue> validateSchema(const json &schema, json &value, const std::string &path) {
        std::vector<SchemaIssue> issues;
        check(schema, value, path, issues);
        return issues;
    }

    void validateOrThrow(const json &schema, json &value, const std::string &capability) {
        auto issues = validateSchema(schema, value);
        if (issues.empty()) return;
        std::string detail;
        for (const auto &issue : issues) {
            if (!detail.empty()) detail += "; ";
            detail += issue.describe();
        }
        throw runtime::SapoError(runtime::ErrorCode::Validation,
                                 "invalid inputs for capability '" + capability + "' — " + detail,
                                 json{{"capability", capability}, {"issues", issues.size()}});
    }

} // namespace sapo::capabilities
