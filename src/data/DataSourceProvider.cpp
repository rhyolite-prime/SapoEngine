//
//  Sapo Engine — data-source providers.
//
#include "data/DataSourceProvider.hpp"
#include "http/IHttpTransport.hpp"
#include "runtime/ExpressionEvaluator.hpp"
#include "runtime/SapoError.hpp"
#include "util/JsonPath.hpp"

#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>

using json = nlohmann::json;

namespace sapo::data {

    namespace {

        /// `true` / `"yes"` / non-empty → true (tolerant so generated blueprints
        /// never silently drop rows).
        bool truthy(const json &value) {
            if (value.is_boolean()) return value.get<bool>();
            if (value.is_null()) return false;
            if (value.is_number()) return value.get<double>() != 0.0;
            if (value.is_string()) {
                const std::string text = value.get<std::string>();
                return !(text.empty() || text == "false" || text == "0" || text == "null");
            }
            return true;
        }

        /// Equality tolerant of number/string and scalar/array comparisons.
        bool equals(const json &left, const json &right) {
            if (left.is_number() && right.is_number()) return left.get<double>() == right.get<double>();
            if (left.is_string() != right.is_string()) {
                if (left.is_number() && right.is_string()) return left.dump() == right.get<std::string>();
                if (left.is_string() && right.is_number()) return left.get<std::string>() == right.dump();
            }
            return left == right;
        }

        struct FieldValue {
            bool found{false};
            json value;
        };

        FieldValue fieldValue(const json &row, const std::string &path) {
            FieldValue out;
            if (path.empty() || !row.is_object()) return out;
            auto direct = row.find(path);
            if (direct != row.end()) {
                out.found = true;
                out.value = *direct;
                return out;
            }
            if (path.find('.') == std::string::npos && path.find('[') == std::string::npos) return out;
            const auto walked = util::getPath(row, path);
            if (walked.has_value()) {
                out.found = true;
                out.value = *walked;
            }
            return out;
        }

        /// Applies one operator object, e.g. `{"$gt": 5, "$in": [1,2]}`.
        bool applyOperators(const FieldValue &field, const json &operators) {
            const json *operand = nullptr;
            auto op = [&](const char *name) -> bool {
                auto it = operators.find(name);
                if (it == operators.end()) return false;
                operand = &(*it);
                return true;
            };

            if (!field.found) {
                // A missing field can only satisfy existence / negation operators.
                for (auto it = operators.begin(); it != operators.end(); ++it) {
                    const std::string key = it.key();
                    if (key == "$exists") return !truthy(it.value());
                    if (key == "$nin") continue;
                    if (key == "$ne") continue;
                    return false;
                }
                return true;
            }

            if (op("$exists") && field.found != truthy(*operand)) return false;
            if (op("$eq") && !equals(field.value, *operand)) return false;
            if (op("$ne") && equals(field.value, *operand)) return false;

            // Relational operators: numeric when both sides are numbers, otherwise
            // lexicographic on the textual form (so ISO dates compare correctly).
            auto relational = [&](const char *name, bool (*test)(const std::string &, const std::string &),
                                  bool (*test_num)(double, double)) -> bool {
                if (!op(name)) return true;
                if (field.value.is_number() && operand->is_number()) {
                    return test_num(field.value.get<double>(), operand->get<double>());
                }
                const std::string left = field.value.is_string() ? field.value.get<std::string>() : field.value.dump();
                const std::string right = operand->is_string() ? operand->get<std::string>() : operand->dump();
                return test(left, right);
            };
            if (!relational("$lt", [](const std::string &a, const std::string &b) { return a < b; },
                            [](double a, double b) { return a < b; })) return false;
            if (!relational("$lte", [](const std::string &a, const std::string &b) { return a <= b; },
                             [](double a, double b) { return a <= b; })) return false;
            if (!relational("$gt", [](const std::string &a, const std::string &b) { return a > b; },
                            [](double a, double b) { return a > b; })) return false;
            if (!relational("$gte", [](const std::string &a, const std::string &b) { return a >= b; },
                             [](double a, double b) { return a >= b; })) return false;

            if (op("$in")) {
                if (!operand->is_array()) return false;
                bool found = false;
                for (const auto &candidate : *operand) {
                    if (equals(field.value, candidate)) {
                        found = true;
                        break;
                    }
                }
                if (!found) return false;
            }
            if (op("$nin")) {
                if (!operand->is_array()) return false;
                for (const auto &candidate : *operand) {
                    if (equals(field.value, candidate)) return false;
                }
            }
            if (op("$regex")) {
                if (!operand->is_string() || !field.value.is_string()) return false;
                try {
                    const std::regex pattern(operand->get<std::string>(), std::regex::ECMAScript);
                    if (!std::regex_search(field.value.get<std::string>(), pattern)) return false;
                } catch (const std::regex_error &) {
                    throw runtime::SapoError(runtime::ErrorCode::Validation,
                                             "invalid $regex pattern '" + operand->get<std::string>() + "'");
                }
            }
            for (auto it = operators.begin(); it != operators.end(); ++it) {
                const std::string key = it.key();
                if (!key.empty() && key[0] == '$') continue;
                // A plain key here means the author nested matchers wrongly; rejecting
                // is better than quietly matching nothing.
                throw runtime::SapoError(runtime::ErrorCode::Validation,
                                         "unsupported filter operator '" + key + "' (expected $eq/$ne/$gt/$gte/$lt/"
                                         "$lte/$in/$nin/$regex/$exists)");
            }
            return true;
        }

    } // namespace

