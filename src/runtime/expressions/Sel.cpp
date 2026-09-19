//
//  Sapo Expression Language (SEL): lexer → parser → evaluator → stdlib.
//
//  One translation unit by design: the AST, the chain resolver and the
//  function table are private to the language, not to individual tasks.
//
#include "runtime/expressions/Sel.hpp"
#include "runtime/expressions/SelNodes.hpp"
#include "runtime/expressions/Value.hpp"
#include "util/Crypto.hpp"
#include "util/Encoding.hpp"
#include "util/JsonPath.hpp"
#include "util/TimeUtils.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <limits>
#include <random>
#include <regex>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace sapo::expr {

    namespace {

        [[noreturn]] void fail(const std::string &message, size_t position, const std::string &source) {
            json data = {{"expression", source}};
            if (position != std::string_view::npos) data["offset"] = position;
            throw runtime::SapoError(runtime::ErrorCode::Expression, message, std::move(data));
        }

        bool isIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
        bool isIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

        // =====================================================================
        // Lexer
        // =====================================================================
        enum class TokKind {
            End, Number, String, Ident, Dollar, Dot, OptDot, LParen, RParen, LBracket, RBracket, LBrace, RBrace,
            Comma, Colon, Question, Coalesce, Plus, Minus, Star, Slash, Percent, Eq, Ne, Lt, Le, Gt, Ge, And, Or, Not,
            Keyword
        };

        struct Token {
            TokKind kind{TokKind::End};
            std::string text;
            json literal;
            size_t pos{0};
        };

        class Lexer {
        public:
            Lexer(std::string_view source, std::string owner) : m_src(source), m_owner(std::move(owner)) {}

            std::vector<Token> tokenize() {
                std::vector<Token> out;
                while (true) {
                    Token tok = next();
                    const bool is_end = tok.kind == TokKind::End;
                    out.push_back(std::move(tok));
                    if (is_end) break;
                }
                return out;
            }

        private:
            std::string_view m_src;
            std::string m_owner;
            size_t m_i{0};

            [[noreturn]] void bad(const std::string &message, size_t pos) { fail(message, pos, m_owner); }

            void skipSpace() {
                while (m_i < m_src.size() && std::isspace(static_cast<unsigned char>(m_src[m_i]))) ++m_i;
            }

            bool peekAt(size_t offset, char expected) const {
                return m_i + offset < m_src.size() && m_src[m_i + offset] == expected;
            }

            Token next() {
                skipSpace();
                if (m_i >= m_src.size()) return Token{TokKind::End, "", json(), m_i};
                const size_t start = m_i;
                const char c = m_src[m_i];

                if (std::isdigit(static_cast<unsigned char>(c))) {
                    std::string digits;
                    while (m_i < m_src.size() &&
                           (std::isdigit(static_cast<unsigned char>(m_src[m_i])) || m_src[m_i] == '.')) {
                        digits.push_back(m_src[m_i++]);
                    }
                    if (m_i < m_src.size() && (m_src[m_i] == 'e' || m_src[m_i] == 'E')) {
                        digits.push_back(m_src[m_i++]);
                        if (m_i < m_src.size() && (m_src[m_i] == '+' || m_src[m_i] == '-')) digits.push_back(m_src[m_i++]);
                        while (m_i < m_src.size() && std::isdigit(static_cast<unsigned char>(m_src[m_i]))) {
                            digits.push_back(m_src[m_i++]);
                        }
                    }
                    Token tok{TokKind::Number, digits, json(), start};
                    try {
                        if (digits.find_first_of(".eE") == std::string::npos) tok.literal = json::parse(digits);
                        else tok.literal = std::stod(digits);
                    } catch (...) {
                        bad("malformed numeric literal '" + digits + "'", start);
                    }
                    return tok;
                }

                if (c == '"' || c == '\'') {
                    const char quote = c;
                    ++m_i;
                    std::string value;
                    while (m_i < m_src.size() && m_src[m_i] != quote) {
                        if (m_src[m_i] == '\\' && m_i + 1 < m_src.size()) {
                            const char esc = m_src[++m_i];
                            switch (esc) {
                                case 'n': value.push_back('\n'); break;
                                case 't': value.push_back('\t'); break;
                                case 'r': value.push_back('\r'); break;
                                case '0': value.push_back('\0'); break;
                                case '\\': value.push_back('\\'); break;
                                case '\'': value.push_back('\''); break;
                                case '"': value.push_back('"'); break;
                                default: value.push_back(esc); break;
                            }
                            ++m_i;
                            continue;
                        }
                        value.push_back(m_src[m_i++]);
                    }
                    if (m_i >= m_src.size()) bad("unterminated string literal", start);
                    ++m_i;
                    return Token{TokKind::String, value, json(value), start};
                }

                if (isIdentStart(c)) {
                    std::string ident;
                    while (m_i < m_src.size() && isIdentChar(m_src[m_i])) ident.push_back(m_src[m_i++]);
                    static const std::vector<std::string> kKeywords = {
                        "and", "or", "not", "in", "contains", "true", "false", "null", "startsWith", "endsWith"};
                    if (std::find(kKeywords.begin(), kKeywords.end(), ident) != kKeywords.end() &&
                        !peekAt(0, '(')) {
                        // Keyword operators only when *not* used as a function call.
                        if (ident == "startsWith" || ident == "endsWith") {
                            // These are plain functions; nothing special here.
                            return Token{TokKind::Ident, ident, json(), start};
                        }
                        return Token{TokKind::Keyword, ident, json(), start};
                    }
                    return Token{TokKind::Ident, ident, json(), start};
                }

                auto take = [&](size_t n, TokKind kind) {
                    std::string text(m_src.substr(m_i, n));
                    m_i += n;
                    return Token{kind, text, json(), start};
                };

                if (c == '?' && peekAt(1, '?')) return take(2, TokKind::Coalesce);
                if (c == '?' && peekAt(1, '.')) return take(2, TokKind::OptDot);

                switch (c) {
                    case '$': return take(1, TokKind::Dollar);
                    case '.': return take(1, TokKind::Dot);
                    case '(': return take(1, TokKind::LParen);
                    case ')': return take(1, TokKind::RParen);
                    case '[': return take(1, TokKind::LBracket);
                    case ']': return take(1, TokKind::RBracket);
                    case '{': return take(1, TokKind::LBrace);
                    case '}': return take(1, TokKind::RBrace);
                    case ',': return take(1, TokKind::Comma);
                    case ':': return take(1, TokKind::Colon);
                    case '?': return take(1, TokKind::Question);
                    case '+': return take(1, TokKind::Plus);
                    case '-': return take(1, TokKind::Minus);
                    case '*': return take(1, TokKind::Star);
                    case '/': return take(1, TokKind::Slash);
                    case '%': return take(1, TokKind::Percent);
                    case '=':
                        if (peekAt(1, '=')) return take(2, TokKind::Eq);
                        if (peekAt(1, '~')) return take(2, TokKind::Keyword); // regex match `=~`
                        bad("'=' is not a valid operator; use '==' for comparison", start);
                    case '!':
                        if (peekAt(1, '=')) return take(2, TokKind::Ne);
                        return take(1, TokKind::Not);
                    case '<':
                        if (peekAt(1, '=')) return take(2, TokKind::Le);
                        return take(1, TokKind::Lt);
                    case '>':
                        if (peekAt(1, '=')) return take(2, TokKind::Ge);
                        return take(1, TokKind::Gt);
                    case '&':
                        if (peekAt(1, '&')) return take(2, TokKind::And);
                        bad("unexpected character '&'; use '&&'", start);
                    case '|':
                        if (peekAt(1, '|')) return take(2, TokKind::Or);
                        bad("unexpected character '|'; use '||'", start);
                    default:
                        break;
                }
                bad(std::string("unexpected character '") + c + "'", start);
            }
        };

        // =====================================================================
        // Parser (precedence climbing)
        // =====================================================================
        class Parser {
        public:
            explicit Parser(std::string source) : m_source(std::move(source)) {
                m_tokens = Lexer(m_source, m_source).tokenize();
            }

            NodePtr parseProgram() {
                if (m_tokens.size() == 1 && m_tokens.front().kind == TokKind::End) {
                    fail("empty expression", 0, m_source);
                }
                NodePtr node = parseTernary();
                if (peek().kind != TokKind::End) fail("unexpected trailing token '" + peek().text + "'", peek().pos, m_source);
                return node;
            }

        private:
            std::string m_source;
            std::vector<Token> m_tokens;
            size_t m_i{0};

            const Token &peek(size_t offset = 0) const {
                const size_t idx = m_i + offset;
                return idx < m_tokens.size() ? m_tokens[idx] : m_tokens.back();
            }
            const Token &advance() {
                const Token &tok = peek();
                if (m_i + 1 < m_tokens.size()) ++m_i;
                return tok;
            }
            bool accept(TokKind kind) {
                if (peek().kind == kind) { advance(); return true; }
                return false;
            }
            [[nodiscard]] bool isKeyword(std::string_view keyword) const {
                return peek().kind == TokKind::Keyword && peek().text == keyword;
            }
            bool acceptKeyword(std::string_view keyword) {
                if (isKeyword(keyword)) { advance(); return true; }
                return false;
            }
            [[noreturn]] void bad(const std::string &message) { fail(message, peek().pos, m_source); }
            void expect(TokKind kind, std::string_view what) {
                if (!accept(kind)) bad("expected " + std::string(what) + " but found '" +
                                       (peek().text.empty() ? "<end>" : peek().text) + "'");
            }

            /// Mutable builder handle; converted to the immutable NodePtr on return.
            static std::shared_ptr<Node> node(NodeKind kind) {
                auto created = std::make_shared<Node>();
                created->kind = kind;
                return created;
            }

            NodePtr makeBinary(std::string op, NodePtr left, NodePtr right) {
                auto created = std::make_shared<Node>();
                created->kind = NodeKind::Binary;
                created->name = std::move(op);
                created->left = std::move(left);
                created->right = std::move(right);
                return created;
            }

            NodePtr parseTernary() {
                NodePtr condition = parseCoalesce();
                if (accept(TokKind::Question)) {
                    NodePtr then_branch = parseTernary();
                    expect(TokKind::Colon, "':' in ternary");
                    NodePtr else_branch = parseTernary();
                    auto created = node(NodeKind::Ternary);
                    created->left = condition;
                    created->right = then_branch;
                    created->extra = else_branch;
                    return created;
                }
                return condition;
            }

            NodePtr parseCoalesce() {
                NodePtr left = parseLogicalOr();
                if (accept(TokKind::Coalesce)) {
                    NodePtr right = parseCoalesce();
                    auto created = node(NodeKind::Coalesce);
                    created->left = left;
                    created->right = right;
                    return created;
                }
                return left;
            }

            NodePtr parseLogicalOr() {
                NodePtr left = parseLogicalAnd();
                while (accept(TokKind::Or) || acceptKeyword("or")) left = makeBinary("||", left, parseLogicalAnd());
                return left;
            }

            NodePtr parseLogicalAnd() {
                NodePtr left = parseEquality();
                while (accept(TokKind::And) || acceptKeyword("and")) left = makeBinary("&&", left, parseEquality());
                return left;
            }

            NodePtr parseEquality() {
                NodePtr left = parseComparison();
                while (peek().kind == TokKind::Eq || peek().kind == TokKind::Ne) {
                    const std::string op = advance().text;
                    left = makeBinary(op, left, parseComparison());
                }
                return left;
            }

            NodePtr parseComparison() {
                NodePtr left = parseAdditive();
                while (peek().kind == TokKind::Lt || peek().kind == TokKind::Le || peek().kind == TokKind::Gt ||
                       peek().kind == TokKind::Ge ||
                       (peek().kind == TokKind::Keyword &&
                        (peek().text == "in" || peek().text == "contains" || peek().text == "=~"))) {
                    const std::string op = advance().text;
                    left = makeBinary(op, left, parseAdditive());
                }
                return left;
            }

            NodePtr parseAdditive() {
                NodePtr left = parseMultiplicative();
                while (peek().kind == TokKind::Plus || peek().kind == TokKind::Minus) {
                    const std::string op = advance().text;
                    left = makeBinary(op, left, parseMultiplicative());
                }
                return left;
            }

            NodePtr parseMultiplicative() {
                NodePtr left = parseUnary();
                while (peek().kind == TokKind::Star || peek().kind == TokKind::Slash || peek().kind == TokKind::Percent) {
                    const std::string op = advance().text;
                    left = makeBinary(op, left, parseUnary());
                }
                return left;
            }

            NodePtr parseUnary() {
                if (peek().kind == TokKind::Not || isKeyword("not") || peek().kind == TokKind::Minus ||
                    peek().kind == TokKind::Plus) {
                    std::string op = advance().text;
                    if (op == "not") op = "!";
                    auto created = node(NodeKind::Unary);
                    created->name = std::move(op);
                    created->left = parseUnary();
                    return created;
                }
                return parsePostfix();
            }

            NodePtr parsePostfix() {
                NodePtr base = parsePrimary();
                return parseChain(std::move(base));
            }

            NodePtr parseChain(NodePtr base) {
                while (true) {
                    if (accept(TokKind::Dot) || peek().kind == TokKind::OptDot) {
                        const bool optional = accept(TokKind::OptDot);
                        if (peek().kind != TokKind::Ident && peek().kind != TokKind::Number) {
                            bad("expected a field name after '.'");
                        }
                        const std::string field = advance().text;
                        if (accept(TokKind::LParen)) {
                            auto created = node(NodeKind::MethodCall);
                            created->name = field;
                            created->left = base;
                            created->args = parseArguments();
                            created->optional = optional;
                            base = created;
                        } else {
                            auto created = node(NodeKind::Member);
                            created->name = field;
                            created->left = base;
                            created->optional = optional;
                            base = created;
                        }
                        continue;
                    }
                    if (accept(TokKind::LBracket)) {
                        NodePtr index = parseTernary();
                        expect(TokKind::RBracket, "']'");
                        auto created = node(NodeKind::Index);
                        created->left = base;
                        if (index->kind == NodeKind::Literal && index->literal.is_number_integer()) {
                            created->index = index->literal.get<int>();
                            created->has_static_index = true;
                        } else {
                            created->right = index;
                        }
                        base = created;
                        continue;
                    }
                    break;
                }
                return base;
            }

            std::vector<NodePtr> parseArguments() {
                std::vector<NodePtr> args;
                if (accept(TokKind::RParen)) return args;
                while (true) {
                    args.push_back(parseTernary());
                    if (accept(TokKind::Comma)) continue;
                    expect(TokKind::RParen, "')'");
                    break;
                }
                return args;
            }

            NodePtr parsePrimary() {
                const Token &tok = peek();
                switch (tok.kind) {
                    case TokKind::Number:
                    case TokKind::String: {
                        advance();
                        auto created = node(NodeKind::Literal);
                        created->literal = tok.literal;
                        return created;
                    }
                    case TokKind::LParen: {
                        advance();
                        NodePtr inner = parseTernary();
                        expect(TokKind::RParen, "')'");
                        return inner;
                    }
                    case TokKind::LBracket: {
                        advance();
                        std::vector<NodePtr> items;
                        if (!isClosingBracket()) {
                            while (true) {
                                items.push_back(parseTernary());
                                if (accept(TokKind::Comma)) continue;
                                break;
                            }
                        }
                        expect(TokKind::RBracket, "']'");
                        auto created = node(NodeKind::ArrayLiteral);
                        created->args = std::move(items);
                        return created;
                    }
                    case TokKind::LBrace: {
                        advance();
                        std::vector<std::string> keys;
                        std::vector<NodePtr> values;
                        if (peek().kind != TokKind::RBrace) {
                            while (true) {
                                std::string key;
                                if (peek().kind == TokKind::String || peek().kind == TokKind::Ident ||
                                    peek().kind == TokKind::Number) {
                                    key = advance().text;
                                } else {
                                    bad("expected an object key");
                                }
                                expect(TokKind::Colon, "':' in object literal");
                                keys.push_back(std::move(key));
                                values.push_back(parseTernary());
                                if (accept(TokKind::Comma)) continue;
                                break;
                            }
                        }
                        expect(TokKind::RBrace, "'}'");
                        auto created = node(NodeKind::ObjectLiteral);
                        created->keys = std::move(keys);
                        created->args = std::move(values);
                        return created;
                    }
                    case TokKind::Dollar: {
                        advance();
                        if (peek().kind != TokKind::Ident) bad("expected a variable name after '$'");
                        const std::string name = advance().text;
                        if (accept(TokKind::LParen)) {
                            // `$fn(...)` is tolerated: `$` is decorative before a call.
                            auto created = node(NodeKind::Call);
                            created->name = name;
                            created->args = parseArguments();
                            return created;
                        }
                        auto created = node(NodeKind::Variable);
                        created->name = name;
                        created->explicit_dollar = true;
                        return created;
                    }
                    case TokKind::Keyword: {
                        advance();
                        if (tok.text == "true" || tok.text == "false") {
                            auto created = node(NodeKind::Literal);
                            created->literal = (tok.text == "true");
                            return created;
                        }
                        if (tok.text == "null") {
                            auto created = node(NodeKind::Literal);
                            created->literal = json(nullptr);
                            return created;
                        }
                        bad("unexpected keyword '" + tok.text + "'");
                    }
                    case TokKind::Ident: {
                        const std::string name = advance().text;
                        if (accept(TokKind::LParen)) {
                            auto created = node(NodeKind::Call);
                            created->name = name;
                            created->args = parseArguments();
                            return created;
                        }
                        auto created = node(NodeKind::Variable);
                        created->name = name;
                        return created;
                    }
                    default:
                        break;
                }
                bad("unexpected token '" + (tok.text.empty() ? std::string("<end>") : tok.text) + "'");
            }

            [[nodiscard]] bool isClosingBracket() const { return peek().kind == TokKind::RBracket; }
        };

        // =====================================================================
        // Evaluator
        // =====================================================================
        json callFunction(const std::string &name, const std::vector<NodePtr> &args, EvalState &state,
                          bool method_call);

        /// Reconstructs the dotted selector of a `var.field.field…` chain so the
        /// evaluator can try flat context keys first.
        bool collectDottedPath(const Node *node, std::string &out) {
            if (node == nullptr) return false;
            if (node->kind == NodeKind::Variable) {
                out = node->name;
                return true;
            }
            if (node->kind == NodeKind::Member) {
                std::string base;
                if (!collectDottedPath(node->left.get(), base)) return false;
                out = base + "." + node->name;
                return true;
            }
            return false;
        }

        class Evaluator {
        public:
            [[nodiscard]] static json eval(const NodePtr &node, EvalState &state);
        };

        double requireNumberChecked(const json &value, const char *what) {
            double out = 0.0;
            if (!v::toNumber(value, out)) {
                throw runtime::SapoError(runtime::ErrorCode::Validation,
                                         std::string("expected a number for ") + what + ", got " +
                                             v::typeName(value) + " '" + v::str(value) + "'");
            }
            return out;
        }

        json Evaluator::eval(const NodePtr &node, EvalState &state) {
            if (!node) return nullptr;

            switch (node->kind) {
                case NodeKind::Literal:
                    return node->literal;

                case NodeKind::Variable: {
                    auto local = state.locals.find(node->name);
                    if (local != state.locals.end()) return local->second;
                    json value;
                    if (state.resolver != nullptr && state.resolver->resolve(node->name, value)) return value;
                    if (state.options.strict) {
                        throw runtime::SapoError(runtime::ErrorCode::NotFound,
                                                 "unresolved reference '" +
                                                     (node->explicit_dollar ? "$" + node->name : node->name) +
                                                     "' — no context variable, loop local, env.*, secret.* or config.* binding");
                    }
                    return nullptr;
                }

                case NodeKind::Member: {
                    // Flat dotted-key fast path: the HTTP extractor stores keys like
                    // "result.accountBalance", so `result.accountBalance` must hit too.
                    std::string dotted;
                    if (collectDottedPath(node.get(), dotted) && state.resolver != nullptr) {
                        json flat;
                        if (state.resolver->resolve(dotted, flat)) return flat;
                    }
                    json current = eval(node->left, state);
                    if (node->name == "length" || node->name == "size") {
                        if (current.is_string()) return static_cast<long long>(current.get<std::string>().size());
                        if (current.is_array() || current.is_object()) return static_cast<long long>(current.size());
                        if (current.is_null()) return 0LL;
                    }
                    if (current.is_object() && current.contains(node->name)) return current.at(node->name);
                    if (current.is_array()) {
                        // Implicit projection: `$items.id` over an array of objects.
                        json projected = json::array();
                        bool any = false;
                        for (const auto &item : current) {
                            if (item.is_object() && item.contains(node->name)) {
                                projected.push_back(item.at(node->name));
                                any = true;
                            } else {
                                projected.push_back(nullptr);
                            }
                        }
                        if (any) return projected;
                    }
                    return nullptr; // missing fields are null; only root refs are strict errors
                }

                case NodeKind::Index: {
                    json current = eval(node->left, state);
                    if (current.is_null()) return nullptr;
                    int idx = node->index;
                    std::string dynamic_key;
                    bool has_key = false;
                    if (!node->has_static_index && node->right) {
                        const json index_value = eval(node->right, state);
                        if (index_value.is_string()) {
                            dynamic_key = index_value.get<std::string>();
                            has_key = true;
                        } else {
                            idx = static_cast<int>(requireNumberChecked(index_value, "array index"));
                        }
                    }
                    if (has_key) {
                        if (current.is_object() && current.contains(dynamic_key)) return current.at(dynamic_key);
                        if (current.is_array()) {
                            try {
                                idx = std::stoi(dynamic_key);
                            } catch (...) {
                                return nullptr;
                            }
                        } else {
                            return nullptr;
                        }
                    }
                    if (current.is_array()) {
                        if (idx < 0) idx += static_cast<int>(current.size());
                        if (idx < 0 || static_cast<size_t>(idx) >= current.size()) return nullptr;
                        return current[static_cast<size_t>(idx)];
                    }
                    if (current.is_object()) {
                        const std::string key = std::to_string(idx);
                        if (!current.contains(key)) return nullptr;
                        return current.at(key);
                    }
                    if (current.is_string()) {
                        std::string text = current.get<std::string>();
                        if (idx < 0) idx += static_cast<int>(text.size());
                        if (idx < 0 || static_cast<size_t>(idx) >= text.size()) return nullptr;
                        return std::string(1, text[static_cast<size_t>(idx)]);
                    }
                    return nullptr;
                }

                case NodeKind::Unary: {
                    const json value = eval(node->left, state);
                    if (node->name == "!") return !v::truthy(value);
                    if (node->name == "-") return -requireNumberChecked(value, "unary '-'");
                    if (node->name == "+") return requireNumberChecked(value, "unary '+'");
                    fail("unsupported unary operator '" + node->name + "'", 0, "");
                }

                case NodeKind::Binary: {
                    const std::string &op = node->name;
                    if (op == "&&") {
                        if (!v::truthy(eval(node->left, state))) return false;
                        return v::truthy(eval(node->right, state));
                    }
                    if (op == "||") {
                        if (v::truthy(eval(node->left, state))) return true;
                        return v::truthy(eval(node->right, state));
                    }

                    const json left = eval(node->left, state);
                    const json right = eval(node->right, state);

                    if (op == "==") return v::equals(left, right);
                    if (op == "!=") return !v::equals(left, right);

                    if (op == "in" || op == "contains") {
                        const json &haystack = (op == "in") ? right : left;
                        const json &needle = (op == "in") ? left : right;
                        if (haystack.is_array()) {
                            for (const auto &item : haystack) {
                                if (v::equals(item, needle)) return true;
                            }
                            return false;
                        }
                        if (haystack.is_object()) {
                            return needle.is_string() && haystack.contains(needle.get<std::string>());
                        }
                        return v::str(haystack).find(v::str(needle)) != std::string::npos;
                    }

                    if (op == "=~") {
                        if (!right.is_string()) {
                            throw runtime::SapoError(runtime::ErrorCode::Validation, "right-hand regex must be a string");
                        }
                        try {
                            return std::regex_search(v::str(left), std::regex(right.get<std::string>()));
                        } catch (const std::regex_error &e) {
                            throw runtime::SapoError(runtime::ErrorCode::Validation,
                                                     std::string("invalid regex: ") + e.what());
                        }
                    }

                    double a = 0.0, b = 0.0;
                    const bool left_number = v::toNumber(left, a);
                    const bool right_number = v::toNumber(right, b);
                    const bool both_numeric = left_number && right_number;

                    if (op == "+") {
                        if (both_numeric && left.is_string() == false && right.is_string() == false) return v::tidy(a + b);
                        if (both_numeric && (left.is_number() || left.is_boolean()) &&
                            (right.is_number() || right.is_boolean())) {
                            return v::tidy(a + b);
                        }
                        if (left.is_array() && right.is_array()) {
                            json merged = left;
                            for (const auto &item : right) merged.push_back(item);
                            return merged;
                        }
                        if (left.is_object() && right.is_object()) {
                            json merged = left;
                            merged.merge_patch(right);
                            return merged;
                        }
                        return v::str(left) + v::str(right);
                    }
                    if (!both_numeric) {
                        throw runtime::SapoError(runtime::ErrorCode::Validation, "cannot apply '" + op + "' to " +
                                                                                     v::typeName(left) + " and " +
                                                                                     v::typeName(right));
                    }
                    switch (op[0]) {
                        case '-': return v::tidy(a - b);
                        case '*': return v::tidy(a * b);
                        case '/':
                            if (b == 0.0) throw runtime::SapoError(runtime::ErrorCode::Validation, "division by zero");
                            return v::tidy(a / b);
                        case '%':
                            if (b == 0.0) throw runtime::SapoError(runtime::ErrorCode::Validation, "modulo by zero");
                            return v::tidy(std::fmod(a, b));
                        default: break;
                    }
                    const auto ordering = v::compare(left, right);
                    if (!ordering.has_value()) {
                        throw runtime::SapoError(runtime::ErrorCode::Validation,
                                                 "cannot order " + v::typeName(left) + " against " + v::typeName(right));
                    }
                    if (op == "<") return *ordering < 0;
                    if (op == "<=") return *ordering <= 0;
                    if (op == ">") return *ordering > 0;
                    if (op == ">=") return *ordering >= 0;
                    fail("unsupported operator '" + op + "'", 0, "");
                }

                case NodeKind::Ternary:
                    return v::truthy(eval(node->left, state)) ? eval(node->right, state) : eval(node->extra, state);

                case NodeKind::Coalesce: {
                    EvalState relaxed = state;
                    relaxed.options.strict = false;
                    json value = eval(node->left, relaxed);
                    if (value.is_null()) value = eval(node->right, state);
                    return value;
                }

                case NodeKind::ArrayLiteral: {
                    json out = json::array();
                    for (const auto &item : node->args) out.push_back(eval(item, state));
                    return out;
                }

                case NodeKind::ObjectLiteral: {
                    json out = json::object();
                    for (size_t i = 0; i < node->keys.size() && i < node->args.size(); ++i) {
                        out[node->keys[i]] = eval(node->args[i], state);
                    }
                    return out;
                }

                case NodeKind::Call:
                    return callFunction(node->name, node->args, state, false);

                case NodeKind::MethodCall: {
                    std::vector<NodePtr> args = node->args;
                    args.insert(args.begin(), node->left); // receiver first
                    return callFunction(node->name, args, state, true);
                }
            }
            return nullptr;
        }

        // -----------------------------------------------------------------
        // Standard library
        // -----------------------------------------------------------------
        using FunctionImpl = std::function<json(const std::vector<NodePtr> &, EvalState &)>;

            json evalArg(const std::vector<NodePtr> &args, size_t index, EvalState &state,
                         json fallback = json(nullptr)) {
                if (index >= args.size()) return fallback;
                return Evaluator::eval(args[index], state);
            }

            void arityAtLeast(const std::vector<NodePtr> &args, size_t needed, const std::string &name) {
                if (args.size() < needed) {
                    throw runtime::SapoError(runtime::ErrorCode::Validation,
                                             "function '" + name + "()' expects at least " + std::to_string(needed) +
                                                 " argument(s), got " + std::to_string(args.size()));
                }
            }

            json arrayArg(const std::vector<NodePtr> &args, size_t index, EvalState &state) {
                json value = evalArg(args, index, state);
                if (value.is_array()) return value;
                if (value.is_null()) return json::array();
                if (value.is_object()) return json::array({value});
                return json::array({value});
            }

            struct LocalScope {
                EvalState &state;
                std::vector<std::string> names;
                explicit LocalScope(EvalState &s) : state(s) {}
                void set(std::string name, json value) {
                    names.push_back(name);
                    state.locals[std::move(name)] = std::move(value);
                }
                ~LocalScope() {
                    for (const auto &name : names) state.locals.erase(name);
                }
            };

            /// Higher-order functions accept either an inline expression
            /// (`map(items, item.qty * 10)`) or a quoted DSL-style predicate
            /// (`map(items, "$item.qty * 10")`), which is compiled on demand and
            /// memoised through the shared cache.
            json evalPredicate(const std::vector<NodePtr> &args, size_t index, EvalState &state) {
                if (index >= args.size()) return json(nullptr);
                const NodePtr &arg = args[index];
                if (arg && arg->kind == NodeKind::Literal && arg->literal.is_string()) {
                    const std::string text = arg->literal.get<std::string>();
                    try {
                        const ProgramPtr program = ExpressionCache::instance().getOrCompile(text);
                        return Evaluator::eval(program->root(), state);
                    } catch (const runtime::SapoError &) {
                        return arg->literal; // not an expression after all: plain string
                    }
                }
                return Evaluator::eval(arg, state);
            }

            bool predicateTrue(const std::vector<NodePtr> &args, size_t index, EvalState &state, const json &item,
                               size_t position) {
                LocalScope scope(state);
                scope.set("item", item);
                scope.set("index", position);
                return v::truthy(evalPredicate(args, index, state));
            }

            std::mt19937_64 &rng() {
                static thread_local std::mt19937_64 engine(std::random_device{}());
                return engine;
            }

            const std::unordered_map<std::string, FunctionImpl> &functionTable() {
                static const std::unordered_map<std::string, FunctionImpl> table = [] {
                    std::unordered_map<std::string, FunctionImpl> f;

                    // --- collections / strings ---
                    f["len"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const json value = evalArg(a, 0, s);
                        if (value.is_string()) return static_cast<long long>(value.get<std::string>().size());
                        if (value.is_array() || value.is_object()) return static_cast<long long>(value.size());
                        if (value.is_null()) return 0LL;
                        return 1LL;
                    };
                    f["size"] = f["len"];
                    f["length"] = f["len"];

                    f["upper"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        std::string text = v::str(evalArg(a, 0, s));
                        std::transform(text.begin(), text.end(), text.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                        return text;
                    };
                    f["lower"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        std::string text = v::str(evalArg(a, 0, s));
                        std::transform(text.begin(), text.end(), text.begin(),
                                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        return text;
                    };
                    f["trim"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        std::string text = v::str(evalArg(a, 0, s));
                        const auto not_space = [](unsigned char c) { return !std::isspace(c); };
                        text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
                        text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
                        return text;
                    };

                    f["starts_with"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const std::string text = v::str(evalArg(a, 0, s));
                        const std::string prefix = v::str(evalArg(a, 1, s));
                        return text.rfind(prefix, 0) == 0;
                    };
                    f["startsWith"] = f["starts_with"];
                    f["ends_with"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const std::string text = v::str(evalArg(a, 0, s));
                        const std::string suffix = v::str(evalArg(a, 1, s));
                        return text.size() >= suffix.size() &&
                               text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
                    };
                    f["endsWith"] = f["ends_with"];

                    f["contains"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        arityAtLeast(a, 2, "contains");
                        const json haystack = evalArg(a, 0, s);
                        const json needle = evalArg(a, 1, s);
                        if (haystack.is_array()) {
                            for (const auto &item : haystack) {
                                if (v::equals(item, needle)) return true;
                            }
                            return false;
                        }
                        if (haystack.is_object()) {
                            return needle.is_string() && haystack.contains(needle.get<std::string>());
                        }
                        return v::str(haystack).find(v::str(needle)) != std::string::npos;
                    };

                    f["index_of"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const std::string text = v::str(evalArg(a, 0, s));
                        const std::string needle = v::str(evalArg(a, 1, s));
                        const size_t pos = text.find(needle);
                        return static_cast<long long>(pos == std::string::npos ? -1 : static_cast<long long>(pos));
                    };
                    f["indexOf"] = f["index_of"];

                    f["substring"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const std::string text = v::str(evalArg(a, 0, s));
                        const long long from = static_cast<long long>(requireNumberChecked(evalArg(a, 1, s), "start"));
                        const long long count =
                            a.size() > 2 ? static_cast<long long>(requireNumberChecked(evalArg(a, 2, s), "length"))
                                         : static_cast<long long>(text.size());
                        if (from < 0 || from > static_cast<long long>(text.size())) return std::string();
                        const long long usable = std::min<long long>(count, static_cast<long long>(text.size()) - from);
                        return text.substr(static_cast<size_t>(from), static_cast<size_t>(std::max<long long>(0, usable)));
                    };
                    f["substr"] = f["substring"];

                    f["replace"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        std::string text = v::str(evalArg(a, 0, s));
                        const std::string from = v::str(evalArg(a, 1, s));
                        const std::string to = v::str(evalArg(a, 2, s));
                        if (from.empty()) return text;
                        size_t pos = 0;
                        while ((pos = text.find(from, pos)) != std::string::npos) {
                            text.replace(pos, from.size(), to);
                            pos += to.size();
                        }
                        return text;
                    };

                    f["split"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const std::string text = v::str(evalArg(a, 0, s));
                        const std::string sep = a.size() > 1 ? v::str(evalArg(a, 1, s)) : std::string(1, ',');
                        json out = json::array();
                        if (sep.empty()) {
                            for (char c : text) out.push_back(std::string(1, c));
                            return out;
                        }
                        size_t start = 0;
                        while (true) {
                            const size_t next = text.find(sep, start);
                            if (next == std::string::npos) {
                                out.push_back(text.substr(start));
                                break;
                            }
                            out.push_back(text.substr(start, next - start));
                            start = next + sep.size();
                        }
                        return out;
                    };

                    f["join"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const json items = arrayArg(a, 0, s);
                        const std::string sep = a.size() > 1 ? v::str(evalArg(a, 1, s)) : std::string(1, ',');
                        std::string out;
                        for (size_t i = 0; i < items.size(); ++i) {
                            if (i) out += sep;
                            out += v::str(items[i]);
                        }
                        return out;
                    };

                    f["concat"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        std::string out;
                        for (size_t i = 0; i < a.size(); ++i) {
                            const json value = evalArg(a, i, s);
                            if (value.is_array() && out.empty()) {
                                out = "[" + value.dump() + "]";
                                continue;
                            }
                            out += v::str(value);
                        }
                        return out;
                    };

                    f["keys"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const json value = evalArg(a, 0, s);
                        json out = json::array();
                        if (value.is_object()) {
                            for (auto it = value.begin(); it != value.end(); ++it) out.push_back(it.key());
                        }
                        return out;
                    };
                    f["values"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const json value = evalArg(a, 0, s);
                        if (value.is_object()) {
                            json out = json::array();
                            for (const auto &item : value) out.push_back(item);
                            return out;
                        }
                        if (value.is_array()) return value;
                        return json::array();
                    };
                    f["has"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        arityAtLeast(a, 2, "has");
                        const json value = evalArg(a, 0, s);
                        return util::getPath(value, v::str(evalArg(a, 1, s))).has_value();
                    };
                    f["get"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        arityAtLeast(a, 2, "get");
                        const json value = evalArg(a, 0, s);
                        auto found = util::getPath(value, v::str(evalArg(a, 1, s)));
                        if (found.has_value() && !found->is_null()) return *found;
                        return evalArg(a, 2, s);
                    };

                    // --- type tests / conversions ---
                    f["type"] = [](const std::vector<NodePtr> &a, EvalState &s) { return v::typeName(evalArg(a, 0, s)); };
                    f["is_number"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        double out = 0.0;
                        return v::toNumber(evalArg(a, 0, s), out);
                    };
                    f["isNumber"] = f["is_number"];
                    f["is_decimal"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const json value = evalArg(a, 0, s);
                        if (value.is_number_float()) return true;
                        if (!value.is_string()) return false;
                        const std::string text = value.get<std::string>();
                        if (text.find('.') == std::string::npos) return false;
                        size_t consumed = 0;
                        try {
                            const double parsed = std::stod(text, &consumed);
                            return consumed == text.size() && std::isfinite(parsed);
                        } catch (...) {
                            return false;
                        }
                    };
                    f["is_integer"] = [](const std::vector<NodePtr> &a, EvalState &s) { return v::isIntegerValue(evalArg(a, 0, s)); };
                    f["isInteger"] = f["is_integer"];
                    f["is_string"] = [](const std::vector<NodePtr> &a, EvalState &s) { return evalArg(a, 0, s).is_string(); };
                    f["is_bool"] = [](const std::vector<NodePtr> &a, EvalState &s) { return evalArg(a, 0, s).is_boolean(); };
                    f["is_null"] = [](const std::vector<NodePtr> &a, EvalState &s) { return evalArg(a, 0, s).is_null(); };
                    f["is_empty"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const json value = evalArg(a, 0, s);
                        if (value.is_null()) return true;
                        if (value.is_string()) return value.get<std::string>().empty();
                        if (value.is_array() || value.is_object()) return value.empty();
                        return false;
                    };
                    f["is_array"] = [](const std::vector<NodePtr> &a, EvalState &s) { return evalArg(a, 0, s).is_array(); };
                    f["is_object"] = [](const std::vector<NodePtr> &a, EvalState &s) { return evalArg(a, 0, s).is_object(); };

                    f["to_string"] = [](const std::vector<NodePtr> &a, EvalState &s) { return v::str(evalArg(a, 0, s)); };
                    f["toString"] = f["to_string"];
                    f["to_number"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const json value = evalArg(a, 0, s);
                        double out = 0.0;
                        if (!v::toNumber(value, out)) {
                            if (a.size() > 1) return evalArg(a, 1, s);
                            throw runtime::SapoError(runtime::ErrorCode::Validation,
                                                     "to_number() cannot convert '" + v::str(value) + "'");
                        }
                        if (std::isfinite(out) && out == std::floor(out) && std::abs(out) < 9.0e15) {
                            return json(static_cast<long long>(out));
                        }
                        return json(v::tidy(out));
                    };
                    f["toNumber"] = f["to_number"];
                    f["to_int"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const json value = evalArg(a, 0, s);
                        double out = 0.0;
                        if (!v::toNumber(value, out)) {
                            if (a.size() > 1) return evalArg(a, 1, s);
                            throw runtime::SapoError(runtime::ErrorCode::Validation,
                                                     "to_int() cannot convert '" + v::str(value) + "'");
                        }
                        return json(static_cast<long long>(std::llround(out)));
                    };
                    f["to_bool"] = [](const std::vector<NodePtr> &a, EvalState &s) { return v::truthy(evalArg(a, 0, s)); };
                    f["to_json"] = [](const std::vector<NodePtr> &a, EvalState &s) { return evalArg(a, 0, s).dump(); };
                    f["fromJson"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const std::string text = v::str(evalArg(a, 0, s));
                        try {
                            return json::parse(text);
                        } catch (const std::exception &) {
                            if (a.size() > 1) return evalArg(a, 1, s);
                            throw runtime::SapoError(runtime::ErrorCode::Validation, "from_json() received invalid JSON");
                        }
                    };
                    f["from_json"] = f["fromJson"];

                    // --- math ---
                    f["abs"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        return std::abs(requireNumberChecked(evalArg(a, 0, s), "abs()"));
                    };
                    f["floor"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        return static_cast<long long>(std::floor(requireNumberChecked(evalArg(a, 0, s), "floor()")));
                    };
                    f["ceil"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        return static_cast<long long>(std::ceil(requireNumberChecked(evalArg(a, 0, s), "ceil()")));
                    };
                    f["round"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const double value = requireNumberChecked(evalArg(a, 0, s), "round()");
                        const int digits = a.size() > 1 ? static_cast<int>(requireNumberChecked(evalArg(a, 1, s), "digits")) : 0;
                        const double factor = std::pow(10.0, digits);
                        const double rounded = std::round(value * factor) / factor;
                        if (digits <= 0) return json(static_cast<long long>(rounded));
                        return json(rounded);
                    };
                    f["min"] = [](const std::vector<NodePtr> &a, EvalState &s) -> json {
                        if (a.empty()) return json(nullptr);
                        if (a.size() == 1) {
                            const json items = evalArg(a, 0, s);
                            if (items.is_array()) {
                                double best = std::numeric_limits<double>::max();
                                for (const auto &item : items) best = std::min(best, requireNumberChecked(item, "min()"));
                                return json(best);
                            }
                        }
                        double best = std::numeric_limits<double>::max();
                        for (size_t i = 0; i < a.size(); ++i) best = std::min(best, requireNumberChecked(evalArg(a, i, s), "min()"));
                        return json(best);
                    };
                    f["max"] = [](const std::vector<NodePtr> &a, EvalState &s) -> json {
                        if (a.empty()) return json(nullptr);
                        if (a.size() == 1) {
                            const json items = evalArg(a, 0, s);
                            if (items.is_array()) {
                                double best = std::numeric_limits<double>::lowest();
                                for (const auto &item : items) best = std::max(best, requireNumberChecked(item, "max()"));
                                return json(best);
                            }
                        }
                        double best = std::numeric_limits<double>::lowest();
                        for (size_t i = 0; i < a.size(); ++i) best = std::max(best, requireNumberChecked(evalArg(a, i, s), "max()"));
                        return json(best);
                    };
                    f["sum"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const json value = evalArg(a, 0, s);
                        double total = 0.0;
                        if (a.size() > 1 && value.is_array()) {
                            for (size_t i = 0; i < value.size(); ++i) {
                                LocalScope scope(s);
                                scope.set("item", value[i]);
                                scope.set("index", i);
                                total += requireNumberChecked(evalPredicate(a, 1, s), "sum()");
                            }
                            return total;
                        }
                        if (value.is_array()) {
                            for (const auto &item : value) {
                                double numeric = 0.0;
                                if (v::toNumber(item, numeric)) total += numeric;
                            }
                        }
                        return v::tidy(total);
                    };
                    f["avg"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const json items = arrayArg(a, 0, s);
                        if (items.empty()) return 0.0;
                        double total = 0.0;
                        size_t counted = 0;
                        for (const auto &item : items) {
                            if (item.is_number() || item.is_string()) {
                                total += requireNumberChecked(item, "avg()");
                                ++counted;
                            }
                        }
                        return counted ? v::tidy(total / static_cast<double>(counted)) : 0.0;
                    };
                    f["pow"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        return std::pow(requireNumberChecked(evalArg(a, 0, s), "pow() base"),
                                       requireNumberChecked(evalArg(a, 1, s), "pow() exponent"));
                    };
                    f["sqrt"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        return std::sqrt(requireNumberChecked(evalArg(a, 0, s), "sqrt()"));
                    };
                    f["mod"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const double divisor = requireNumberChecked(evalArg(a, 1, s), "mod()");
                        if (divisor == 0.0) throw runtime::SapoError(runtime::ErrorCode::Validation, "mod() by zero");
                        return std::fmod(requireNumberChecked(evalArg(a, 0, s), "mod()"), divisor);
                    };

                    // --- logic / control ---
                    f["coalesce"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        EvalState relaxed = s;
                        relaxed.options.strict = false;
                        for (const auto &arg : a) {
                            json value = Evaluator::eval(arg, relaxed);
                            if (!value.is_null() && !(value.is_string() && value.get<std::string>().empty())) return value;
                        }
                        return json(nullptr);
                    };
                    f["default"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        arityAtLeast(a, 2, "default");
                        EvalState relaxed = s;
                        relaxed.options.strict = false;
                        json value = Evaluator::eval(a[0], relaxed);
                        return value.is_null() ? evalArg(a, 1, s) : value;
                    };
                    f["if"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        arityAtLeast(a, 2, "if");
                        if (v::truthy(evalArg(a, 0, s))) return evalArg(a, 1, s);
                        return a.size() > 2 ? evalArg(a, 2, s) : json(nullptr);
                    };

                    // --- time ---
                    f["now"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        if (!a.empty()) {
                            const std::string unit = v::str(evalArg(a, 0, s));
                            const int64_t ms = nowMillis();
                            if (unit == "ms" || unit == "millis") return json(ms);
                            if (unit == "s" || unit == "seconds") return json(ms / 1000);
                        }
                        return json(util::formatIso8601(nowMillis()));
                    };
                    f["timestamp"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        if (a.empty()) return json(nowMillis() / 1000);
                        const json value = evalArg(a, 0, s);
                        if (value.is_string()) {
                            auto parsed = util::parseIso8601(value.get<std::string>());
                            if (parsed.has_value()) return json(*parsed / 1000);
                            throw runtime::SapoError(runtime::ErrorCode::Validation,
                                                     "timestamp() cannot parse '" + v::str(value) + "'");
                        }
                        return json(static_cast<long long>(requireNumberChecked(value, "timestamp()")));
                    };
                    f["format_date"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        int64_t epoch_ms = nowMillis();
                        if (!a.empty()) {
                            const json value = evalArg(a, 0, s);
                            if (value.is_string()) {
                                const auto parsed = util::parseIso8601(value.get<std::string>());
                                if (!parsed.has_value()) {
                                    throw runtime::SapoError(runtime::ErrorCode::Validation,
                                                             "format_date() cannot parse '" + v::str(value) + "'");
                                }
                                epoch_ms = *parsed;
                            } else {
                                epoch_ms = static_cast<int64_t>(requireNumberChecked(value, "format_date()"));
                                if (epoch_ms < 100000000000LL) epoch_ms *= 1000; // seconds given
                            }
                        }
                        const std::string fmt = a.size() > 1 ? v::str(evalArg(a, 1, s)) : std::string("%Y-%m-%d");
                        const auto broken = util::breakDown(epoch_ms, 0);
                        std::tm std_tm{};
                        std_tm.tm_year = broken.year - 1900;
                        std_tm.tm_mon = broken.month - 1;
                        std_tm.tm_mday = broken.day;
                        std_tm.tm_hour = broken.hour;
                        std_tm.tm_min = broken.minute;
                        std_tm.tm_sec = broken.second;
                        char buffer[128];
                        if (std::strftime(buffer, sizeof(buffer), fmt.c_str(), &std_tm) == 0) return std::string();
                        return std::string(buffer);
                    };
                    f["date_add"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        arityAtLeast(a, 2, "date_add");
                        int64_t epoch_ms = nowMillis();
                        const json base = evalArg(a, 0, s);
                        if (base.is_string()) {
                            epoch_ms = util::parseIso8601(base.get<std::string>()).value_or(epoch_ms);
                        } else if (base.is_number()) {
                            epoch_ms = static_cast<int64_t>(base.get<double>());
                            if (epoch_ms < 100000000000LL) epoch_ms *= 1000;
                        }
                        const auto delta = util::parseDuration(v::str(evalArg(a, 1, s)));
                        if (!delta.has_value()) {
                            throw runtime::SapoError(runtime::ErrorCode::Validation, "date_add() got an invalid duration");
                        }
                        return json(util::formatIso8601(epoch_ms + delta->count()));
                    };

                    // --- encoding / crypto ---
                    f["base64_encode"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        return util::base64Encode(v::str(evalArg(a, 0, s)));
                    };
                    f["base64_decode"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const auto decoded = util::base64Decode(v::str(evalArg(a, 0, s)));
                        if (!decoded.has_value() && s.options.strict) {
                            throw runtime::SapoError(runtime::ErrorCode::Validation, "base64_decode() received invalid input");
                        }
                        return decoded.value_or(std::string());
                    };
                    f["url_encode"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        return util::percentEncode(v::str(evalArg(a, 0, s)));
                    };
                    f["hex_encode"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        return util::toHex(v::str(evalArg(a, 0, s)));
                    };
                    f["sha256"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        return util::digestHex(util::DigestKind::Sha256, v::str(evalArg(a, 0, s)));
                    };
                    f["md5"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        return util::digestHex(util::DigestKind::Md5, v::str(evalArg(a, 0, s)));
                    };
                    f["hash"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        arityAtLeast(a, 1, "hash");
                        const std::string algo = a.size() > 1 ? v::str(evalArg(a, 1, s)) : "sha256";
                        const auto value = util::digestHexByName(algo, v::str(evalArg(a, 0, s)));
                        if (!value.has_value()) {
                            throw runtime::SapoError(runtime::ErrorCode::Validation, "unsupported hash algorithm '" + algo + "'");
                        }
                        return *value;
                    };
                    f["hmac"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        arityAtLeast(a, 2, "hmac");
                        const std::string algo = a.size() > 2 ? v::str(evalArg(a, 2, s)) : "sha256";
                        const auto value = util::hmacHex(algo, v::str(evalArg(a, 0, s)), v::str(evalArg(a, 1, s)));
                        if (!value.has_value()) {
                            throw runtime::SapoError(runtime::ErrorCode::Validation,
                                                     "unsupported hmac algorithm '" + algo + "'");
                        }
                        return *value;
                    };
                    f["uuid"] = [](const std::vector<NodePtr> &, EvalState &) { return util::uuidV4(); };
                    f["random_int"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const long long low = a.size() > 0 ? static_cast<long long>(requireNumberChecked(evalArg(a, 0, s), "low")) : 0;
                        const long long high =
                            a.size() > 1 ? static_cast<long long>(requireNumberChecked(evalArg(a, 1, s), "high")) : 1000000LL;
                        std::uniform_int_distribution<long long> dist(low, std::max(low, high));
                        return json(dist(rng()));
                    };
                    f["random_pin"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const int digits = a.empty() ? 6 : static_cast<int>(requireNumberChecked(evalArg(a, 0, s), "digits"));
                        return util::randomPin(static_cast<size_t>(std::max(1, std::min(digits, 32))));
                    };

                    // --- higher order ---
                    f["sort"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        json items = arrayArg(a, 0, s);
                        if (a.size() > 1) {
                            json keyed = json::array();
                            for (size_t i = 0; i < items.size(); ++i) {
                                LocalScope scope(s);
                                scope.set("item", items[i]);
                                scope.set("index", i);
                                keyed.push_back({{"key", evalPredicate(a, 1, s)}, {"value", items[i]}});
                            }
                            std::stable_sort(keyed.begin(), keyed.end(), [](const json &x, const json &y) {
                                const auto ordering = v::compare(x["key"], y["key"]);
                                return ordering.has_value() && *ordering < 0;
                            });
                            json out = json::array();
                            for (auto &entry : keyed) out.push_back(entry["value"]);
                            return out;
                        }
                        std::stable_sort(items.begin(), items.end(), [](const json &x, const json &y) {
                            const auto ordering = v::compare(x, y);
                            return ordering.has_value() && *ordering < 0;
                        });
                        return items;
                    };
                    f["reverse"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        json items = arrayArg(a, 0, s);
                        std::reverse(items.begin(), items.end());
                        return items;
                    };
                    f["unique"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const json items = arrayArg(a, 0, s);
                        json out = json::array();
                        for (const auto &item : items) {
                            bool seen = false;
                            for (const auto &existing : out) {
                                if (v::equals(existing, item)) { seen = true; break; }
                            }
                            if (!seen) out.push_back(item);
                        }
                        return out;
                    };
                    f["slice"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const json items = arrayArg(a, 0, s);
                        const long long size = static_cast<long long>(items.size());
                        long long from = a.size() > 1 ? static_cast<long long>(requireNumberChecked(evalArg(a, 1, s), "from")) : 0;
                        long long to = a.size() > 2 ? static_cast<long long>(requireNumberChecked(evalArg(a, 2, s), "to")) : size;
                        if (from < 0) from = std::max<long long>(0, size + from);
                        if (to < 0) to = std::max<long long>(0, size + to);
                        from = std::clamp<long long>(from, 0, size);
                        to = std::clamp<long long>(to, from, size);
                        json out = json::array();
                        for (long long i = from; i < to; ++i) out.push_back(items[static_cast<size_t>(i)]);
                        return out;
                    };
                    f["first"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const json items = arrayArg(a, 0, s);
                        return items.empty() ? json(nullptr) : items.front();
                    };
                    f["last"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const json items = arrayArg(a, 0, s);
                        return items.empty() ? json(nullptr) : items.back();
                    };
                    f["push"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        json items = arrayArg(a, 0, s);
                        for (size_t i = 1; i < a.size(); ++i) items.push_back(evalArg(a, i, s));
                        return items;
                    };
                    // `append` is the spelling blueprint authors reach for first.
                    f["append"] = f["push"];
                    f["merge"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        json out = json::object();
                        for (size_t i = 0; i < a.size(); ++i) {
                            const json value = evalArg(a, i, s);
                            if (value.is_object()) out.merge_patch(value);
                        }
                        return out;
                    };
                    f["count"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const json items = arrayArg(a, 0, s);
                        if (a.size() < 2) return static_cast<long long>(items.size());
                        long long matched = 0;
                        for (size_t i = 0; i < items.size(); ++i) {
                            if (predicateTrue(a, 1, s, items[i], i)) ++matched;
                        }
                        return matched;
                    };
                    f["any"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const json items = arrayArg(a, 0, s);
                        for (size_t i = 0; i < items.size(); ++i) {
                            if (predicateTrue(a, 1, s, items[i], i)) return true;
                        }
                        return false;
                    };
                    f["all"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const json items = arrayArg(a, 0, s);
                        for (size_t i = 0; i < items.size(); ++i) {
                            if (!predicateTrue(a, 1, s, items[i], i)) return false;
                        }
                        return true;
                    };
                    f["none"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const json items = arrayArg(a, 0, s);
                        for (size_t i = 0; i < items.size(); ++i) {
                            if (predicateTrue(a, 1, s, items[i], i)) return false;
                        }
                        return true;
                    };
                    f["filter"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const json items = arrayArg(a, 0, s);
                        if (a.size() < 2) return items;
                        json out = json::array();
                        for (size_t i = 0; i < items.size(); ++i) {
                            if (predicateTrue(a, 1, s, items[i], i)) out.push_back(items[i]);
                        }
                        return out;
                    };
                    f["map"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        const json items = arrayArg(a, 0, s);
                        if (a.size() < 2) return items;
                        json out = json::array();
                        for (size_t i = 0; i < items.size(); ++i) {
                            LocalScope scope(s);
                            scope.set("item", items[i]);
                            scope.set("index", i);
                            out.push_back(evalPredicate(a, 1, s));
                        }
                        return out;
                    };
                    f["matches"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        arityAtLeast(a, 2, "matches");
                        const std::string text = v::str(evalArg(a, 0, s));
                        const std::string pattern = v::str(evalArg(a, 1, s));
                        try {
                            return std::regex_match(text, std::regex(pattern));
                        } catch (const std::regex_error &e) {
                            throw runtime::SapoError(runtime::ErrorCode::Validation, std::string("invalid regex: ") + e.what());
                        }
                    };
                    f["equals"] = [](const std::vector<NodePtr> &a, EvalState &s) {
                        arityAtLeast(a, 2, "equals");
                        return v::equals(evalArg(a, 0, s), evalArg(a, 1, s));
                    };
                    f["regexp"] = f["matches"];

                    return f;
                }();
                return table;
            }

            json callFunction(const std::string &name, const std::vector<NodePtr> &args, EvalState &state,
                              bool method_call) {
                const auto &table = functionTable();
                auto it = table.find(name);
                if (it == table.end()) {
                    if (method_call && !args.empty()) {
                        // `obj.field()` where `field` is data, not a function.
                        const json receiver = Evaluator::eval(args.front(), state);
                        if (receiver.is_object() && receiver.contains(name)) return receiver.at(name);
                    }
                    throw runtime::SapoError(runtime::ErrorCode::Validation,
                                             "unknown function '" + name + "()' — not part of the SEL standard library");
                }
                return it->second(args, state);
            }

    } // namespace (language internals)

    // =====================================================================
    // Public surface
        // =====================================================================
    namespace {
        std::function<int64_t()> g_clock_provider;
    } // namespace

        void setClockProvider(std::function<int64_t()> provider) { g_clock_provider = std::move(provider); }

        int64_t nowMillis() {
            if (g_clock_provider) return g_clock_provider();
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        }

        json evaluate(const Program &program, EvalState &state) { return Evaluator::eval(program.root(), state); }

        ProgramPtr compile(const std::string &source) { return ExpressionCache::instance().getOrCompile(source); }

        bool isPlainLiteral(const std::string &source) { return source.empty() || source.find('$') == std::string::npos; }

        bool looksLikeExpression(const std::string &source) {
            static const std::string kOperators = "=<>!+-*/%?&|(){}";
            return source.find_first_of(kOperators) != std::string::npos;
        }

        ProgramPtr ExpressionCache::getOrCompile(const std::string &source) {
            {
                std::scoped_lock lock(m_mutex);
                auto it = m_programs.find(source);
                if (it != m_programs.end()) return it->second;
            }
            NodePtr root;
            try {
                Parser parser(source);
                root = parser.parseProgram();
            } catch (const runtime::SapoError &) {
                throw;
            } catch (const std::exception &e) {
                throw runtime::SapoError(runtime::ErrorCode::Expression,
                                         std::string("failed to compile expression: ") + e.what(),
                                         json{{"expression", source}});
            }
            auto program = std::make_shared<const Program>(root, source);
            std::scoped_lock lock(m_mutex);
            if (m_programs.size() >= kSoftLimit) m_programs.clear();
            m_programs.emplace(source, program);
            return program;
        }

        TemplatePtr ExpressionCache::getOrCompileTemplate(const std::string &source) {
            {
                std::scoped_lock lock(m_mutex);
                auto it = m_templates.find(source);
                if (it != m_templates.end()) return it->second;
            }

            std::vector<Template::Segment> segments;
            std::string literal;
            size_t i = 0;
            while (i < source.size()) {
                const char c = source[i];
                if (c == '\\' && i + 1 < source.size() && source[i + 1] == '$') {
                    literal.push_back('$');
                    i += 2;
                    continue;
                }
                if (c == '$' && i + 1 < source.size() && source[i + 1] == '$') {
                    literal.push_back('$');
                    i += 2;
                    continue;
                }
                if (c == '$' && i + 1 < source.size() && source[i + 1] == '{') {
                    if (!literal.empty()) {
                        segments.push_back({literal, nullptr});
                        literal.clear();
                    }
                    size_t j = i + 2;
                    int depth = 1;
                    std::string inner;
                    char quote = 0;
                    while (j < source.size() && depth > 0) {
                        const char current = source[j];
                        if (quote != 0) {
                            if (current == '\\') {
                                inner.push_back(current);
                                ++j;
                                if (j < source.size()) {
                                    inner.push_back(source[j]);
                                    ++j;
                                }
                                continue;
                            }
                            if (current == quote) quote = 0;
                            inner.push_back(current);
                            ++j;
                            continue;
                        }
                        if (current == '"' || current == '\'') {
                            quote = current;
                            inner.push_back(current);
                            ++j;
                            continue;
                        }
                        if (current == '{') ++depth;
                        if (current == '}') {
                            --depth;
                            if (depth == 0) break;
                        }
                        inner.push_back(current);
                        ++j;
                    }
                    if (depth != 0) {
                        throw runtime::SapoError(runtime::ErrorCode::Expression, "unterminated '${' interpolation",
                                                 json{{"expression", source}});
                    }
                    segments.push_back({inner, getOrCompile(inner)});
                    i = j + 1;
                    continue;
                }
                if (c == '$' && i + 1 < source.size() && (isIdentStart(source[i + 1]))) {
                    if (!literal.empty()) {
                        segments.push_back({literal, nullptr});
                        literal.clear();
                    }
                    size_t j = i + 1;
                    std::string path;
                    while (j < source.size() && isIdentChar(source[j])) path.push_back(source[j++]);
                    if (!path.empty() && path.back() == '.') path.pop_back();
                    // Optional dotted continuation: `$user.isSubscribed`
                    while (j + 1 < source.size() && source[j] == '.' && isIdentStart(source[j + 1])) {
                        path.push_back('.');
                        ++j;
                        while (j < source.size() && isIdentChar(source[j])) path.push_back(source[j++]);
                    }
                    while (j < source.size() && source[j] == '[') {
                        const size_t close = source.find(']', j);
                        if (close == std::string::npos) break;
                        path.append(source.substr(j, close - j + 1));
                        j = close + 1;
                    }
                    segments.push_back({path, getOrCompile(path)});
                    i = j;
                    continue;
                }
                literal.push_back(c);
                ++i;
            }
            if (!literal.empty() || segments.empty()) segments.push_back({literal, nullptr});

            auto tpl = std::make_shared<const Template>(source, std::move(segments));
            std::scoped_lock lock(m_mutex);
            if (m_templates.size() >= kSoftLimit) m_templates.clear();
            m_templates.emplace(source, tpl);
            return tpl;
        }

        json Template::render(EvalState &state) const {
            if (isSingleInterpolation()) {
                return Evaluator::eval(m_segments.front().program->root(), state);
            }
            std::string out;
            for (const auto &segment : m_segments) {
                if (!segment.program) {
                    out += segment.literal;
                    continue;
                }
                const json value = Evaluator::eval(segment.program->root(), state);
                if (value.is_string()) out += value.get<std::string>();
                else if (!value.is_null()) out += v::str(value);
            }
            return out;
        }

        TemplatePtr compileTemplate(const std::string &source) { return ExpressionCache::instance().getOrCompileTemplate(source); }

        Expression::Expression(std::string source) : m_source(std::move(source)) {
            if (m_source.empty()) return;
            m_template = ExpressionCache::instance().getOrCompileTemplate(m_source);
            if (m_template->isSingleInterpolation()) {
                // "https://x/$id" style selectors keep the resolved JSON type.
                m_program = m_template->segments().front().program;
            }
        }

        Expression Expression::fromExpression(std::string source) {
            Expression expression;
            expression.m_source = std::move(source);
            if (!expression.m_source.empty()) {
                expression.m_program = ExpressionCache::instance().getOrCompile(expression.m_source);
                expression.m_template = ExpressionCache::instance().getOrCompileTemplate(expression.m_source);
            }
            return expression;
        }

        Expression Expression::fromLiteral(json value) {
            auto literal_node = std::make_shared<Node>();
            literal_node->kind = NodeKind::Literal;
            literal_node->literal = std::move(value);
            Expression expression;
            expression.m_source = literal_node->literal.dump();
            expression.m_program = std::make_shared<const Program>(literal_node, expression.m_source);
            expression.m_template = std::make_shared<const Template>(expression.m_source,
                                                                     std::vector<Template::Segment>{{expression.m_source, nullptr}});
            return expression;
        }

        json Expression::resolve(const IResolver &resolver, const EvalOptions &options) const {
            if (m_source.empty()) return json(m_source);
            if (m_template && m_template->isPlainLiteral()) return json(m_source);
            EvalState state;
            state.resolver = &resolver;
            state.options = options;
            if (m_template) return m_template->render(state);
            if (m_program) return Evaluator::eval(m_program->root(), state);
            return json(m_source);
        }

        json Expression::resolveJson(const IResolver &resolver, const EvalOptions &options, const json &fallback) const {
            if (empty()) return fallback;
            return resolve(resolver, options);
        }

        json Expression::evaluate(const IResolver &resolver, const EvalOptions &options) const {
            if (m_source.empty()) return json(nullptr);
            EvalState state;
            state.resolver = &resolver;
            state.options = options;
            if (m_program) return Evaluator::eval(m_program->root(), state);
            if (m_template) return m_template->render(state);
            return json(m_source);
        }

        bool Expression::evaluateBool(const IResolver &resolver, const EvalOptions &options) const {
            return v::truthy(evaluate(resolver, options));
        }

        std::string Expression::normalizeSource(const json &value) {
            if (value.is_null()) return "null";
            if (value.is_boolean() || value.is_number()) return value.dump();
            if (value.is_string()) return value.get<std::string>();
            if (value.is_object() && value.contains("operator") && (value.contains("left") || value.contains("right"))) {
                const auto side = [](const json &endpoint) {
                    if (endpoint.is_string()) {
                        const std::string text = endpoint.get<std::string>();
                        if (text.find_first_of("=<>+-*/%?&|") != std::string::npos) return "(" + text + ")";
                        return text;
                    }
                    return normalizeSource(endpoint);
                };
                return "(" + side(value.value("left", json(nullptr))) + ") " +
                       value.value("operator", "==") + " (" + side(value.value("right", json(nullptr))) + ")";
            }
            if (value.is_object() && value.contains("function")) {
                std::string out = value.at("function").get<std::string>() + "(";
                if (value.contains("args") && value["args"].is_array()) {
                    const auto &args = value["args"];
                    for (size_t i = 0; i < args.size(); ++i) {
                        if (i) out += ", ";
                        out += normalizeSource(args[i]);
                    }
                }
                return out + ")";
            }
            return value.dump();
        }

        Expression Expression::fromJson(const json &value) {
            if (value.is_string()) return Expression(value.get<std::string>());
            if (value.is_null()) return Expression();
            if ((value.is_number() || value.is_boolean())) return Expression::fromLiteral(value);
            return Expression(normalizeSource(value));
        }

        void Expression::setJson(const json &value) { *this = Expression::fromJson(value); }

        size_t ExpressionCache::programs() const {
            std::scoped_lock lock(m_mutex);
            return m_programs.size();
        }

        size_t ExpressionCache::templates() const {
            std::scoped_lock lock(m_mutex);
            return m_templates.size();
        }

        void ExpressionCache::clear() {
            std::scoped_lock lock(m_mutex);
            m_programs.clear();
            m_templates.clear();
        }

        ExpressionCache &ExpressionCache::instance() {
            static ExpressionCache cache;
            return cache;
        }

} // namespace sapo::expr
