// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "internal.h"

#include <cstdlib>
#include <locale>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace libksp::detail {
namespace {

std::int64_t parse_integer_literal(const std::string &text) {
    const bool hexadecimal =
            text.size() > 1 && (text.back() == 'h' || text.back() == 'H');
    if (!hexadecimal)
        return static_cast<std::int32_t>(std::atoi(text.c_str()));

    unsigned int value = 0;
    std::sscanf(text.c_str(), "%x", &value);
    return static_cast<std::int32_t>(value);
}

double parse_real_literal(const std::string &text) {
    if (!text.empty() && text.front() == '.')
        return std::atof(text.c_str());

    double value = 0.0;
    std::stringstream input(text);
    input.imbue(std::locale("C"));
    input >> value;
    return value;
}

class Parser {
  public:
    Parser(const std::vector<Token> &tokens,
            std::vector<Diagnostic> &diagnostics, Host *host)
        : tokens(tokens), diagnostics(diagnostics), host(host) {}

    std::unordered_map<std::string, std::vector<Statement>> parse() {
        std::unordered_map<std::string, std::vector<Statement>> callbacks;
        skip_lines();
        while (!at(TokenKind::eof)) {
            if (word("function")) {
                take();
                if (!at(TokenKind::identifier)) {
                    error("syntax error");
                    skip_line();
                    continue;
                }
                const auto name = lowercase(take().text);
                skip_line();
                auto body = block({ { "end", "function" } });
                if (!functions.emplace(name, std::move(body)).second)
                    error("Function \"" + name + "\" was already declared!");
                if (word("end") && word("function", 1)) {
                    take();
                    take();
                    skip_line();
                } else
                    error("syntax error");
                skip_lines();
                continue;
            }
            if (!word("on")) {
                error("syntax error");
                skip_line();
                continue;
            }
            take();
            if (!at(TokenKind::identifier)) {
                error("syntax error");
                skip_line();
                continue;
            }
            const auto name = canonical_callback_name(take().text);
            static const std::unordered_set<std::string> callback_names = {
                "async_complete", "controller", "init", "listener", "midi_in",
                "note", "modulate", "note_controller", "nrpn",
                "persistence_changed", "pgs_changed", "poly_at", "release",
                "rpn", "ui_control", "ui_controls", "ui_update"
            };
            if (!callback_names.contains(name))
                error("syntax error");
            std::string callback_variable;
            if (at(TokenKind::lparen)) {
                take();
                if (at(TokenKind::variable)) {
                    callback_variable = lowercase(take().text);
                } else {
                    error("syntax error");
                }
                if (at(TokenKind::rparen))
                    take();
                else
                    error("syntax error");
            }
            skip_line();
            auto body = block({ { "end", "on" } });
            if (name == "ui_control") {
                if (callback_variable.empty()) {
                    error("syntax error");
                } else {
                    ui_callbacks.insert_or_assign(
                            callback_variable, std::move(body));
                }
            } else if (!callback_variable.empty()) {
                error("syntax error");
            } else if (!callbacks.emplace(name, std::move(body)).second) {
                error(name + " callback was already defined!");
            }
            if (word("end") && word("on", 1)) {
                take();
                take();
                skip_line();
            } else
                error("syntax error");
            skip_lines();
        }
        return callbacks;
    }
    std::unordered_map<std::string, std::vector<Statement>> functions;
    std::unordered_map<std::string, std::vector<Statement>> ui_callbacks;