    bool matchesFilter(const json &row, const json &filter) {
        if (filter.is_null()) return true;
        if (filter.is_boolean()) return truthy(filter);
        if (filter.is_array()) {
            for (const auto &entry : filter) {
                if (!matchesFilter(row, entry)) return false;
            }
            return true;
        }
        if (!filter.is_object()) {
            throw runtime::SapoError(runtime::ErrorCode::Validation,
                                     "a query 'filter' must be an object, an array of objects or null (got " +
                                         std::string(filter.type_name()) + ")");
        }
        for (auto it = filter.begin(); it != filter.end(); ++it) {
            const FieldValue field = fieldValue(row, it.key());
            if (it.value().is_object() && !it.value().empty()) {
                bool operator_object = false;
                for (auto inner = it.value().begin(); inner != it.value().end(); ++inner) {
                    if (!inner.key().empty() && inner.key()[0] == '$') {
                        operator_object = true;
                        break;
                    }
                }
                if (operator_object) {
                    if (!applyOperators(field, it.value())) return false;
                    continue;
                }
            }
            if (!field.found) return false;
            if (!equals(field.value, it.value())) return false;
        }
        return true;
    }

    RowMatcher makeRowMatcher(json filter) {
        return [filter = std::move(filter)](const json &row) { return matchesFilter(row, filter); };
    }

    std::vector<std::string> IDataSourceProvider::validate(const DataSourceConfig &config) const {
        std::vector<std::string> problems;
        if (config.name.empty()) problems.push_back("data source '" + config.name + "' has no name");
        if (config.scope != "internal" && config.scope != "external") {
            problems.push_back("data source '" + config.name + "' has invalid scope '" + config.scope + "'");
        }
        if (!config.config.is_object()) {
            problems.push_back("data source '" + config.name + "' config must be an object");
        }
        return problems;
    }

    DataPage IDataSourceProvider::query(const DataSourceConfig &config, const DataQuery &,
                                        sapo::runtime::RuntimeContext &, const RowMatcher &) const {
        throw runtime::SapoError(runtime::ErrorCode::NotImplemented,
                                 "data source provider '" + providerId() + "' does not implement query() for '" +
                                     config.name + "'");
    }

