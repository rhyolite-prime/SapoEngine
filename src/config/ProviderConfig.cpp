//
//  Sapo Engine — provider configuration store.
//
#include "config/ProviderConfig.hpp"
#include "runtime/SapoError.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <fstream>
#include <sstream>

using json = nlohmann::json;

namespace sapo::config {

    namespace {

        /// Dotted path walk (`smtp.host`) over objects and arrays.
        std::optional<json> walk(const json &root, const std::string &path) {
            if (path.empty()) return root;
            json current = root;
            size_t cursor = 0;
            while (cursor <= path.size()) {
                const size_t dot = path.find('.', cursor);
                const std::string segment = path.substr(cursor, dot == std::string::npos ? std::string::npos : dot - cursor);
                if (segment.empty()) return std::nullopt;
                if (current.is_object()) {
                    auto it = current.find(segment);
                    if (it == current.end()) return std::nullopt;
                    current = *it;
                } else if (current.is_array()) {
                    try {
                        const size_t index = std::stoul(segment);
                        if (index >= current.size()) return std::nullopt;
                        current = current[index];
                    } catch (...) {
                        return std::nullopt;
                    }
                } else {
                    return std::nullopt;
                }
                if (dot == std::string::npos) break;
                cursor = dot + 1;
            }
            return current;
        }

        std::string upper(std::string value) {
            for (char &c : value) {
                if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
            }
            return value;
        }

        class ConfigBindingProvider final : public runtime::IBindingProvider {
        public:
            explicit ConfigBindingProvider(std::shared_ptr<const json> document, std::string origin)
                : m_document(std::move(document)), m_origin(std::move(origin)) {}

            [[nodiscard]] bool lookup(std::string_view ns, std::string_view key, json &out) const override {
                const std::string name(ns);
                if (name == "config") {
                    if (auto value = walkValue("config", std::string(key)); value.has_value()) {
                        out = *value;
                        return true;
                    }
                    return false;
                }
                if (name == "secret") {
                    const std::string secret_name(key);
                    if (auto value = walkValue("secrets", secret_name); value.has_value()) {
                        out = *value;
                        return true;
                    }
                    const std::string env_name = "SAPO_SECRET_" + upper(secret_name);
                    if (const char *env = std::getenv(env_name.c_str()); env != nullptr) {
                        out = std::string(env);
                        return true;
                    }
                    // A dotted secret path may address a nested provider block.
                    if (auto value = walkValue("secrets", secret_name, /*allow_provider_fallback=*/true);
                        value.has_value()) {
                        out = *value;
                        return true;
                    }
                    return false;
                }
                return false;
            }

            [[nodiscard]] std::vector<std::string> namespaces() const override { return {"config", "secret"}; }

        private:
            [[nodiscard]] std::optional<json> walkValue(const char *root_key, const std::string &path,
                                                        bool allow_provider_fallback = false) const {
                auto root_it = m_document->find(root_key);
                if (root_it != m_document->end()) {
                    if (auto value = walk(*root_it, path); value.has_value()) return value;
                    if (root_key == std::string("config") && path.find('.') == std::string::npos) {
                        // Top-level `config` may be given inline at the document root.
                        if (auto direct = m_document->find(path); direct != m_document->end()) return *direct;
                    }
                }
                if (allow_provider_fallback && root_key == std::string("secrets")) {
                    if (auto providers = m_document->find("providers");
                        providers != m_document->end() && providers->is_object()) {
                        if (auto value = walk(*providers, path); value.has_value()) return value;
                    }
                }
                return std::nullopt;
            }

            std::shared_ptr<const json> m_document;
            std::string m_origin;
        };

    } // namespace

    json expandIndirections(const json &value, const ProviderConfigStore &store, std::vector<std::string> &problems,
                            const std::string &path) {
        if (value.is_object()) {
            if (value.size() == 1) {
                if (auto it = value.find("$secret"); it != value.end() && it->is_string()) {
                    const std::string name = it->get<std::string>();
                    auto resolved = store.secret(name);
                    if (!resolved.has_value()) {
                        problems.push_back("secret '" + name + "' referenced at " +
                                           (path.empty() ? "<root>" : path) + " is not defined (add it to `secrets` or "
                                           "set SAPO_SECRET_" + [&] {
                                               std::string upper_name = name;
                                               for (char &c : upper_name) {
                                                   if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
                                               }
                                               for (char &c : upper_name) {
                                                   if (c == '.' || c == '-') c = '_';
                                               }
                                               return upper_name;
                                           }() +
                                           ")");
                        return value;
                    }
                    return *resolved;
                }
                if (auto it = value.find("$env"); it != value.end() && it->is_string()) {
                    const std::string name = it->get<std::string>();
                    if (const char *env = std::getenv(name.c_str()); env != nullptr) return std::string(env);
                    problems.push_back("environment variable '" + name + "' referenced at " +
                                       (path.empty() ? "<root>" : path) + " is not set");
                    return value;
                }
                if (auto it = value.find("$include"); it != value.end() && it->is_string()) {
                    problems.push_back("$include is not supported (" + it->get<std::string>() + ")");
                    return value;
                }
            }
            json out = json::object();
            for (auto it = value.begin(); it != value.end(); ++it) {
                out[it.key()] = expandIndirections(it.value(), store, problems,
                                                    path.empty() ? it.key() : path + "." + it.key());
            }
            return out;
        }
        if (value.is_array()) {
            json out = json::array();
            for (size_t index = 0; index < value.size(); ++index) {
                out.push_back(expandIndirections(value[index], store, problems, path + "[" + std::to_string(index) + "]"));
            }
            return out;
        }
        return value;
    }

