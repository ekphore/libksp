// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "libksp/ksp.h"

namespace libksp {
namespace {

bool is_ascii_digit(char ch) { return ch >= '0' && ch <= '9'; }

bool is_ascii_alpha(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

bool is_ascii_hex_digit(char ch) {
    return is_ascii_digit(ch) || (ch >= 'a' && ch <= 'f') ||
           (ch >= 'A' && ch <= 'F');
}

bool is_identifier_start(char ch) { return is_ascii_alpha(ch) || ch == '_'; }

bool is_identifier_character(char ch) {
    return is_ascii_alpha(ch) || is_ascii_digit(ch) || ch == '_';
}

bool consume_exponent(
        std::string_view source, std::size_t &offset, std::size_t &column) {
    if (offset >= source.size() ||
            (source[offset] != 'e' && source[offset] != 'E'))
        return false;

    std::size_t end = offset + 1;
    if (end < source.size() && (source[end] == '+' || source[end] == '-'))
        ++end;
    const std::size_t first_digit = end;
    while (end < source.size() && is_ascii_digit(source[end]))
        ++end;
    if (end == first_digit)
        return false;

    column += end - offset;
    offset = end;
    return true;
}

struct Operator {
    std::string_view text;
    TokenKind kind;
};

constexpr Operator operators[] = { { ":=", TokenKind::assign },
    { "#", TokenKind::not_equal }, { "<=", TokenKind::less_equal },
    { ">=", TokenKind::greater_equal }, { "=", TokenKind::equal },
    { "<", TokenKind::less }, { ">", TokenKind::greater },
    { "&", TokenKind::concat }, { "+", TokenKind::plus },
    { "-", TokenKind::minus }, { "*", TokenKind::star },
    { "/", TokenKind::slash }, { "(", TokenKind::lparen },
    { ")", TokenKind::rparen }, { "[", TokenKind::lbracket },
    { "]", TokenKind::rbracket }, { ",", TokenKind::comma } };

} // namespace

std::vector<Token> lex(std::string_view source, std::vector<Diagnostic> *) {
    if (const auto nul = source.find('\0'); nul != std::string_view::npos)
        source = source.substr(0, nul);

    std::vector<Token> tokens;
    std::size_t offset = 0;
    std::size_t line = 1;
    std::size_t column = 1;

    const auto location = [&] {
        return SourceLocation{ offset, line, column };
    };
    const auto push = [&](TokenKind kind, std::string text,
                              SourceLocation token_location) {
        tokens.push_back({ kind, std::move(text), token_location });
    };

    while (offset < source.size()) {
        const char ch = source[offset];
        const auto start = location();

        if (ch == ' ' || ch == '\t' || ch == '\r') {
            ++offset;
            ++column;
            continue;
        }
        if (ch == '\n') {
            push(TokenKind::newline, "\n", start);
            ++offset;
            ++line;
            column = 1;
            continue;
        }
        if (ch == '.' && offset + 4 < source.size()) {
            const auto dotted = source.substr(offset, 5);
            TokenKind kind = TokenKind::eof;
            if (dotted == ".and.")
                kind = TokenKind::bit_and;
            else if (dotted == ".xor.")
                kind = TokenKind::bit_xor;
            else if (dotted == ".not.")
                kind = TokenKind::logical_not;
            if (kind != TokenKind::eof) {
                push(kind, std::string(source.substr(offset, 5)), start);
                offset += 5;
                column += 5;
                continue;
            }
        }
        if (ch == '.' && offset + 3 < source.size() &&
                source.substr(offset, 4) == ".or.") {
            push(TokenKind::bit_or, std::string(source.substr(offset, 4)),
                    start);
            offset += 4;
            column += 4;
            continue;
        }
        if (ch == '{') {
            ++offset;
            ++column;
            while (offset < source.size() && source[offset] != '}') {
                if (source[offset++] == '\n') {
                    ++line;
                    column = 1;
                } else {
                    ++column;
                }
            }
            if (offset < source.size()) {
                ++offset;
                ++column;
            }
            continue;
        }
        if (ch == '"') {
            ++offset;
            ++column;
            std::string value;
            bool closed = false;
            while (offset < source.size()) {
                const char string_ch = source[offset++];
                if (string_ch == '"') {
                    ++column;
                    closed = true;
                    break;
                }
                if (string_ch == '\\' && offset < source.size() &&
                        source[offset] == '"') {
                    ++column;
                    ++offset;
                    ++column;
                    value += '"';
                } else {
                    if (string_ch == '\n') {
                        ++line;
                        column = 1;
                    } else {
                        ++column;
                    }
                    if (string_ch != '\n')
                        value += string_ch;
                }
            }
            if (!closed)
                continue;
            push(TokenKind::string, std::move(value), start);
            continue;
        }
        if (is_ascii_digit(ch)) {
            const std::size_t begin = offset;
            std::size_t hex_end = offset;
            while (hex_end < source.size() &&
                    is_ascii_hex_digit(source[hex_end]))
                ++hex_end;
            if (hex_end < source.size() &&
                    (source[hex_end] == 'h' || source[hex_end] == 'H')) {
                offset = hex_end + 1;
                column += offset - begin;
                push(TokenKind::integer,
                        std::string(source.substr(begin, offset - begin)),
                        start);
                continue;
            }

            bool real = false;
            while (offset < source.size() && is_ascii_digit(source[offset])) {
                ++offset;
                ++column;
            }
            if (offset < source.size() && source[offset] == '.') {
                real = true;
                ++offset;
                ++column;
                while (offset < source.size() &&
                        is_ascii_digit(source[offset])) {
                    ++offset;
                    ++column;
                }
            }
            if (real)
                consume_exponent(source, offset, column);
            push(real ? TokenKind::real : TokenKind::integer,
                    std::string(source.substr(begin, offset - begin)), start);
            continue;
        }
        if (ch == '.' && offset + 1 < source.size() &&
                is_ascii_digit(source[offset + 1])) {
            const std::size_t begin = offset++;
            ++column;
            while (offset < source.size() && is_ascii_digit(source[offset])) {
                ++offset;
                ++column;
            }
            consume_exponent(source, offset, column);
            push(TokenKind::real,
                    std::string(source.substr(begin, offset - begin)), start);
            continue;
        }
        if (ch == '$' || ch == '%' || ch == '~' || ch == '?' || ch == '@' ||
                ch == '!') {
            const std::size_t begin = offset++;
            ++column;
            while (offset < source.size() &&
                    is_identifier_character(source[offset])) {
                ++offset;
                ++column;
            }
            if (offset == begin + 1) {
                push(TokenKind::unknown, std::string(1, ch), start);
                continue;
            }
            push(TokenKind::variable,
                    std::string(source.substr(begin, offset - begin)), start);
            continue;
        }
        if (is_identifier_start(ch)) {
            const std::size_t begin = offset++;
            ++column;
            while (offset < source.size() &&
                    is_identifier_character(source[offset])) {
                ++offset;
                ++column;
            }
            auto text = std::string(source.substr(begin, offset - begin));
            TokenKind kind = TokenKind::identifier;
            if (text == "mod")
                kind = TokenKind::mod;
            else if (text == "and")
                kind = TokenKind::logical_and;
            else if (text == "or")
                kind = TokenKind::logical_or;
            else if (text == "xor")
                kind = TokenKind::logical_xor;
            else if (text == "not")
                kind = TokenKind::logical_not;
            push(kind, std::move(text), start);
            continue;
        }

        bool matched_operator = false;
        for (const auto &candidate : operators) {
            if (source.substr(offset, candidate.text.size()) != candidate.text)
                continue;
            push(candidate.kind, std::string(candidate.text), start);
            offset += candidate.text.size();
            column += candidate.text.size();
            matched_operator = true;
            break;
        }
        if (matched_operator)
            continue;

        push(TokenKind::unknown, std::string(1, ch), start);
        ++offset;
        ++column;
    }

    tokens.push_back({ TokenKind::eof, "", { offset, line, column } });
    return tokens;
}

} // namespace libksp