    // ---------------------------------------------------------------------
    // Registry
    // ---------------------------------------------------------------------
    void DataSourceRegistry::add(ProviderPtr provider) {
        if (provider == nullptr) return;
        for (auto it = m_providers.begin(); it != m_providers.end(); ++it) {
            if ((*it)->providerId() == provider->providerId()) {
                m_providers.erase(it);
                break;
            }
        }
        m_providers.push_back(std::move(provider));
    }

    void DataSourceRegistry::clear() { m_providers.clear(); }

    bool DataSourceRegistry::has(const std::string &provider_id) const { return find(provider_id) != nullptr; }

    const IDataSourceProvider *DataSourceRegistry::find(const std::string &provider_id) const {
        for (const auto &provider : m_providers) {
            if (provider->providerId() == provider_id) return provider.get();
        }
        return nullptr;
    }

    std::vector<std::string> DataSourceRegistry::providerIds() const {
        std::vector<std::string> out;
        for (const auto &provider : m_providers) out.push_back(provider->providerId());
        std::sort(out.begin(), out.end());
        return out;
    }

    std::vector<std::string> DataSourceRegistry::deferredProviders() const {
        std::vector<std::string> out;
        for (const auto &provider : m_providers) {
            if (provider->deferred()) out.push_back(provider->providerId());
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    DataPage DataSourceRegistry::run(const DataSourceConfig &config, const DataQuery &query,
                                    sapo::runtime::RuntimeContext &context, const RowMatcher &matcher) const {
        const IDataSourceProvider *provider = find(config.provider);
        if (provider == nullptr) {
            throw runtime::SapoError(
                runtime::ErrorCode::NotFound,
                "no data source provider is registered for '" + config.provider + "' (data source '" + config.name +
                    "'). Available: " + [&] {
                        std::string list;
                        for (const auto &id : providerIds()) {
                            if (!list.empty()) list += ", ";
                            list += id;
                        }
                        return list.empty() ? std::string("none") : list;
                    }() +
                    (config.origin.empty() ? "" : " [declared in " + config.origin + "]"),
                json{{"data_source", config.name}, {"provider", config.provider}});
        }
        if (provider->deferred()) {
            throw runtime::SapoError(runtime::ErrorCode::NotImplemented,
                                     "data source provider '" + config.provider + "' is registered but not implemented "
                                     "in this build (deferred; see docs/LIMITATIONS.md)",
                                     json{{"data_source", config.name}, {"provider", config.provider}});
        }

        DataPage page = provider->query(config, query, context, matcher);
        if (!page.rows.is_array()) {
            throw runtime::SapoError(runtime::ErrorCode::DataSource,
                                     "data source provider '" + config.provider + "' returned non-array rows for '" +
                                         config.name + "'");
        }
        json rows = page.rows;
        if (!page.pre_filtered && matcher) {
            json filtered = json::array();
            for (const auto &row : rows) {
                if (matcher(row)) filtered.push_back(row);
            }
            rows = std::move(filtered);
        }
        const size_t matched = rows.size();
        size_t begin = 0;
        if (query.offset.has_value() && *query.offset > 0) begin = static_cast<size_t>(*query.offset);
        if (begin > rows.size()) begin = rows.size();
        if (begin > 0) rows.erase(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(begin));

        size_t limit = rows.size();
        bool truncated = begin > 0;
        if (query.limit.has_value() && *query.limit >= 0 && static_cast<size_t>(*query.limit) < rows.size()) {
            limit = static_cast<size_t>(*query.limit);
            rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(limit), rows.end());
            truncated = true;
        }
        DataPage out;
        out.rows = std::move(rows);
        out.pre_filtered = true;
        out.pre_paged = true;
        out.total_matches = page.total_matches != 0 ? page.total_matches : matched;
        out.meta = page.meta;
        out.truncated = truncated;
        return out;
    }

    std::vector<std::string> DataSourceRegistry::validate(const std::vector<DataSourceConfig> &sources) const {
        std::vector<std::string> problems;
        for (const auto &source : sources) {
            if (source.provider.empty()) {
                problems.push_back("data source '" + source.name + "' declares no provider");
                continue;
            }
            const IDataSourceProvider *provider = find(source.provider);
            if (provider == nullptr) {
                problems.push_back("data source '" + source.name + "' uses unknown provider '" + source.provider + "'");
                continue;
            }
            if (provider->deferred()) {
                problems.push_back("data source '" + source.name + "' uses provider '" + source.provider +
                                   "' which is deferred in this build");
            }
            for (auto &problem : provider->validate(source)) problems.push_back(std::move(problem));
        }
        return problems;
    }

    DataSourceRegistry &DataSourceRegistry::global() {
        static DataSourceRegistry registry;
        return registry;
    }

    // ---------------------------------------------------------------------
    // Built-in providers
    // ---------------------------------------------------------------------
    namespace {

        class ContextDataSourceProvider final : public IDataSourceProvider {
        public:
            [[nodiscard]] std::string providerId() const override { return "context"; }
            [[nodiscard]] std::string description() const override {
                return "Reads a collection already present in the session context.";
            }

            [[nodiscard]] std::vector<std::string> validate(const DataSourceConfig &config) const override {
                auto problems = IDataSourceProvider::validate(config);
                if (!config.config.is_object() || !config.config.contains("source")) {
                    problems.push_back("data source '" + config.name +
                                       "' (provider=context) needs config.source, e.g. \"$rows\"");
                }
                return problems;
            }

            [[nodiscard]] DataPage query(const DataSourceConfig &config, const DataQuery &query,
                                         sapo::runtime::RuntimeContext &context, const RowMatcher &matcher) const override {
                const std::string expression = config.config.value("source", "");
                sapo::runtime::EvaluationScope scope(context);
                scope.options.strict = false;
                const json value = sapo::runtime::ExpressionEvaluator::resolveValue(
                    expression.empty() ? json(config.config.dump()).get<std::string>() : expression, scope);
                json rows = json::array();
                if (value.is_array()) rows = value;
                else if (value.is_object()) {
                    // `{items: [...]}` / `{rows: [...]}` / `{data: [...]}` unwrapping.
                    for (const char *key : {"items", "rows", "data", "results"}) {
                        if (value.contains(key) && value[key].is_array()) {
                            rows = value[key];
                            break;
                        }
                    }
                    if (rows.empty()) rows.push_back(value);
                } else if (!value.is_null()) {
                    throw runtime::SapoError(runtime::ErrorCode::DataSource,
                                             "data source '" + config.name + "' resolved to a non-collection value");
                }
                DataPage page;
                if (matcher) {
                    json filtered = json::array();
                    for (const auto &row : rows) {
                        if (matcher(row)) filtered.push_back(row);
                    }
                    rows = std::move(filtered);
                }
                page.rows = std::move(rows);
                page.pre_filtered = matcher != nullptr;
                (void) query;
                return page;
            }
        };

        class MockDataSourceProvider final : public IDataSourceProvider {
        public:
            [[nodiscard]] std::string providerId() const override { return "mock"; }
            [[nodiscard]] std::string description() const override {
                return "Static rows supplied in config.rows (tests, examples, demos).";
            }

            [[nodiscard]] std::vector<std::string> validate(const DataSourceConfig &config) const override {
                auto problems = IDataSourceProvider::validate(config);
                const bool has_rows = config.config.is_object() && config.config.contains("rows");
                const bool has_file = config.config.is_object() && config.config.contains("file");
                if (!has_rows && !has_file) {
                    problems.push_back("data source '" + config.name +
                                       "' (provider=mock) needs config.rows or config.file");
                }
                if (has_rows && !config.config["rows"].is_array()) {
                    problems.push_back("data source '" + config.name + "' config.rows must be an array");
                }
                return problems;
            }

            [[nodiscard]] DataPage query(const DataSourceConfig &config, const DataQuery &query,
                                         sapo::runtime::RuntimeContext &context, const RowMatcher &matcher) const override {
                json rows = config.config.value("rows", json::array());
                if (rows.is_object() && rows.contains("rows")) rows = rows["rows"];
                if (rows.is_null() && config.config.contains("file")) {
                    std::ifstream in(config.config["file"].get<std::string>(), std::ios::binary);
                    std::stringstream buffer;
                    buffer << in.rdbuf();
                    try {
                        rows = json::parse(buffer.str());
                    } catch (const std::exception &error) {
                        throw runtime::SapoError(runtime::ErrorCode::DataSource,
                                                 "mock data source file could not be read: " + std::string(error.what()));
                    }
                    if (rows.is_object() && rows.contains("rows")) rows = rows["rows"];
                }
                if (!rows.is_array()) {
                    throw runtime::SapoError(runtime::ErrorCode::DataSource,
                                             "mock data source '" + config.name + "' has no row array");
                }
                DataPage page;
                if (matcher) {
                    json filtered = json::array();
                    for (const auto &row : rows) {
                        if (matcher(row)) filtered.push_back(row);
                    }
                    rows = std::move(filtered);
                    page.pre_filtered = true;
                }
                page.rows = std::move(rows);
                (void) query;
                (void) context;
                return page;
            }
        };

        class HttpDataSourceProvider final : public IDataSourceProvider {
        public:
            explicit HttpDataSourceProvider(sapo::http::TransportPtr transport) : m_transport(std::move(transport)) {}

            [[nodiscard]] std::string providerId() const override { return "http"; }
            [[nodiscard]] std::string description() const override {
                return "Fetches a JSON array over HTTP (transport injected; mock/replay in tests).";
            }

            [[nodiscard]] std::vector<std::string> validate(const DataSourceConfig &config) const override {
                auto problems = IDataSourceProvider::validate(config);
                if (!config.config.is_object() || !config.config.contains("url")) {
                    problems.push_back("data source '" + config.name + "' (provider=http) needs config.url");
                }
                if (m_transport == nullptr) {
                    problems.push_back("http data source provider has no transport installed");
                }
                return problems;
            }

            [[nodiscard]] DataPage query(const DataSourceConfig &config, const DataQuery &query,
                                        sapo::runtime::RuntimeContext &context, const RowMatcher &matcher) const override {
                sapo::runtime::EvaluationScope scope(context);
                sapo::http::Request request;
                request.method = config.config.value("method", "GET");
                request.url = sapo::runtime::ExpressionEvaluator::resolveValue(config.config.value("url", ""), scope)
                                  .dump();
                if (!request.url.empty() && request.url.front() == '"' && request.url.back() == '"') {
                    request.url = request.url.substr(1, request.url.size() - 2);
                }
                if (config.config.contains("headers")) {
                    request.headers = sapo::runtime::ExpressionEvaluator::resolveMap(config.config["headers"], scope);
                }
                if (query.limit.has_value()) request.query["limit"] = std::to_string(*query.limit);
                if (query.offset.has_value()) request.query["offset"] = std::to_string(*query.offset);
                for (auto it = request.headers.begin(); it != request.headers.end(); ++it) {
                    if (!it.value().is_string()) it.value() = it.value().dump();
                }
                if (config.config.contains("query")) {
                    request.query = sapo::runtime::ExpressionEvaluator::resolveMap(config.config["query"], scope);
                }
                for (auto it = request.query.begin(); it != request.query.end(); ++it) {
                    if (!it.value().is_string()) it.value() = it.value().dump();
                }
                if (config.config.contains("body")) {
                    request.body = sapo::runtime::ExpressionEvaluator::resolveValue(config.config["body"], scope);
                }
                request.timeout_ms = config.config.value("timeout_ms", 10000);

                const sapo::http::Response response = m_transport->send(request);
                if (!response.transport_error.empty()) {
                    throw runtime::SapoError(runtime::ErrorCode::Http,
                                             "data source '" + config.name + "' transport failure: " +
                                                 response.transport_error,
                                             json{{"url", request.url}});
                }
                if (!response.ok()) {
                    throw runtime::SapoError(
                        runtime::ErrorCode::HttpStatus,
                        "data source '" + config.name + "' returned HTTP " + std::to_string(response.status_code),
                        json{{"status", response.status_code}, {"url", request.url}});
                }
                json document = response.jsonBody();
                if (document.is_null()) {
                    throw runtime::SapoError(runtime::ErrorCode::DataSource,
                                             "data source '" + config.name + "' returned a body that is not JSON");
                }
                const std::string rows_path = config.config.value("rows_path", "");
                if (!rows_path.empty()) {
                    if (const auto walked = util::getPath(document, rows_path); walked.has_value()) document = *walked;
                }
                if (document.is_object()) {
                    for (const char *key : {"items", "rows", "data", "results"}) {
                        if (document.contains(key) && document[key].is_array()) {
                            document = document[key];
                            break;
                        }
                    }
                }
                if (!document.is_array()) document = json::array({document});

                DataPage page;
                json rows = std::move(document);
                if (matcher) {
                    json filtered = json::array();
                    for (const auto &row : rows) {
                        if (matcher(row)) filtered.push_back(row);
                    }
                    rows = std::move(filtered);
                    page.pre_filtered = true;
                }
                page.rows = std::move(rows);
                page.meta = json{{"status", response.status_code}, {"url", request.url}};
                // The provider has already been paged via query params when the
                // server honoured them; keep registry paging as the safe default.
                page.pre_paged = false;
                return page;
            }

        private:
            sapo::http::TransportPtr m_transport;
        };

        /// Registered-but-unavailable providers: the *name* resolves (so blueprints
        /// validate) while any call fails with NOT_IMPLEMENTED, never with empty
        /// results (plan T2.8 / T3.3).
        class DeferredDataSourceProvider final : public IDataSourceProvider {
        public:
            DeferredDataSourceProvider(std::string id, std::string reason)
                : m_id(std::move(id)), m_reason(std::move(reason)) {}

            [[nodiscard]] std::string providerId() const override { return m_id; }
            [[nodiscard]] std::string description() const override { return m_reason; }
            [[nodiscard]] bool deferred() const override { return true; }

            [[nodiscard]] DataPage query(const DataSourceConfig &config, const DataQuery &,
                                         sapo::runtime::RuntimeContext &, const RowMatcher &) const override {
                throw runtime::SapoError(runtime::ErrorCode::NotImplemented,
                                         "data source provider '" + m_id + "' is not available: " + m_reason,
                                         json{{"data_source", config.name}, {"provider", m_id}});
            }

        private:
            std::string m_id;
            std::string m_reason;
        };

    } // namespace

    std::shared_ptr<DataSourceRegistry> DataSourceRegistry::withBuiltIns(
        const std::shared_ptr<sapo::http::IHttpTransport> &transport) {
        auto registry = std::make_shared<DataSourceRegistry>();
        registry->add(std::make_shared<ContextDataSourceProvider>());
        registry->add(std::make_shared<MockDataSourceProvider>());
        registry->add(std::make_shared<HttpDataSourceProvider>(transport));
        registry->add(std::make_shared<DeferredDataSourceProvider>(
            "redis", "no Redis client is linked in this build; implement IDataSourceProvider to enable it"));
        registry->add(std::make_shared<DeferredDataSourceProvider>(
            "postgresql", "no PostgreSQL client is linked in this build; implement IDataSourceProvider to enable it"));
        return registry;
    }

} // namespace sapo::data
