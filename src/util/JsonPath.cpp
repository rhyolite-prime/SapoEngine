#include "util/JsonPath.hpp"

#include <cctype>

namespace sapo::util {

    std::string PathStep::toString() const {
        switch (kind) {
            case Kind::Field: return "." + field;
            case Kind::Index: return "[" + std::to_string(index) + "]";
            case Kind::Wildcard: return "[*]";
        }
        return {};
    }

    std::optional<std::vector<PathStep>> parsePath(std::string_view path) {
        std::vector<PathStep> steps;
        size_t i = 0;
        const size_t n = path.size();
        if (i < n && (path[i] == '$')) {
            ++i;
            if (i < n && path[i] == '.') ++i;
        }
        if (i == n) return steps; // "$" alone = the root document

        while (i < n) {
            if (path[i] == '[') {
                ++i;
                if (i < n && (path[i] == '"' || path[i] == '\'')) {
                    const char quote = path[i++];
                    std::string key;
                    while (i < n && path[i] != quote) key.push_back(path[i++]);
                    if (i >= n) return std::nullopt;
                    ++i; // closing quote
                    if (i >= n || path[i] != ']') return std::nullopt;
                    ++i;
                    steps.push_back(PathStep{PathStep::Kind::Field, key, 0});
                } else {
                    size_t start = i;
                    while (i < n && path[i] != ']') ++i;
                    if (i >= n) return std::nullopt;
                    const std::string inner = std::string(path.substr(start, i - start));
                    ++i; // closing bracket
                    if (inner == "*") {
                        steps.push_back(PathStep{PathStep::Kind::Wildcard, "*", 0});
                    } else {
                        bool is_number = !inner.empty();
                        for (size_t k = 0; k < inner.size(); ++k) {
                            if (!std::isdigit(static_cast<unsigned char>(inner[k])) &&
                                !(k == 0 && inner[k] == '-')) {
                                is_number = false;
                                break;
                            }
                        }
                        if (!is_number) return std::nullopt;
                        const int idx = std::atoi(inner.c_str());
                        steps.push_back(PathStep{PathStep::Kind::Index, inner, idx < 0 ? 0 : idx});
                    }
                }
                if (i < n && path[i] == '.') ++i;
                continue;
            }

            if (path[i] == '.') {
                ++i;
                continue;
            }

            size_t start = i;
            while (i < n && path[i] != '.' && path[i] != '[') ++i;
            if (start == i) return std::nullopt;
            const std::string field = std::string(path.substr(start, i - start));
            if (field == "*") {
                steps.push_back(PathStep{PathStep::Kind::Wildcard, field, 0});
            } else {
                steps.push_back(PathStep{PathStep::Kind::Field, field, 0});
            }
        }
        return steps;
    }

    const nlohmann::json *walk(const nlohmann::json &root, const std::vector<PathStep> &steps) {
        const nlohmann::json *cursor = &root;
        for (const auto &step : steps) {
            if (cursor == nullptr) return nullptr;
            switch (step.kind) {
                case PathStep::Kind::Field:
                    if (!cursor->is_object() || !cursor->contains(step.field)) return nullptr;
                    cursor = &cursor->at(step.field);
                    break;
                case PathStep::Kind::Index:
                    if (!cursor->is_array()) {
                        // A single-element projection is still addressable as [0].
                        if (step.index == 0 && !cursor->is_array()) break;
                        return nullptr;
                    }
                    if (step.index < 0 || static_cast<size_t>(step.index) >= cursor->size()) return nullptr;
                    cursor = &cursor->at(static_cast<size_t>(step.index));
                    break;
                case PathStep::Kind::Wildcard:
                    return nullptr; // wildcards are projections, not lookups
            }
        }
        return cursor;
    }

    std::optional<nlohmann::json> getPath(const nlohmann::json &root, std::string_view path) {
        const auto steps = parsePath(path);
        if (!steps.has_value()) return std::nullopt;
        const auto *found = walk(root, *steps);
        if (found == nullptr) return std::nullopt;
        return *found;
    }

    bool setPath(nlohmann::json &root, std::string_view raw_path, const nlohmann::json &value) {
        const auto steps = parsePath(raw_path);
        if (!steps.has_value() || steps->empty()) return false;
        nlohmann::json *cursor = &root;
        for (size_t i = 0; i < steps->size(); ++i) {
            const PathStep &step = (*steps)[i];
            const bool last = (i + 1 == steps->size());
            if (step.kind == PathStep::Kind::Index) {
                if (!cursor->is_array()) {
                    if (cursor->is_null()) {
                        *cursor = nlohmann::json::array();
                    } else {
                        return false;
                    }
                }
                while (cursor->size() <= static_cast<size_t>(step.index)) cursor->push_back(nullptr);
                cursor = &cursor->at(static_cast<size_t>(step.index));
            } else {
                if (!cursor->is_object()) {
                    if (cursor->is_null()) {
                        *cursor = nlohmann::json::object();
                    } else {
                        return false;
                    }
                }
                cursor = &(*cursor)[step.field];
            }
            if (last) *cursor = value;
        }
        return true;
    }

    std::string joinPath(std::string_view base, std::string_view suffix) {
        std::string out(base);
        if (out.empty()) return std::string(suffix);
        if (!suffix.empty() && suffix[0] == '[') {
            out.append(suffix);
            return out;
        }
        if (out.back() != '.') out.push_back('.');
        out.append(suffix);
        return out;
    }

} // namespace sapo::util
