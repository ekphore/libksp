// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "libksp/ksp.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <unordered_map>

namespace libksp::detail {

struct Expression {
    enum class Kind {
        literal,
        name,
        unary,
        binary,
        call
    } kind{ Kind::literal };
    Value value{ std::int64_t{ 0 } };
    std::string text;
    std::string source_name;
    std::vector<Expression> children;
    SourceLocation location;
};

struct Statement {
    enum class Kind {
        declare,
        assign,
        call,
        if_,
        while_,
        select_,
        exit_,
        continue_,
        make_persistent,
        read_persistent
    } kind{ Kind::call };
    std::string name;
    std::string source_name;
    bool is_constant{};
    bool is_ui_control{};
    std::string ui_type;
    bool is_polyphonic{};
    PersistenceMode persistence_mode{ PersistenceMode::snapshot };
    std::optional<Expression> index;
    std::optional<Expression> expr;
    std::vector<Expression> ui_arguments;
    std::vector<Expression> arguments;
    std::vector<Statement> body;
    std::vector<Statement> alternate;
    std::vector<std::vector<Statement>> branches;
    std::vector<Expression> case_low;
    std::vector<Expression> case_high;
    SourceLocation location;
};

struct ParsedProgram {
    std::unordered_map<std::string, std::vector<Statement>> callbacks;
    std::unordered_map<std::string, std::vector<Statement>> ui_callbacks;
    std::unordered_map<std::string, std::vector<Statement>> functions;
};

ParsedProgram parse_program(const std::vector<Token> &tokens,
        std::vector<Diagnostic> &diagnostics, Host *host);
void validate_program(const ParsedProgram &program, Host *host,
        std::vector<Diagnostic> &diagnostics);
bool is_builtin_variable(std::string_view name);
std::optional<int> lookup_builtin_variable_id(std::string_view name);
bool is_runtime_managed_variable(std::string_view name);
std::optional<std::int64_t> lookup_builtin_integer_constant(
        std::string_view name);
std::optional<double> lookup_builtin_real_constant(std::string_view name);
bool is_valid_engine_parameter_index(std::int64_t index);

inline std::string lowercase(std::string_view text) {
    std::string result(text);
    std::transform(
            result.begin(), result.end(), result.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
    return result;
}

inline std::string canonical_callback_name(std::string_view name) {
    if (name == "_pgs_changed")
        return "pgs_changed";
    return std::string(name);
}

inline bool is_pgs_call_name(std::string_view name) {
    if (name.starts_with("_pgs_"))
        name.remove_prefix(1);
    return name == "pgs_create_key" || name == "pgs_create_str_key" ||
           name == "pgs_key_exists" || name == "pgs_str_key_exists" ||
           name == "pgs_set_key_val" || name == "pgs_set_str_key_val" ||
           name == "pgs_get_key_val" || name == "pgs_get_str_key_val";
}

inline std::string canonical_builtin_name(std::string_view name) {
    if (name.starts_with("_pgs_") && is_pgs_call_name(name))
        name.remove_prefix(1);
    constexpr std::pair<std::string_view, std::string_view> aliases[] = {
        { "_delay_event_for_loading_slots", "delay_event_for_loading_slots" },
        { "_get_engine_par", "get_engine_par" },
        { "_get_engine_par_disp", "get_engine_par_disp" },
        { "_get_engine_par_disp_m", "get_engine_par_disp_m" },
        { "_get_engine_par_m", "get_engine_par_m" },
        { "_get_folder", "get_folder" },
        { "_load_ir_sample", "load_ir_sample" },
        { "_load_ir_sample_m", "load_ir_sample_m" },
        { "_load_patch", "load_patch" },
        { "_num_slices", "num_slices" },
        { "_purge_group", "purge_group" },
        { "_read_persistent_var", "read_persistent_var" },
        { "_redirect_output", "redirect_output" },
        { "_reset_engine", "reset_engine" },
        { "_set_engine_par", "set_engine_par" },
        { "_set_engine_par_m", "set_engine_par_m" },
        { "_set_skin_offset", "set_skin_offset" },
        { "_slice_idx_loop_end", "slice_idx_loop_end" },
        { "_slice_idx_loop_start", "slice_idx_loop_start" },
        { "_slice_length", "slice_length" },
        { "_slice_loop_count", "slice_loop_count" },
        { "_slice_start", "slice_start" },
        { "_unload_slot", "unload_slot" },
        { "get_navigation_par", "get_nks_nav_par" },
    };
    for (const auto &[alias, canonical] : aliases) {
        if (name == alias)
            return std::string(canonical);
    }
    return std::string(name);
}

inline int callback_type_id(std::string_view name) {
    static constexpr std::pair<std::string_view, int> callbacks[] = {
        { "note", 1 },
        { "release", 2 },
        { "controller", 3 },
        { "poly_at", 4 },
        { "init", 5 },
        { "ui_control", 6 },
        { "ui_update", 7 },
        { "pgs_changed", 8 },
        { "midi_in", 9 },
        { "rpn", 10 },
        { "nrpn", 11 },
        { "listener", 12 },
        { "async_complete", 13 },
        { "persistence_changed", 14 },
        { "ui_controls", 15 },
        { "note_controller", 16 },
        { "modulate", 17 },
    };
    for (const auto &[callback, id] : callbacks) {
        if (name == callback)
            return id;
    }
    return 0;
}

inline std::int64_t as_integer(const Value &value) {
    if (const auto integer = std::get_if<std::int64_t>(&value))
        return *integer;
    if (const auto real = std::get_if<double>(&value))
        return static_cast<std::int64_t>(*real);
    if (const auto string = std::get_if<std::string>(&value))
        return std::atoi(string->c_str());
    return 0;
}

inline std::int64_t wrap_integer(std::int64_t value) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(value));
}

inline double as_real(const Value &value) {
    if (const auto real = std::get_if<double>(&value))
        return *real;
    return static_cast<double>(as_integer(value));
}

inline std::string as_string(const Value &value) {
    if (const auto string = std::get_if<std::string>(&value))
        return *string;
    if (const auto real = std::get_if<double>(&value)) {
        char formatted[64]{};
        std::snprintf(formatted, sizeof(formatted), "%.*g", 15, *real);
        return formatted;
    }
    return std::to_string(as_integer(value));
}

inline bool as_boolean(const Value &value) { return as_integer(value) > 0; }

inline bool real_equal(double left, double right) {
    const auto scale = std::min(std::abs(left), std::abs(right));
    return std::abs(left - right) <=
           scale * std::numeric_limits<double>::epsilon();
}

} // namespace libksp::detail