  private:
    const std::vector<Token> &tokens;
    std::vector<Diagnostic> &diagnostics;
    Host *host;
    std::size_t position{};
    bool at(TokenKind k, std::size_t n = 0) const {
        return tokens[std::min(position + n, tokens.size() - 1)].kind == k;
    }
    bool word(std::string_view w, std::size_t n = 0) const {
        const auto &x = tokens[std::min(position + n, tokens.size() - 1)];
        return x.kind == TokenKind::identifier && x.text == w;
    }
    Token take() { return tokens[position++]; }
    void error(std::string message) {
        const auto index = std::min(position, tokens.size() - 1);
        diagnostics.push_back({ tokens[index].location, std::move(message) });
    }
    void skip_line() {
        while (!at(TokenKind::newline) && !at(TokenKind::eof))
            take();
        if (at(TokenKind::newline))
            take();
    }
    void skip_lines() {
        while (at(TokenKind::newline))
            take();
    }
    bool terminator(
            const std::vector<std::pair<std::string, std::string>> &terms)
            const {
        for (const auto &[a, b] : terms)
            if (word(a) && (b.empty() || word(b, 1)))
                return true;
        return false;
    }
    std::vector<Statement> block(
            std::vector<std::pair<std::string, std::string>> terms) {
        std::vector<Statement> result;
        skip_lines();
        while (!at(TokenKind::eof) && !terminator(terms)) {
            if (word("SET_CONDITION") || word("RESET_CONDITION") ||
                    word("USE_CODE_IF") || word("USE_CODE_IF_NOT")) {
                const auto directive = take().text;
                std::string symbol;
                if (!at(TokenKind::lparen))
                    error("syntax error");
                else
                    take();
                if (!at(TokenKind::identifier))
                    error("syntax error");
                else
                    symbol = take().text;
                if (!at(TokenKind::rparen))
                    error("syntax error");
                else
                    take();
                skip_line();
                if (directive == "SET_CONDITION") {
                    if (host)
                        host->define_script_condition(symbol);
                } else if (directive == "RESET_CONDITION") {
                    if (host)
                        host->undefine_script_condition(symbol);
                } else {
                    auto body = block({ { "END_USE_CODE", "" } });
                    if (word("END_USE_CODE")) {
                        take();
                        skip_line();
                    } else
                        error("syntax error");
                    // parse even if the condition is false, discarded bodies
                    // can still define/reset compile-time symbols
                    if (!host || host->script_condition_defined(symbol) ==
                                         (directive == "USE_CODE_IF")) {
                        for (auto &entry : body)
                            result.push_back(std::move(entry));
                    }
                }
                skip_lines();
                continue;
            }
            auto s = statement();
            if (s)
                result.push_back(std::move(*s));
            else
                skip_line();
            skip_lines();
        }
        return result;
    }
    std::optional<Statement> statement() {
        Statement result;
        result.location = tokens[position].location;
        if (word("declare")) {
            take();
            result.kind = Statement::Kind::declare;
            // modifiers come before the variable, we check them later
            while (at(TokenKind::identifier)) {
                const auto modifier = take().text;
                result.is_constant |= modifier == "const";
                result.is_ui_control |= modifier.starts_with("ui_");
                if (modifier.starts_with("ui_"))
                    result.ui_type = modifier;
                result.is_polyphonic |= modifier == "polyphonic";
            }
            if (!at(TokenKind::variable)) {
                error("syntax error");
                return {};
            }
            result.source_name = take().text;
            result.name = lowercase(result.source_name);
            if (at(TokenKind::lbracket)) {
                take();
                result.index = expression();
                if (!at(TokenKind::rbracket))
                    error("syntax error");
                else
                    take();
            }
            if (at(TokenKind::lparen)) {
                take();
                while (!at(TokenKind::rparen) && !at(TokenKind::eof)) {
                    result.ui_arguments.push_back(expression());
                    if (at(TokenKind::comma))
                        take();
                    else
                        break;
                }
                if (at(TokenKind::rparen))
                    take();
                else
                    error("syntax error");
            }
            if (at(TokenKind::assign)) {
                take();
                // only arrays take a parenthesized initializer list, a scalar
                // can just start with (a + b) * c
                if (result.index && at(TokenKind::lparen)) {
                    take();
                    while (!at(TokenKind::rparen) && !at(TokenKind::eof)) {
                        result.arguments.push_back(expression());
                        if (at(TokenKind::comma))
                            take();
                        else
                            break;
                    }
                    if (at(TokenKind::rparen))
                        take();
                    else
                        error("syntax error");
                } else
                    result.expr = expression();
            }
            skip_line();
            return result;
        }
        if (word("if")) {
            take();
            result.kind = Statement::Kind::if_;
            if (at(TokenKind::lparen))
                take();
            result.expr = expression();
            if (at(TokenKind::rparen))
                take();
            skip_line();
            result.body = block({ { "else", "" }, { "end", "if" } });
            if (word("else")) {
                take();
                skip_line();
                result.alternate = block({ { "end", "if" } });
            }
            if (word("end") && word("if", 1)) {
                take();
                take();
                skip_line();
            } else
                error("syntax error");
            return result;
        }
        if (word("while")) {
            take();
            result.kind = Statement::Kind::while_;
            if (at(TokenKind::lparen))
                take();
            result.expr = expression();
            if (at(TokenKind::rparen))
                take();
            skip_line();
            result.body = block({ { "end", "while" } });
            if (word("end") && word("while", 1)) {
                take();
                take();
                skip_line();
            } else
                error("syntax error");
            return result;
        }
        if (word("select")) {
            take();
            result.kind = Statement::Kind::select_;
            if (at(TokenKind::lparen))
                take();
            result.expr = expression();
            if (at(TokenKind::rparen))
                take();
            else
                error("syntax error");
            skip_line();
            skip_lines();
            while (word("case")) {
                take();
                result.case_low.push_back(expression());
                if (word("to")) {
                    take();
                    result.case_high.push_back(expression());
                } else
                    result.case_high.push_back(result.case_low.back());
                skip_line();
                result.branches.push_back(
                        block({ { "case", "" }, { "end", "select" } }));
            }
            if (word("end") && word("select", 1)) {
                take();
                take();
                skip_line();
            } else
                error("syntax error");
            return result;
        }
        if (word("exit")) {
            take();
            result.kind = Statement::Kind::exit_;
            skip_line();
            return result;
        }
        if (word("continue")) {
            take();
            result.kind = Statement::Kind::continue_;
            skip_line();
            return result;
        }
        if (at(TokenKind::variable)) {
            result.kind = Statement::Kind::assign;
            result.source_name = take().text;
            result.name = lowercase(result.source_name);
            if (at(TokenKind::lbracket)) {
                take();
                result.index = expression();
                if (at(TokenKind::rbracket))
                    take();
                else
                    error("syntax error");
            }
            if (!at(TokenKind::assign)) {
                error("syntax error");
                return {};
            }
            take();
            result.expr = expression();
            skip_line();
            return result;
        }
        if (word("call")) {
            take();
            result.kind = Statement::Kind::call;
            if (!at(TokenKind::identifier)) {
                error("syntax error");
                return {};
            }
            // fold case like declarations do, otherwise mixed-case names end
            // up at the host as unknown builtins
            result.name = lowercase(take().text);
            skip_line();
            return result;
        }
        if (at(TokenKind::identifier)) {
            result.kind = Statement::Kind::call;
            result.name = canonical_builtin_name(take().text);
            if (result.name == "reset_ksp_timer" && at(TokenKind::lparen)) {
                error("syntax error");
            }
            if (!at(TokenKind::lparen)) {
                if (result.name == "make_persistent" ||
                        result.name == "make_instr_persistent" ||
                        result.name == "read_persistent_var") {
                    error("syntax error");
                }
                skip_line();
                return result;
            }
            take();
            while (!at(TokenKind::rparen) && !at(TokenKind::eof) &&
                    !at(TokenKind::newline)) {
                result.arguments.push_back(expression());
                if (at(TokenKind::comma))
                    take();
                else
                    break;
            }
            if (at(TokenKind::rparen))
                take();
            else
                error("syntax error");
            if (result.name == "make_persistent" ||
                    result.name == "make_instr_persistent") {
                result.kind = Statement::Kind::make_persistent;
                result.persistence_mode = result.name == "make_instr_persistent"
                                                  ? PersistenceMode::instrument
                                                  : PersistenceMode::snapshot;
            } else if (result.name == "read_persistent_var") {
                result.kind = Statement::Kind::read_persistent;
            }
            skip_line();
            return result;
        }
        error("syntax error");
        return {};
    }
    int precedence(TokenKind k) const {
        switch (k) {
            case TokenKind::logical_or:
                return 1;
            case TokenKind::logical_xor:
                return 2;
            case TokenKind::logical_and:
                return 3;
            case TokenKind::equal:
            case TokenKind::not_equal:
            case TokenKind::less:
            case TokenKind::less_equal:
            case TokenKind::greater:
            case TokenKind::greater_equal:
                return 4;
            case TokenKind::bit_or:
            case TokenKind::bit_xor:
            case TokenKind::bit_and:
                return 5;
            case TokenKind::concat:
                return 6;
            case TokenKind::plus:
            case TokenKind::minus:
                return 7;
            case TokenKind::star:
            case TokenKind::slash:
            case TokenKind::mod:
            case TokenKind::shift_left:
            case TokenKind::shift_right:
                return 8;
            default:
                return 0;
        }
    }
    Expression expression(int min = 1) {
        Expression left = primary();
        while (precedence(tokens[std::min(position, tokens.size() - 1)].kind) >=
                min) {
            const auto op_token = take();
            const int op_precedence = precedence(op_token.kind);
            Expression right = expression(op_precedence + 1);
            Expression binary;
            binary.kind = Expression::Kind::binary;
            binary.text = op_token.text;
            binary.location = op_token.location;
            binary.children.push_back(std::move(left));
            binary.children.push_back(std::move(right));
            left = std::move(binary);
        }
        return left;
    }
    Expression primary() {
        const auto token = take();
        Expression result;
        result.location = token.location;
        if (token.kind == TokenKind::minus || token.kind == TokenKind::plus ||
                token.kind == TokenKind::logical_not) {
            result.kind = Expression::Kind::unary;
            result.text = lowercase(token.text);
            result.children.push_back(primary());
            return result;
        }
        if (token.kind == TokenKind::integer) {
            result.value = parse_integer_literal(token.text);
            return result;
        }
        if (token.kind == TokenKind::real) {
            result.value = parse_real_literal(token.text);
            return result;
        }
        if (token.kind == TokenKind::string) {
            result.value = token.text;
            return result;
        }
        if (token.kind == TokenKind::variable) {
            result.kind = Expression::Kind::name;
            result.source_name = token.text;
            result.text = lowercase(token.text);
            if (at(TokenKind::lbracket)) {
                take();
                result.children.push_back(expression());
                if (at(TokenKind::rbracket))
                    take();
                else
                    error("syntax error");
            }
            return result;
        }
        if (token.kind == TokenKind::identifier) {
            result.kind = Expression::Kind::call;
            result.text = token.text;
            if (at(TokenKind::lparen)) {
                take();
                while (!at(TokenKind::rparen) && !at(TokenKind::eof)) {
                    result.children.push_back(expression());
                    if (at(TokenKind::comma))
                        take();
                    else
                        break;
                }
                if (at(TokenKind::rparen))
                    take();
                else
                    error("syntax error");
            }
            return result;
        }
        if (token.kind == TokenKind::lparen) {
            result = expression();
            if (at(TokenKind::rparen))
                take();
            else
                error("syntax error");
            return result;
        }
        error("syntax error");
        return result;
    }
};

} // namespace

ParsedProgram parse_program(const std::vector<Token> &tokens,
        std::vector<Diagnostic> &diagnostics, Host *host) {
    Parser parser(tokens, diagnostics, host);
    auto callbacks = parser.parse();
    return { std::move(callbacks), std::move(parser.ui_callbacks),
        std::move(parser.functions) };
}

} // namespace libksp::detail