    ProviderConfigStore::ProviderConfigStore(json document, std::string origin)
        : m_document(std::make_shared<const json>(std::move(document))), m_origin(std::move(origin)) {
        if (!m_document->is_object()) {
            throw runtime::SapoError(runtime::ErrorCode::Parse, "a config document must be a JSON object");
        }
    }

    ProviderConfigStore ProviderConfigStore::load(const std::string &path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw runtime::SapoError(runtime::ErrorCode::Parse, "cannot open config file '" + path + "'");
        }
        std::stringstream buffer;
        buffer << in.rdbuf();
        json document;
        try {
            document = json::parse(buffer.str());
        } catch (const std::exception &error) {
            throw runtime::SapoError(runtime::ErrorCode::Parse,
                                    "config file '" + path + "' is not valid JSON: " + error.what());
        }
        return ProviderConfigStore(std::move(document), path);
    }

    std::optional<ProviderConfigStore> ProviderConfigStore::discover(const std::string &start_directory) {
        std::error_code error;
        auto directory = std::filesystem::absolute(start_directory.empty() ? "." : start_directory, error);
        for (size_t hop = 0; hop < 8 && !error; ++hop) {
            const auto candidate = directory / "sapo-config.json";
            if (std::filesystem::exists(candidate, error)) {
                return ProviderConfigStore::load(candidate.string());
            }
            if (!std::filesystem::exists(directory, error) || directory == directory.root_path()) break;
            directory = directory.parent_path();
        }
        return std::nullopt;
    }

    bool ProviderConfigStore::hasProvider(const std::string &id) const {
        auto providers = m_document->find("providers");
        return providers != m_document->end() && providers->is_object() && providers->contains(id);
    }

    json ProviderConfigStore::providerConfig(const std::string &id) const {
        if (auto providers = m_document->find("providers"); providers != m_document->end() && providers->is_object()) {
            if (auto entry = providers->find(id); entry != providers->end()) {
                if (entry->is_object() && entry->contains("config")) return (*entry)["config"];
                return *entry;
            }
        }
        return json();
    }

    std::string ProviderConfigStore::providerType(const std::string &id) const {
        if (auto providers = m_document->find("providers"); providers != m_document->end() && providers->is_object()) {
            if (auto entry = providers->find(id); entry != providers->end() && entry->is_object()) {
                return entry->value("type", entry->value("provider", ""));
            }
        }
        return {};
    }

    bool ProviderConfigStore::providerDeferred(const std::string &id) const {
        if (auto providers = m_document->find("providers"); providers != m_document->end() && providers->is_object()) {
            if (auto entry = providers->find(id); entry != providers->end() && entry->is_object()) {
                return entry->value("deferred", false);
            }
        }
        return false;
    }

    std::vector<std::string> ProviderConfigStore::providerIds() const {
        std::vector<std::string> out;
        if (auto providers = m_document->find("providers"); providers != m_document->end() && providers->is_object()) {
            for (auto it = providers->begin(); it != providers->end(); ++it) out.push_back(it.key());
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    std::optional<json> ProviderConfigStore::setting(const std::string &dotted_path) const {
        if (auto config = m_document->find("config"); config != m_document->end()) {
            if (auto value = walk(*config, dotted_path); value.has_value()) return value;
        }
        if (dotted_path.find('.') == std::string::npos) {
            if (auto direct = m_document->find(dotted_path); direct != m_document->end()) return *direct;
        }
        return std::nullopt;
    }

    std::optional<json> ProviderConfigStore::secret(const std::string &name) const {
        if (auto secrets = m_document->find("secrets"); secrets != m_document->end() && secrets->is_object()) {
            if (auto value = secrets->find(name); value != secrets->end()) return *value;
            if (auto nested = walk(*secrets, name); nested.has_value()) return nested;
        }
        std::string env_name = "SAPO_SECRET_" + upper(name);
        for (char &c : env_name) {
            if (c == '.' || c == '-') c = '_';
        }
        if (const char *env = std::getenv(env_name.c_str()); env != nullptr) return std::string(env);
        return std::nullopt;
    }

    json ProviderConfigStore::engine() const {
        if (auto engine = m_document->find("engine"); engine != m_document->end() && engine->is_object()) return *engine;
        return json::object();
    }

    std::vector<json> ProviderConfigStore::dataSourceDeclarations() const {
        std::vector<json> out;
        if (auto sources = m_document->find("data_sources"); sources != m_document->end() && sources->is_array()) {
            for (const auto &entry : *sources) out.push_back(entry);
        }
        return out;
    }

    std::vector<std::string> ProviderConfigStore::redactList() const {
        std::vector<std::string> secrets;
        auto collect = [&](const json &value, const char *key, auto &&self) -> void {
            if (!value.is_object()) return;
            for (auto it = value.begin(); it != value.end(); ++it) {
                const bool secretish = key != nullptr && std::string(key) == "secrets";
                const std::string name = it.key();
                const bool looks_secret = secretish || name.find("password") != std::string::npos ||
                                          name.find("secret") != std::string::npos || name.find("token") != std::string::npos ||
                                          name.find("api_key") != std::string::npos || name.find("apikey") != std::string::npos;
                if (it.value().is_string() && looks_secret && !it.value().get<std::string>().empty()) {
                    secrets.push_back(it.value().get<std::string>());
                } else if (it.value().is_object()) {
                    self(it.value(), name.c_str(), self);
                }
            }
        };
        if (auto secrets_node = m_document->find("secrets"); secrets_node != m_document->end()) {
            collect(*secrets_node, "secrets", collect);
        }
        if (auto providers = m_document->find("providers"); providers != m_document->end()) {
            collect(*providers, "providers", collect);
        }
        std::sort(secrets.begin(), secrets.end());
        secrets.erase(std::unique(secrets.begin(), secrets.end()), secrets.end());
        return secrets;
    }

    std::vector<std::string> ProviderConfigStore::validate() const {
        std::vector<std::string> problems;
        if (!m_document->is_object()) {
            problems.push_back("config root must be an object");
            return problems;
        }
        for (auto it = m_document->begin(); it != m_document->end(); ++it) {
            static const std::vector<std::string> kKnown = {"version", "engine", "secrets", "config", "providers",
                                                            "data_sources", "workflows", "plugins"};
            if (std::find(kKnown.begin(), kKnown.end(), it.key()) == kKnown.end()) {
                problems.push_back("unknown top-level config section '" + it.key() + "'");
            }
        }
        if (auto providers = m_document->find("providers"); providers != m_document->end()) {
            if (!providers->is_object()) {
                problems.push_back("`providers` must be an object keyed by provider id");
            } else {
                for (auto it = providers->begin(); it != providers->end(); ++it) {
                    if (!it.value().is_object()) {
                        problems.push_back("provider '" + it.key() + "' must be an object");
                        continue;
                    }
                    if (const std::string type = it.value().value("type", ""); type.empty()) {
                        problems.push_back("provider '" + it.key() + "' declares no `type`");
                    }
                    std::vector<std::string> local;
                    (void) expandIndirections(it.value(), *this, local, "providers." + it.key());
                    problems.insert(problems.end(), local.begin(), local.end());
                }
            }
        }
        if (auto engine = m_document->find("engine"); engine != m_document->end() && engine->is_object()) {
            if (auto workers = engine->find("workers"); workers != engine->end()) {
                if (!workers->is_number_integer() || workers->get<int>() <= 0) {
                    problems.push_back("`engine.workers` must be a positive integer");
                }
            }
            if (auto level = engine->find("log_level"); level != engine->end()) {
                const std::string text = level->is_string() ? level->get<std::string>() : "";
                static const std::vector<std::string> kLevels = {"trace", "debug", "info", "warn", "error", "off"};
                if (std::find(kLevels.begin(), kLevels.end(), text) == kLevels.end()) {
                    problems.push_back("`engine.log_level` must be trace|debug|info|warn|error|off");
                }
            }
        }
        if (auto sources = m_document->find("data_sources"); sources != m_document->end()) {
            if (!sources->is_array()) {
                problems.push_back("`data_sources` must be an array");
            } else {
                for (const auto &entry : *sources) {
                    if (!entry.is_object() || !entry.contains("name") || !entry.contains("provider")) {
                        problems.push_back("every `data_sources` entry needs `name` and `provider`");
                    }
                }
            }
        }
        return problems;
    }

    runtime::BindingProviderPtr ProviderConfigStore::bindingProvider() const {
        return std::make_shared<ConfigBindingProvider>(m_document, m_origin);
    }

} // namespace sapo::config
