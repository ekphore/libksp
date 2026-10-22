// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "internal.h"

#include <array>
#include <unordered_set>

namespace libksp::detail {
namespace {

bool has_strict_symbol_host(Host *host) {
    return host && !host->defer_unknown_ui_control_types();
}

enum class ValueType { unknown, integer, real, string };

struct Symbol {
    ValueType type{ ValueType::unknown };
    bool is_array{};
    bool is_constant{};
    bool is_ui_control{};
    bool is_polyphonic{};
    std::string ui_type;
    SourceLocation location;
};

using SymbolTable = std::unordered_map<std::string, Symbol>;

struct DeclarationCounts {
    std::size_t file_selectors{};
    std::unordered_map<std::string, std::size_t> ui_controls;
};

struct UiDeclarationSignature {
    char sigil{};
    std::size_t constructor_arguments{};
    bool array{};
};

std::optional<UiDeclarationSignature> ui_declaration_signature(
        std::string_view type) {
    static const std::unordered_map<std::string_view, UiDeclarationSignature>
            signatures = {
                { "ui_button", { '$', 0, false } },
                { "ui_switch", { '$', 0, false } },
                { "ui_knob", { '$', 3, false } },
                { "ui_value_edit", { '$', 3, false } },
                { "ui_menu", { '$', 0, false } },
                { "ui_table", { '%', 3, true } },
                { "ui_xy", { '?', 0, true } },
                { "ui_label", { '$', 2, false } },
                { "ui_waveform", { '$', 2, false } },
                { "ui_slider", { '$', 2, false } },
                { "ui_text_edit", { '@', 0, false } },
                { "ui_level_meter", { '$', 0, false } },
                { "ui_file_selector", { '$', 0, false } },
                { "ui_mouse_area", { '$', 0, false } },
                { "ui_panel", { '$', 0, false } },
                { "ui_wavetable", { '$', 0, false } },
            };
    const auto signature = signatures.find(type);
    if (signature == signatures.end())
        return std::nullopt;
    return signature->second;
}

ValueType type_from_sigil(std::string_view name) {
    if (name.empty())
        return ValueType::unknown;
    switch (name.front()) {
        case '$':
        case '%':
            return ValueType::integer;
        case '~':
        case '?':
            return ValueType::real;
        case '@':
        case '!':
            return ValueType::string;
        default:
            return ValueType::unknown;
    }
}

bool is_array_sigil(std::string_view name) {
    return !name.empty() &&
           (name.front() == '%' || name.front() == '?' || name.front() == '!');
}

ValueType type_from_value(const Value &value) {
    if (std::holds_alternative<std::int64_t>(value) ||
            std::holds_alternative<std::vector<std::int64_t>>(value))
        return ValueType::integer;
    if (std::holds_alternative<double>(value) ||
            std::holds_alternative<std::vector<double>>(value))
        return ValueType::real;
    if (std::holds_alternative<std::string>(value) ||
            std::holds_alternative<std::vector<std::string>>(value))
        return ValueType::string;
    return ValueType::unknown;
}

bool is_array_value(const Value &value) {
    return std::holds_alternative<std::vector<std::int64_t>>(value) ||
           std::holds_alternative<std::vector<double>>(value) ||
           std::holds_alternative<std::vector<std::string>>(value);
}

std::string_view type_name(ValueType type) {
    switch (type) {
        case ValueType::integer:
            return "integer";
        case ValueType::real:
            return "real";
        case ValueType::string:
            return "string";
        case ValueType::unknown:
            return "unknown";
    }
    return "unknown";
}

void report_type_mismatch(std::vector<Diagnostic> &diagnostics,
        SourceLocation location, ValueType expected, ValueType actual) {
    if (expected == ValueType::unknown || actual == ValueType::unknown ||
            expected == actual || expected == ValueType::string)
        return;
    diagnostics.push_back({ location,
            "expected " + std::string(type_name(expected)) +
                    " expression, got " + std::string(type_name(actual)) });
}

bool declaration_expression_uses_only_constants(const Expression &expression,
        const SymbolTable &symbols,
        const std::unordered_set<std::string> &declared_constants, Host *host) {
    if (expression.kind == Expression::Kind::literal)
        return true;
    if (expression.kind == Expression::Kind::name) {
        if (!expression.children.empty())
            return false;
        if (lookup_builtin_integer_constant(expression.source_name) ||
                lookup_builtin_real_constant(expression.source_name)) {
            return true;
        }
        return declared_constants.contains(expression.text);
    }
    if (expression.kind == Expression::Kind::unary ||
            expression.kind == Expression::Kind::binary) {
        return std::all_of(expression.children.begin(),
                expression.children.end(), [&](const auto &child) {
                    return declaration_expression_uses_only_constants(
                            child, symbols, declared_constants, host);
                });
    }

    const auto name = canonical_builtin_name(expression.text);
    if (name == "get_ui_id" && expression.children.size() == 1) {
        const auto &control = expression.children.front();
        if (control.kind != Expression::Kind::name || !control.children.empty())
            return false;
        const auto symbol = symbols.find(control.text);
        if (symbol != symbols.end())
            return symbol->second.is_ui_control;
        return host && host->defer_unknown_ui_control_types();
    }
    if (name == "num_elements" && expression.children.size() == 1) {
        const auto &array = expression.children.front();
        if (array.kind != Expression::Kind::name || !array.children.empty())
            return false;
        const auto symbol = symbols.find(array.text);
        return symbol != symbols.end() && symbol->second.is_array;
    }

    static const std::unordered_set<std::string> folded_calls = { "abs", "sgn",
        "signbit", "lsb", "msb", "sh_left", "sh_right", "in_range",
        "event_status", "exp", "exp2", "log", "log2", "log10", "sqrt", "cbrt",
        "sin", "cos", "tan", "asin", "acos", "atan", "ceil", "floor", "round",
        "pow", "int", "real", "real_to_int", "int_to_real" };
    if (!folded_calls.contains(name))
        return false;
    return std::all_of(expression.children.begin(), expression.children.end(),
            [&](const auto &child) {
                return declaration_expression_uses_only_constants(
                        child, symbols, declared_constants, host);
            });
}

bool builtin_available_in_callback(int variable_id, int callback_type) {
    if (callback_type == 0)
        return true;
    switch (variable_id) {
        case 0:
        case 55:
            return callback_type != 5;
        case 1:
        case 5:
            return callback_type == 3;
        case 2:
        case 3:
        case 6:
        case 7:
        case 11:
        case 13:
            return callback_type == 1 || callback_type == 2 ||
                   callback_type == 9;
        case 4:
        case 8:
            return callback_type == 1 || callback_type == 2;
        case 9:
            return callback_type == 9;
        case 27:
            return callback_type == 12;
        case 39:
            return callback_type == 1 || callback_type == 2 ||
                   callback_type == 3 || callback_type == 4 ||
                   callback_type == 9 || callback_type == 10 ||
                   callback_type == 11 || callback_type == 16;
        case 40:
        case 41:
        case 42:
            return callback_type == 9;
        case 43:
        case 44:
            return callback_type == 10 || callback_type == 11;
        case 56:
            return callback_type == 4;
        case 57:
        case 58:
        case 59:
        case 60:
        case 61:
        case 62:
        case 63:
            return callback_type == 13;
        case 64:
        case 65:
        case 68:
        case 69:
        case 70:
            return callback_type == 6 || callback_type == 15;
        case 73:
        case 74:
        case 75:
            return callback_type == 16;
        default:
            return true;
    }
}

void validate_engine_call_context(std::string_view name,
        SourceLocation location, int callback_type,
        std::vector<Diagnostic> &diagnostics) {
    const auto canonical_name = canonical_builtin_name(name);
    name = canonical_name;
    if (callback_type == 0)
        return;
    if ((name == "watch_var" || name == "watch_array_idx" ||
                name == "disable_logging") &&
            callback_type != 5) {
        diagnostics.push_back({ location,
                std::string(name) + " can only be used in the init callback" });
    }
    if (name == "reset_engine" && callback_type == 5) {
        diagnostics.push_back({ location,
                "reset_engine cannot be used in the init callback" });
    }
    if (name == "play_note" && callback_type == 5) {
        diagnostics.push_back({ location,
                "play_note() cannot be used in \"on init\" callback!" });
    }
    if ((name == "note_off" || name == "note_off_no_cb") &&
            callback_type == 5) {
        diagnostics.push_back({ location,
                std::string(name) + "() cannot be used in the init callback" });
    }
    if (name == "get_event_ids" && callback_type == 5) {
        diagnostics.push_back({ location,
                "get_event_ids() cannot be used in the init callback" });
    }
    if ((name == "change_vol" || name == "change_pan" ||
                name == "change_tune") &&
            callback_type == 5) {
        diagnostics.push_back({ location,
                std::string(name) +
                        "() cannot be used in \"on init\" callback!" });
    }
    if (name == "ignore_event" && callback_type != 1 && callback_type != 2) {
        diagnostics.push_back({ location,
                "ignore_event() can only be used in \"on note\" and "
                "\"on release\" callbacks!" });
    }
    if ((name == "change_note" || name == "change_velo") &&
            callback_type != 1) {
        diagnostics.push_back({ location,
                std::string(name) +
                        "() can only be used in \"on note\" callback!" });
    }
    if ((name == "allow_group" || name == "disallow_group") &&
            callback_type != 1 && callback_type != 2) {
        diagnostics.push_back({ location,
                std::string(name) +
                        "() can only be used in note and release callbacks" });
    }
    if (name == "dont_use_machine_mode" && callback_type != 1) {
        diagnostics.push_back({ location, "dont_use_machine_mode() can only be "
                                          "used in the note callback" });
    }
    if (name == "ignore_controller" && callback_type != 3 &&
            callback_type != 4 && callback_type != 10 && callback_type != 11 &&
            callback_type != 16) {
        diagnostics.push_back({ location,
                "ignore_controller can only be used in controller, "
                "note_controller, poly_at, rpn and nrpn callbacks" });
    }
    if (name == "ignore_midi" && callback_type != 9) {
        diagnostics.push_back({ location,
                "ignore_midi can only be used in the midi_in callback" });
    }
    if (name == "set_midi" && callback_type == 5) {
        diagnostics.push_back(
                { location, "set_midi() cannot be used in the init callback" });
    }
    if ((name == "redirect_output" || name == "redirect_midi") &&
            callback_type == 5) {
        diagnostics.push_back({ location,
                std::string(name) + "() cannot be used in the init callback" });
    }
    if ((name == "set_ui_height" || name == "set_ui_height_px") &&
            callback_type != 5) {
        diagnostics.push_back({ location,
                std::string(name) + " can only be used in the init callback" });
    }
    if ((name == "load_performance_view" ||
                name == "set_map_editor_event_color") &&
            callback_type != 5) {
        diagnostics.push_back({ location,
                std::string(name) + " can only be used in the init callback" });
    }
    if (name == "set_script_title" && callback_type != 5) {
        diagnostics.push_back({ location,
                "set_script_title() can only be used in \"on init\" callback!" });
    }
    if (name == "set_group_dyn_par_name" && callback_type != 5) {
        diagnostics.push_back({ location,
                "set_group_dyn_par_name can only be used in the init callback" });
    }
    if ((name == "set_snapshot_type" || name == "set_num_user_zones" ||
                name == "mf_set_num_export_areas" ||
                name == "set_ui_width_px") &&
            callback_type != 5) {
        diagnostics.push_back({ location,
                std::string(name) + "() can only be used in \"on init\" "
                                    "callback!" });
    }
    if (name == "find_zone" && callback_type != 5) {
        diagnostics.push_back({ location,
                "find_zone() can only be used in the init callback" });
    }
    if (name == "set_listener" && callback_type != 5) {
        diagnostics.push_back({ location,
                "set_listener() can only be used in the init callback" });
    }
    if ((name == "set_control_help" || name == "get_font_id") &&
            callback_type != 5) {
        diagnostics.push_back({ location,
                std::string(name) + " can only be used in the init callback" });
    }
    if (name == "add_menu_item" && callback_type != 5) {
        diagnostics.push_back({ location,
                "add_menu_item can only be used in the init callback" });
    }
    if (name == "purge_group" && callback_type != 6 && callback_type != 14 &&
            callback_type != 15) {
        diagnostics.push_back({ location,
                "purge_group() can only be used in ui_control, ui_controls, and "
                "persistence_changed callbacks" });
    }
    if ((name == "load_array" || name == "save_array" ||
                name == "load_array_str" || name == "save_array_str") &&
            callback_type != 5 && callback_type != 6 && callback_type != 8 &&
            callback_type != 14 && callback_type != 15) {
        diagnostics.push_back({ location,
                std::string(name) +
                        "() can only be used in \"on init\", \"on "
                        "persistence_changed\", \"on pgs_changed\" and \"on "
                        "ui_control(s)\" callbacks!" });
    }
    if (name == "fs_get_filename" && callback_type != 6 &&
            callback_type != 15) {
        diagnostics.push_back({ location,
                "fs_get_filename() can only be used in ui_control and "
                "ui_controls callbacks" });
    }
    if (name == "fs_navigate" && callback_type != 6 && callback_type != 8 &&
            callback_type != 15) {
        diagnostics.push_back(
                { location, "fs_navigate() can only be used in ui_control, "
                            "pgs_changed, and ui_controls callbacks" });
    }
}

void validate_expression_context(const Expression &expression, Host *host,
        int callback_type, std::vector<Diagnostic> &diagnostics) {
    if (expression.kind == Expression::Kind::name) {
        if (const auto id = lookup_builtin_variable_id(expression.source_name);
                id && !builtin_available_in_callback(*id, callback_type)) {
            diagnostics.push_back({ expression.location,
                    "built-in variable is not available in this callback: " +
                            expression.source_name });
        }
        if (const auto id =
                        lookup_builtin_variable_id(expression.source_name)) {
            const bool multi_script = host && host->is_multi_script();
            if (*id == 45 && !multi_script) {
                diagnostics.push_back({ expression.location,
                        "built-in variable can only be used in a multi script: " +
                                expression.source_name });
            }
            if ((*id == 46 || *id == 47 || *id == 52) && multi_script) {
                diagnostics.push_back({ expression.location,
                        "built-in variable cannot be used in a multi script: " +
                                expression.source_name });
            }
        }
    }
    if (expression.kind == Expression::Kind::call &&
            expression.text == "search" && !expression.children.empty() &&
            expression.children.front().kind == Expression::Kind::name) {
        const auto id = lookup_builtin_variable_id(
                expression.children.front().source_name);
        bool available = true;
        if (id && (*id == 7 || *id == 8)) {
            available = callback_type == 0 || callback_type == 1 ||
                        callback_type == 2;
        } else if (id && *id == 9) {
            available = callback_type == 0 || callback_type == 9;
        }
        if (!available) {
            diagnostics.push_back({ expression.location,
                    "built-in array cannot be searched in this callback: " +
                            expression.children.front().source_name });
        }
    }
    if (expression.kind == Expression::Kind::call) {
        validate_engine_call_context(expression.text, expression.location,
                callback_type, diagnostics);
    }
    for (const auto &child : expression.children)
        validate_expression_context(child, host, callback_type, diagnostics);
}

ValueType infer_expression(const Expression &expression,
        const SymbolTable &symbols, std::vector<Diagnostic> &diagnostics) {
    if (expression.kind == Expression::Kind::literal) {
        if (std::holds_alternative<std::int64_t>(expression.value))
            return ValueType::integer;
        if (std::holds_alternative<double>(expression.value))
            return ValueType::real;
        if (std::holds_alternative<std::string>(expression.value))
            return ValueType::string;
        return ValueType::unknown;
    }

    if (expression.kind == Expression::Kind::name) {
        if (lookup_builtin_integer_constant(expression.source_name)) {
            if (!expression.children.empty()) {
                diagnostics.push_back({ expression.location,
                        "cannot index a built-in constant" });
            }
            return ValueType::integer;
        }
        if (lookup_builtin_real_constant(expression.source_name)) {
            if (!expression.children.empty()) {
                diagnostics.push_back({ expression.location,
                        "cannot index a built-in constant" });
            }
            return ValueType::real;
        }
        const auto symbol = symbols.find(expression.text);
        if (symbol != symbols.end()) {
            const bool indexed = !expression.children.empty();
            if (!symbol->second.is_array && indexed) {
                diagnostics.push_back({ expression.location,
                        "cannot index scalar " + expression.text });
            }
        }
        if (!expression.children.empty()) {
            report_type_mismatch(diagnostics,
                    expression.children.front().location, ValueType::integer,
                    infer_expression(
                            expression.children.front(), symbols, diagnostics));
        }
        return type_from_sigil(expression.text);
    }

    if (expression.kind == Expression::Kind::unary) {
        const auto operand = infer_expression(
                expression.children.front(), symbols, diagnostics);
        const auto operation = lowercase(expression.text);
        if (operation == "not" || operation == ".not.")
            return ValueType::integer;
        return operand;
    }

    if (expression.kind == Expression::Kind::call) {
        std::vector<ValueType> arguments;
        arguments.reserve(expression.children.size());
        for (const auto &argument : expression.children)
            arguments.push_back(
                    infer_expression(argument, symbols, diagnostics));

        const auto call_name = canonical_builtin_name(expression.text);
        static const std::unordered_set<std::string> real_functions = { "exp",
            "exp2", "log", "log2", "log10", "sqrt", "cbrt", "sin", "cos", "tan",
            "asin", "acos", "atan", "ceil", "floor", "pow", "round" };
        static const std::unordered_set<std::string> integer_functions = {
            "array_equal", "by_marks", "by_track", "event_status", "in_range",
            "change_time_with_pitch", "get_event_mark", "get_event_par",
            "get_event_par_arr", "int", "get_engine_par", "get_engine_par_m",
            "get_group_par", "get_purge_state", "get_key_color",
            "get_key_triggerstate", "get_key_type", "get_keyrange_max_note",
            "get_keyrange_min_note", "get_voice_limit", "find_group",
            "get_group_idx", "lsb", "msb", "find_mod", "find_target",
            "get_mod_idx", "get_target_idx", "get_nks_nav_par", "get_zone_par",
            "get_loop_par", "find_zone", "is_zone_empty", "get_zone_status",
            "get_num_zones", "get_zone_id", "ms_to_ticks", "ticks_to_ms",
            "num_elements", "num_groups", "get_sample_length", "num_slices",
            "slice_start", "slice_length", "slice_idx_loop_start",
            "slice_idx_loop_end", "slice_loop_count", "num_slices_zone",
            "zone_slice_start", "zone_slice_length",
            "zone_slice_idx_loop_start", "zone_slice_idx_loop_end",
            "zone_slice_loop_count", "play_note", "purge_group",
            "set_engine_par", "set_sample", "load_ir_sample", "load_midi_file",
            "save_midi_file", "load_array", "save_array", "load_array_str",
            "save_array_str", "get_font_id", "get_control_par",
            "get_control_par_arr", "get_ui_id", "get_ui_wf_property",
            "mf_get_num_tracks", "mf_get_buffer_size", "mf_get_channel",
            "mf_get_command", "mf_get_byte_one", "mf_get_byte_two",
            "mf_get_event_par", "mf_get_pos", "mf_get_track_idx", "mf_get_id",
            "mf_get_mark", "mf_get_note_length", "mf_get_length",
            "mf_insert_event", "mf_set_export_area", "mf_reset",
            "mf_set_buffer_size", "mf_insert_file", "detect_key",
            "detect_pitch", "detect_loudness", "detect_peak", "detect_rms",
            "detect_tempo", "detect_sample_type", "detect_drum_type",
            "detect_instrument_type", "circle_of_fifths_distance",
            "guiidx_to_slotidx", "slotidx_to_guiidx", "request_value_async",
            "set_value_async", "set_value_async_str", "get_menu_item_value",
            "get_menu_item_visibility", "real_to_int", "search", "random",
            "sh_left", "sh_right", "pgs_key_exists", "pgs_str_key_exists",
            "pgs_get_key_val"
        };
        static const std::unordered_set<std::string> string_functions = {
            "get_engine_par_disp", "get_engine_par_disp_ext",
            "get_engine_par_disp_m", "get_key_name", "get_keyrange_name",
            "get_scale_name", "group_name", "get_folder", "get_nks_nav_name",
            "get_sample", "output_channel_name", "get_control_par_str",
            "pgs_get_str_key_val", "get_control_par_str_arr",
            "get_menu_item_str", "fs_get_filename"
        };
        static const std::unordered_set<std::string> midi_string_functions = {
            "mf_get_last_filename"
        };

        if (real_functions.contains(call_name) || call_name == "real" ||
                call_name == "int_to_real" ||
                call_name == "get_control_par_real_arr")
            return ValueType::real;
        if (integer_functions.contains(call_name))
            return ValueType::integer;
        if (string_functions.contains(call_name) ||
                midi_string_functions.contains(call_name))
            return ValueType::string;
        if ((expression.text == "abs" || expression.text == "sgn" ||
                    expression.text == "signbit") &&
                !arguments.empty())
            return arguments.front();
        return ValueType::unknown;
    }

    const auto left =
            infer_expression(expression.children[0], symbols, diagnostics);
    const auto right =
            infer_expression(expression.children[1], symbols, diagnostics);
    const auto operation = lowercase(expression.text);
    if (operation == "&")
        return ValueType::string;
    if (operation == "=" || operation == "#" || operation == "<" ||
            operation == "<=" || operation == ">" || operation == ">=" ||
            operation == "and" || operation == "or" || operation == "xor")
        return ValueType::integer;
    if (left == right)
        return left;
    return ValueType::unknown;
}

bool is_unindexed_array(const Expression &expression, ValueType required_type,
        const SymbolTable &symbols, Host *host) {
    if (expression.kind != Expression::Kind::name ||
            !expression.children.empty())
        return false;
    if (const auto symbol = symbols.find(expression.text);
            symbol != symbols.end()) {
        return symbol->second.is_array && symbol->second.type == required_type;
    }
    if (has_strict_symbol_host(host)) {
        const auto lookup_name = is_builtin_variable(expression.source_name)
                                         ? expression.source_name
                                         : expression.text;
        if (const auto external = host->resolve_symbol(lookup_name)) {
            return is_array_value(external->value) &&
                   type_from_value(external->value) == required_type;
        }
    }
    if (is_builtin_variable(expression.source_name))
        return false;
    return is_array_sigil(expression.source_name) &&
           type_from_sigil(expression.source_name) == required_type;
}

bool is_unindexed_array_argument(const Expression &expression,
        std::optional<ValueType> required_type, const SymbolTable &symbols,
        Host *host) {
    if (expression.kind != Expression::Kind::name ||
            !expression.children.empty())
        return false;
    if (const auto symbol = symbols.find(expression.text);
            symbol != symbols.end()) {
        return symbol->second.is_array &&
               (!required_type || symbol->second.type == *required_type);
    }
    if (has_strict_symbol_host(host)) {
        const auto lookup_name = is_builtin_variable(expression.source_name)
                                         ? expression.source_name
                                         : expression.text;
        if (const auto external = host->resolve_symbol(lookup_name)) {
            return is_array_value(external->value) &&
                   (!required_type ||
                           type_from_value(external->value) == *required_type);
        }
    }
    return is_array_sigil(expression.source_name) &&
           (!required_type ||
                   type_from_sigil(expression.source_name) == *required_type);
}

bool is_equality_testable_array(
        const Expression &expression, const SymbolTable &symbols, Host *host) {
    if (is_unindexed_array(expression, ValueType::integer, symbols, host))
        return true;
    if (expression.kind != Expression::Kind::name ||
            !expression.children.empty())
        return false;
    const auto id = lookup_builtin_variable_id(expression.source_name);
    if (!id)
        return false;
    static const std::unordered_set<int> allowed_ids = { 0, 1, 7, 8, 9, 10, 12,
        13, 14, 45, 55, 71 };
    return allowed_ids.contains(*id);
}

bool is_mutable_integer_target(
        const Expression &expression, const SymbolTable &symbols, Host *host) {
    if (expression.kind != Expression::Kind::name ||
            expression.children.size() > 1 ||
            is_builtin_variable(expression.source_name))
        return false;
    const bool indexed = !expression.children.empty();
    if (const auto symbol = symbols.find(expression.text);
            symbol != symbols.end()) {
        return symbol->second.type == ValueType::integer &&
               symbol->second.is_array == indexed &&
               !symbol->second.is_constant;
    }
    if (!has_strict_symbol_host(host))
        return type_from_sigil(expression.source_name) == ValueType::integer;
    const auto external = host->resolve_symbol(expression.text);
    if (!external || !external->writable)
        return false;
    return type_from_value(external->value) == ValueType::integer &&
           is_array_value(external->value) == indexed;
}

bool is_mutable_scalar_target(const Expression &expression,
        ValueType required_type, const SymbolTable &symbols, Host *host) {
    if (expression.kind != Expression::Kind::name ||
            !expression.children.empty() ||
            is_builtin_variable(expression.source_name))
        return false;
    if (const auto symbol = symbols.find(expression.text);
            symbol != symbols.end()) {
        return symbol->second.type == required_type &&
               !symbol->second.is_array && !symbol->second.is_constant;
    }
    if (!has_strict_symbol_host(host))
        return type_from_sigil(expression.source_name) == required_type;
    const auto external = host->resolve_symbol(expression.text);
    return external && external->writable &&
           type_from_value(external->value) == required_type &&
           !is_array_value(external->value);
}

bool is_mutable_integer_array(
        const Expression &expression, const SymbolTable &symbols, Host *host) {
    if (expression.kind != Expression::Kind::name ||
            !expression.children.empty() ||
            is_builtin_variable(expression.source_name))
        return false;
    if (const auto symbol = symbols.find(expression.text);
            symbol != symbols.end()) {
        return symbol->second.type == ValueType::integer &&
               symbol->second.is_array && !symbol->second.is_constant;
    }
    if (!has_strict_symbol_host(host))
        return type_from_sigil(expression.source_name) == ValueType::integer;
    const auto external = host->resolve_symbol(expression.text);
    return external && external->writable &&
           std::holds_alternative<std::vector<std::int64_t>>(external->value);
}

bool is_mutable_array(
        const Expression &expression, const SymbolTable &symbols, Host *host) {
    if (expression.kind != Expression::Kind::name ||
            !expression.children.empty() ||
            is_builtin_variable(expression.source_name))
        return false;
    if (const auto symbol = symbols.find(expression.text);
            symbol != symbols.end()) {
        return symbol->second.is_array && !symbol->second.is_constant;
    }
    if (!has_strict_symbol_host(host))
        return is_array_sigil(expression.source_name);
    const auto external = host->resolve_symbol(expression.text);
    return external && external->writable && is_array_value(external->value);
}

bool is_ui_control_argument(const Expression &expression,
        const SymbolTable &symbols, Host *host,
        std::string_view required_type = {}) {
    if (expression.kind != Expression::Kind::name ||
            !expression.children.empty())
        return false;
    if (const auto symbol = symbols.find(expression.text);
            symbol != symbols.end()) {
        if (symbol->second.is_ui_control) {
            return required_type.empty() ||
                   symbol->second.ui_type == required_type;
        }
        if (!host || !host->defer_unknown_ui_control_types())
            return false;
        const auto sigil = expression.source_name.empty()
                                   ? '\0'
                                   : expression.source_name.front();
        return sigil == '$' || sigil == '%' || sigil == '?' || sigil == '@' ||
               sigil == '!' || sigil == '~';
    }
    if (!has_strict_symbol_host(host)) {
        if (expression.source_name.empty() ||
                is_builtin_variable(expression.source_name)) {
            return false;
        }
        const auto sigil = expression.source_name.front();
        return sigil == '$' || sigil == '%' || sigil == '?' || sigil == '@' ||
               sigil == '!' || sigil == '~';
    }
    const auto external = host->resolve_symbol(expression.text);
    return external && external->ui_control &&
           (required_type.empty() || external->ui_type == required_type);
}

bool validate_engine_call_arguments(std::string_view name,
        const std::vector<Expression> &arguments, SourceLocation location,
        const SymbolTable &symbols, Host *host,
        std::vector<Diagnostic> &diagnostics) {
    const auto canonical_name = canonical_builtin_name(name);
    name = canonical_name;
    std::size_t minimum{};
    std::size_t maximum{};
    const auto validate_pgs_key = [&](const Expression &argument) {
        if (argument.kind != Expression::Kind::call ||
                !argument.children.empty()) {
            diagnostics.push_back({ argument.location,
                    std::string(name) +
                            " requires a bare PGS key identifier" });
            return false;
        }
        return true;
    };
    if (name == "watch_var") {
        if (arguments.size() != 1) {
            diagnostics.push_back(
                    { location, "watch_var requires one declared variable" });
            return true;
        }
        const auto &variable = arguments.front();
        const auto symbol = symbols.find(variable.text);
        if (variable.kind != Expression::Kind::name ||
                !variable.children.empty() || symbol == symbols.end() ||
                symbol->second.is_constant) {
            diagnostics.push_back({ variable.location,
                    "watch_var requires a declared user variable" });
        }
        return true;
    }
    if (name == "get_controller_pages" || name == "set_controller_pages") {
        if (arguments.size() != 3) {
            diagnostics.push_back({ location, "syntax error" });
            return true;
        }
        for (const auto &argument : arguments) {
            if (argument.kind != Expression::Kind::name ||
                    !argument.children.empty()) {
                diagnostics.push_back({ argument.location, "syntax error" });
                return true;
            }
        }
        for (const auto &argument : arguments) {
            if (symbols.find(argument.text) == symbols.end()) {
                diagnostics.push_back({ location,
                        "set_controller_pages(): some arrays are not "
                        "declared!" });
                return true;
            }
        }
        constexpr ValueType expected_types[] = { ValueType::string,
            ValueType::integer, ValueType::string };
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            const auto &argument = arguments[index];
            const auto symbol = symbols.find(argument.text);
            if (!symbol->second.is_array ||
                    symbol->second.type != expected_types[index]) {
                diagnostics.push_back({ argument.location,
                        "array \"" + argument.text + "\" was not declared" });
                return true;
            }
        }
        return true;
    }
    if (name == "watch_array_idx") {
        if (arguments.size() != 2) {
            diagnostics.push_back({ location,
                    "watch_array_idx requires an array and integer index" });
            return true;
        }
        const auto &variable = arguments.front();
        const auto symbol = symbols.find(variable.text);
        if (variable.kind != Expression::Kind::name ||
                !variable.children.empty() || symbol == symbols.end() ||
                !symbol->second.is_array || symbol->second.is_constant) {
            diagnostics.push_back({ variable.location,
                    "watch_array_idx requires a declared user array" });
        }
        report_type_mismatch(diagnostics, arguments[1].location,
                ValueType::integer,
                infer_expression(arguments[1], symbols, diagnostics));
        return true;
    }
    if (name == "disable_logging") {
        if (arguments.size() != 1) {
            diagnostics.push_back({ location,
                    "disable_logging requires one logging constant" });
            return true;
        }
        const auto &event_type = arguments.front();
        std::optional<std::int64_t> constant;
        if (event_type.kind == Expression::Kind::literal &&
                std::holds_alternative<std::int64_t>(event_type.value)) {
            constant = std::get<std::int64_t>(event_type.value);
        } else if (event_type.kind == Expression::Kind::name) {
            constant = lookup_builtin_integer_constant(event_type.source_name);
        }
        if (!constant || *constant < 0 || *constant > 2) {
            diagnostics.push_back({ event_type.location,
                    "disable_logging requires $NI_LOG_MESSAGE, "
                    "$NI_LOG_WARNING or "
                    "$NI_LOG_WATCHING" });
        }
        return true;
    }
    static const std::unordered_set<std::string> real_detection_functions = {
        "detect_pitch", "detect_loudness", "detect_peak", "detect_rms",
        "detect_tempo"
    };
    static const std::unordered_set<std::string> integer_detection_functions = {
        "detect_key", "detect_sample_type", "detect_drum_type",
        "detect_instrument_type"
    };
    const bool real_detection =
            real_detection_functions.contains(std::string(name));
    if (real_detection ||
            integer_detection_functions.contains(std::string(name))) {
        if (arguments.size() != 2) {
            diagnostics.push_back({ location,
                    std::string(name) +
                            " requires a zone ID and output variable" });
            return true;
        }
        report_type_mismatch(diagnostics, arguments.front().location,
                ValueType::integer,
                infer_expression(arguments.front(), symbols, diagnostics));
        const auto output_type =
                real_detection ? ValueType::real : ValueType::integer;
        if (!is_mutable_scalar_target(
                    arguments[1], output_type, symbols, host)) {
            diagnostics.push_back({ arguments[1].location,
                    std::string(name) + " requires a mutable " +
                            std::string(type_name(output_type)) +
                            " output variable" });
        }
        return true;
    }
    if (name == "request_value_async" || name == "subscribe_async") {
        if (arguments.size() != 3) {
            diagnostics.push_back({ location,
                    std::string(name) + " requires three integer arguments" });
        } else {
            for (const auto &argument : arguments) {
                report_type_mismatch(diagnostics, argument.location,
                        ValueType::integer,
                        infer_expression(argument, symbols, diagnostics));
            }
        }
        return true;
    }
    if (name == "load_patch") {
        if (arguments.size() != 2) {
            diagnostics.push_back({ location,
                    "load_patch requires a path and program slot" });
        } else {
            report_type_mismatch(diagnostics, arguments[0].location,
                    ValueType::string,
                    infer_expression(arguments[0], symbols, diagnostics));
            report_type_mismatch(diagnostics, arguments[1].location,
                    ValueType::integer,
                    infer_expression(arguments[1], symbols, diagnostics));
        }
        return true;
    }
    if (name == "load_next_patch") {
        if (arguments.size() != 2) {
            diagnostics.push_back({ location,
                    "load_next_patch requires two integer arguments" });
        } else {
            for (const auto &argument : arguments) {
                report_type_mismatch(diagnostics, argument.location,
                        ValueType::integer,
                        infer_expression(argument, symbols, diagnostics));
            }
        }
        return true;
    }
    if (name == "unload_slot" || name == "reset_engine") {
        if (arguments.size() != 1) {
            diagnostics.push_back({ location,
                    std::string(name) + " requires one integer argument" });
        } else {
            report_type_mismatch(diagnostics, arguments[0].location,
                    ValueType::integer,
                    infer_expression(arguments[0], symbols, diagnostics));
        }
        return true;
    }
    if (name == "set_value_async" || name == "set_value_async_str") {
        if (arguments.size() != 4) {
            diagnostics.push_back({ location,
                    std::string(name) + " requires four arguments" });
        } else {
            for (std::size_t index = 0; index < 3; ++index) {
                report_type_mismatch(diagnostics, arguments[index].location,
                        ValueType::integer,
                        infer_expression(
                                arguments[index], symbols, diagnostics));
            }
            const auto value_type = name == "set_value_async_str"
                                            ? ValueType::string
                                            : ValueType::integer;
            report_type_mismatch(diagnostics, arguments[3].location, value_type,
                    infer_expression(arguments[3], symbols, diagnostics));
        }
        return true;
    }
    if (name == "pgs_create_key" || name == "pgs_get_key_val" ||
            name == "pgs_key_exists" || name == "pgs_create_str_key" ||
            name == "pgs_get_str_key_val" || name == "pgs_str_key_exists" ||
            name == "pgs_set_key_val" || name == "pgs_set_str_key_val") {
        std::size_t expected = 1;
        if (name == "pgs_create_key" || name == "pgs_get_key_val" ||
                name == "pgs_set_str_key_val")
            expected = 2;
        if (name == "pgs_set_key_val")
            expected = 3;
        if (arguments.size() != expected) {
            diagnostics.push_back({ location,
                    std::string(name) + " has an invalid argument count" });
            return true;
        }
        validate_pgs_key(arguments.front());
        for (std::size_t index = 1; index < arguments.size(); ++index) {
            const auto expected_type = name == "pgs_set_str_key_val"
                                               ? ValueType::string
                                               : ValueType::integer;
            report_type_mismatch(diagnostics, arguments[index].location,
                    expected_type,
                    infer_expression(arguments[index], symbols, diagnostics));
        }
        return true;
    }
    if (name == "find_group" || name == "get_group_idx" ||
            name == "find_zone") {
        if (arguments.size() != 1) {
            diagnostics.push_back({ location,
                    std::string(name) + " requires one string argument" });
        } else {
            report_type_mismatch(diagnostics, arguments.front().location,
                    ValueType::string,
                    infer_expression(arguments.front(), symbols, diagnostics));
        }
        return true;
    }
    if (name == "find_mod" || name == "get_mod_idx" || name == "find_target" ||
            name == "get_target_idx") {
        const auto expected_count =
                name == "find_target" || name == "get_target_idx" ? 3u : 2u;
        if (arguments.size() != expected_count) {
            diagnostics.push_back({ location,
                    std::string(name) + " has an invalid argument count" });
        } else {
            for (std::size_t index = 0; index + 1 < arguments.size(); ++index) {
                report_type_mismatch(diagnostics, arguments[index].location,
                        ValueType::integer,
                        infer_expression(
                                arguments[index], symbols, diagnostics));
            }
            report_type_mismatch(diagnostics, arguments.back().location,
                    ValueType::string,
                    infer_expression(arguments.back(), symbols, diagnostics));
        }
        return true;
    }
    if (name == "set_nks_nav_name") {
        if (arguments.size() != 3) {
            diagnostics.push_back(
                    { location, "set_nks_nav_name requires two integers and a "
                                "string argument" });
        } else {
            for (std::size_t index = 0; index < 2; ++index) {
                report_type_mismatch(diagnostics, arguments[index].location,
                        ValueType::integer,
                        infer_expression(
                                arguments[index], symbols, diagnostics));
            }
            report_type_mismatch(diagnostics, arguments[2].location,
                    ValueType::string,
                    infer_expression(arguments[2], symbols, diagnostics));
        }
        return true;
    }
    if (name == "num_groups" || name == "get_num_zones") {
        if (!arguments.empty()) {
            diagnostics.push_back(
                    { location, std::string(name) + " requires no arguments" });
        }
        return true;
    }
    if (name == "group_name" || name == "get_key_name" ||
            name == "get_keyrange_name" || name == "output_channel_name" ||
            name == "get_folder") {
        if (arguments.size() != 1) {
            diagnostics.push_back({ location,
                    std::string(name) + " requires one integer argument" });
        } else {
            report_type_mismatch(diagnostics, arguments.front().location,
                    ValueType::integer,
                    infer_expression(arguments.front(), symbols, diagnostics));
        }
        return true;
    }
    if (name == "get_sample") {
        if (arguments.empty() || arguments.size() > 2) {
            diagnostics.push_back({ location,
                    "get_sample requires one or two integer arguments" });
        } else {
            for (const auto &argument : arguments) {
                report_type_mismatch(diagnostics, argument.location,
                        ValueType::integer,
                        infer_expression(argument, symbols, diagnostics));
            }
        }
        return true;
    }
    if (name == "set_sample") {
        if (arguments.size() != 2) {
            diagnostics.push_back({ location,
                    "set_sample requires an integer and a string argument" });
        } else {
            report_type_mismatch(diagnostics, arguments[0].location,
                    ValueType::integer,
                    infer_expression(arguments[0], symbols, diagnostics));
            report_type_mismatch(diagnostics, arguments[1].location,
                    ValueType::string,
                    infer_expression(arguments[1], symbols, diagnostics));
        }
        return true;
    }
    if (name == "load_ir_sample" || name == "load_ir_sample_m") {
        const auto expected_count = name == "load_ir_sample" ? 3u : 4u;
        if (arguments.size() != expected_count) {
            diagnostics.push_back({ location,
                    std::string(name) + " has an invalid argument count" });
        } else {
            report_type_mismatch(diagnostics, arguments[0].location,
                    ValueType::string,
                    infer_expression(arguments[0], symbols, diagnostics));
            for (std::size_t index = 1; index < arguments.size(); ++index) {
                report_type_mismatch(diagnostics, arguments[index].location,
                        ValueType::integer,
                        infer_expression(
                                arguments[index], symbols, diagnostics));
            }
        }
        return true;
    }
    if (name == "load_midi_file" || name == "save_midi_file") {
        if (arguments.size() != 1) {
            diagnostics.push_back({ location,
                    std::string(name) + " requires one string argument" });
        } else {
            report_type_mismatch(diagnostics, arguments.front().location,
                    ValueType::string,
                    infer_expression(arguments.front(), symbols, diagnostics));
        }
        return true;
    }
    if (name == "get_font_id") {
        if (arguments.size() != 1) {
            diagnostics.push_back(
                    { location, "get_font_id requires one string argument" });
        } else {
            report_type_mismatch(diagnostics, arguments.front().location,
                    ValueType::string,
                    infer_expression(arguments.front(), symbols, diagnostics));
        }
        return true;
    }
    if (name == "get_scale") {
        if (arguments.size() != 2) {
            diagnostics.push_back({ location,
                    "get_scale requires an integer and an integer array" });
        } else {
            report_type_mismatch(diagnostics, arguments[0].location,
                    ValueType::integer,
                    infer_expression(arguments[0], symbols, diagnostics));
            if (!is_mutable_integer_array(arguments[1], symbols, host)) {
                diagnostics.push_back({ arguments[1].location,
                        "get_scale requires a mutable unindexed integer array" });
            }
        }
        return true;
    }
    if (name == "move_control" || name == "move_control_px" ||
            name == "attach_zone") {
        if (arguments.size() != 3) {
            diagnostics.push_back({ location,
                    std::string(name) + " requires three arguments" });
        } else {
            const auto required_type =
                    name == "attach_zone" ? std::string_view{ "ui_waveform" }
                                          : std::string_view{};
            if (!is_ui_control_argument(
                        arguments[0], symbols, host, required_type)) {
                diagnostics.push_back({ arguments[0].location,
                        std::string(name) + " requires a declared " +
                                (required_type.empty() ? "UI control"
                                                       : "ui_waveform") });
            }
            for (std::size_t index = 1; index < arguments.size(); ++index) {
                report_type_mismatch(diagnostics, arguments[index].location,
                        ValueType::integer,
                        infer_expression(
                                arguments[index], symbols, diagnostics));
            }
        }
        return true;
    }
    if (name == "hide_part") {
        if (arguments.size() != 2) {
            diagnostics.push_back({ location,
                    "hide_part requires a UI control and an integer" });
        } else {
            if (!is_ui_control_argument(arguments[0], symbols, host)) {
                diagnostics.push_back({ arguments[0].location,
                        "hide_part requires a declared UI control" });
            }
            report_type_mismatch(diagnostics, arguments[1].location,
                    ValueType::integer,
                    infer_expression(arguments[1], symbols, diagnostics));
        }
        return true;
    }
    if (name == "set_control_help") {
        if (arguments.size() != 2) {
            diagnostics.push_back({ location,
                    "set_control_help requires a UI control and a string" });
        } else {
            if (!is_ui_control_argument(arguments[0], symbols, host)) {
                diagnostics.push_back({ arguments[0].location,
                        "set_control_help requires a declared UI control" });
            }
            report_type_mismatch(diagnostics, arguments[1].location,
                    ValueType::string,
                    infer_expression(arguments[1], symbols, diagnostics));
        }
        return true;
    }
    if (name == "set_knob_label" || name == "set_knob_unit" ||
            name == "set_table_steps_shown" || name == "set_knob_defval") {
        if (arguments.size() != 2) {
            diagnostics.push_back({ location,
                    std::string(name) + " requires two arguments" });
            return true;
        }

        const auto required_type = name == "set_table_steps_shown"
                                           ? std::string_view{ "ui_table" }
                                           : std::string_view{ "ui_knob" };
        const auto accepts_slider = name == "set_knob_defval";
        const auto is_required_control =
                is_ui_control_argument(
                        arguments[0], symbols, host, required_type) ||
                (accepts_slider && is_ui_control_argument(arguments[0], symbols,
                                           host, "ui_slider"));
        if (!is_required_control) {
            diagnostics.push_back({ arguments[0].location,
                    std::string(name) + " requires a declared " +
                            (name == "set_table_steps_shown" ? "ui_table"
                                    : accepts_slider ? "ui_knob or ui_slider"
                                                     : "ui_knob") });
        }
        report_type_mismatch(diagnostics, arguments[1].location,
                name == "set_knob_label" ? ValueType::string
                                         : ValueType::integer,
                infer_expression(arguments[1], symbols, diagnostics));
        return true;
    }
    if (name == "set_text" || name == "add_text_line") {
        if (arguments.size() != 2) {
            diagnostics.push_back({ location,
                    std::string(name) + " requires two arguments" });
            return true;
        }

        bool valid_control = false;
        if (name == "add_text_line") {
            valid_control = is_ui_control_argument(
                    arguments[0], symbols, host, "ui_label");
        } else {
            static constexpr std::array<std::string_view, 6> accepted_types = {
                "ui_button", "ui_switch", "ui_knob", "ui_value_edit",
                "ui_text_edit", "ui_label"
            };
            valid_control = std::any_of(accepted_types.begin(),
                    accepted_types.end(), [&](std::string_view type) {
                        return is_ui_control_argument(
                                arguments[0], symbols, host, type);
                    });
        }
        if (!valid_control) {
            diagnostics.push_back({ arguments[0].location,
                    std::string(name) +
                            " requires an appropriate UI control" });
        }
        report_type_mismatch(diagnostics, arguments[1].location,
                ValueType::string,
                infer_expression(arguments[1], symbols, diagnostics));
        return true;
    }
    if (name == "set_ui_wf_property" || name == "get_ui_wf_property") {
        const auto expected = name == "set_ui_wf_property" ? 4u : 3u;
        if (arguments.size() != expected) {
            diagnostics.push_back(
                    { location, std::string(name) + " requires " +
                                        (expected == 4 ? "four" : "three") +
                                        " arguments" });
            return true;
        }
        if (!is_ui_control_argument(
                    arguments[0], symbols, host, "ui_waveform")) {
            diagnostics.push_back({ arguments[0].location,
                    std::string(name) + " requires a declared ui_waveform" });
        }
        for (std::size_t index = 1; index < arguments.size(); ++index) {
            report_type_mismatch(diagnostics, arguments[index].location,
                    ValueType::integer,
                    infer_expression(arguments[index], symbols, diagnostics));
        }
        return true;
    }
    if (name == "attach_level_meter") {
        if (arguments.size() != 5) {
            diagnostics.push_back({ location,
                    "attach_level_meter requires five integer arguments" });
        } else {
            for (const auto &argument : arguments) {
                report_type_mismatch(diagnostics, argument.location,
                        ValueType::integer,
                        infer_expression(argument, symbols, diagnostics));
            }
        }
        return true;
    }
    static const std::unordered_set<std::string> midi_zero_argument_queries = {
        "mf_get_num_tracks", "mf_get_buffer_size", "mf_get_channel",
        "mf_get_command", "mf_get_byte_one", "mf_get_byte_two", "mf_get_pos",
        "mf_get_track_idx", "mf_get_id", "mf_get_note_length", "mf_get_length",
        "mf_get_last_filename"
    };
    if (midi_zero_argument_queries.contains(std::string(name))) {
        if (!arguments.empty()) {
            diagnostics.push_back(
                    { location, std::string(name) + " requires no arguments" });
        }
        return true;
    }
    if (name == "mf_reset") {
        if (!arguments.empty()) {
            diagnostics.push_back(
                    { location, "mf_reset requires no arguments" });
        }
        return true;
    }
    if (name == "mf_set_buffer_size" || name == "mf_copy_export_area" ||
            name == "mf_set_num_export_areas") {
        if (arguments.size() != 1) {
            diagnostics.push_back({ location,
                    std::string(name) + " requires one integer argument" });
        } else {
            report_type_mismatch(diagnostics, arguments.front().location,
                    ValueType::integer,
                    infer_expression(arguments.front(), symbols, diagnostics));
        }
        return true;
    }
    if (name == "mf_insert_file") {
        if (arguments.size() != 4) {
            diagnostics.push_back(
                    { location, "mf_insert_file requires four arguments" });
        } else {
            report_type_mismatch(diagnostics, arguments.front().location,
                    ValueType::string,
                    infer_expression(arguments.front(), symbols, diagnostics));
            for (std::size_t index = 1; index < arguments.size(); ++index) {
                report_type_mismatch(diagnostics, arguments[index].location,
                        ValueType::integer,
                        infer_expression(
                                arguments[index], symbols, diagnostics));
            }
        }
        return true;
    }
    if (name == "mf_get_event_par" || name == "mf_get_mark") {
        if (arguments.size() != 2) {
            diagnostics.push_back({ location,
                    std::string(name) + " requires two integer arguments" });
        } else {
            for (const auto &argument : arguments) {
                report_type_mismatch(diagnostics, argument.location,
                        ValueType::integer,
                        infer_expression(argument, symbols, diagnostics));
            }
        }
        return true;
    }
    if (name == "mf_get_next" || name == "mf_get_prev" ||
            name == "mf_get_first" || name == "mf_get_last") {
        if (arguments.size() != 1) {
            diagnostics.push_back({ location,
                    std::string(name) + " requires one integer argument" });
        } else {
            report_type_mismatch(diagnostics, arguments.front().location,
                    ValueType::integer,
                    infer_expression(arguments.front(), symbols, diagnostics));
        }
        return true;
    }
    if (name == "mf_get_next_at" || name == "mf_get_prev_at") {
        if (arguments.size() != 2) {
            diagnostics.push_back({ location,
                    std::string(name) + " requires two integer arguments" });
        } else {
            for (const auto &argument : arguments) {
                report_type_mismatch(diagnostics, argument.location,
                        ValueType::integer,
                        infer_expression(argument, symbols, diagnostics));
            }
        }
        return true;
    }
    if (name == "mf_remove_event") {
        if (arguments.size() != 1) {
            diagnostics.push_back({ location,
                    "mf_remove_event requires one integer argument" });
        } else {
            report_type_mismatch(diagnostics, arguments.front().location,
                    ValueType::integer,
                    infer_expression(arguments.front(), symbols, diagnostics));
        }
        return true;
    }
    static const std::unordered_set<std::string>
            midi_legacy_one_argument_setters = { "mf_set_channel",
                "mf_set_command", "mf_set_byte_one", "mf_set_byte_two",
                "mf_set_note_length", "mf_set_pos" };
    if (midi_legacy_one_argument_setters.contains(std::string(name))) {
        if (arguments.size() != 1) {
            diagnostics.push_back({ location,
                    std::string(name) + " requires one integer argument" });
        } else {
            report_type_mismatch(diagnostics, arguments.front().location,
                    ValueType::integer,
                    infer_expression(arguments.front(), symbols, diagnostics));
        }
        return true;
    }
    if (name == "mf_set_length") {
        if (arguments.size() != 2) {
            diagnostics.push_back({ location,
                    "mf_set_length requires two integer arguments" });
        } else {
            for (const auto &argument : arguments) {
                report_type_mismatch(diagnostics, argument.location,
                        ValueType::integer,
                        infer_expression(argument, symbols, diagnostics));
            }
        }
        return true;
    }
    if (name == "mf_set_event_par" || name == "mf_set_mark") {
        if (arguments.size() != 3) {
            diagnostics.push_back({ location,
                    std::string(name) + " requires three integer arguments" });
        } else {
            for (const auto &argument : arguments) {
                report_type_mismatch(diagnostics, argument.location,
                        ValueType::integer,
                        infer_expression(argument, symbols, diagnostics));
            }
        }
        return true;
    }
    if (name == "mf_insert_event") {
        if (arguments.size() != 5) {
            diagnostics.push_back({ location,
                    "mf_insert_event requires five integer arguments" });
        } else {
            for (const auto &argument : arguments) {
                report_type_mismatch(diagnostics, argument.location,
                        ValueType::integer,
                        infer_expression(argument, symbols, diagnostics));
            }
        }
        return true;
    }
    if (name == "mf_set_export_area") {
        if (arguments.size() != 5) {
            diagnostics.push_back(
                    { location, "mf_set_export_area requires five arguments" });
        } else {
            report_type_mismatch(diagnostics, arguments.front().location,
                    ValueType::string,
                    infer_expression(arguments.front(), symbols, diagnostics));
            for (std::size_t index = 1; index < arguments.size(); ++index) {
                report_type_mismatch(diagnostics, arguments[index].location,
                        ValueType::integer,
                        infer_expression(
                                arguments[index], symbols, diagnostics));
            }
        }
        return true;
    }
    if (name == "set_script_title") {
        if (arguments.size() != 1) {
            diagnostics.push_back({ location,
                    "set_script_title requires one string argument" });
        } else {
            report_type_mismatch(diagnostics, arguments.front().location,
                    ValueType::string,
                    infer_expression(arguments.front(), symbols, diagnostics));
        }
        return true;
    }
    if (name == "set_skin_offset" || name == "set_ui_color" ||
            name == "set_map_editor_event_color") {
        if (arguments.size() != 1) {
            diagnostics.push_back({ location,
                    std::string(name) + " requires one integer argument" });
        } else {
            report_type_mismatch(diagnostics, arguments.front().location,
                    ValueType::integer,
                    infer_expression(arguments.front(), symbols, diagnostics));
        }
        return true;
    }
    if (name == "make_perfview") {
        if (!arguments.empty()) {
            diagnostics.push_back(
                    { location, "make_perfview requires no arguments" });
        }
        return true;
    }
    if (name == "expose_controls" || name == "show_library_tab" ||
            name == "hide_inst_icon" ||
            name == "delay_event_for_loading_slots") {
        if (!arguments.empty()) {
            diagnostics.push_back(
                    { location, std::string(name) + " requires no arguments" });
        }
        return true;
    }
    if (name == "load_performance_view" || name == "load_native_ui") {
        const auto is_literal_string =
                arguments.size() == 1 &&
                arguments.front().kind == Expression::Kind::literal &&
                std::holds_alternative<std::string>(arguments.front().value);
        if (!is_literal_string) {
            diagnostics.push_back({ location,
                    std::string(name) +
                            " requires one string literal argument" });
        }
        return true;
    }
    if (name == "set_group_dyn_par_name") {
        if (arguments.size() != 2) {
            diagnostics.push_back({ location,
                    "set_group_dyn_par_name requires an integer and a "
                    "string argument" });
        } else {
            report_type_mismatch(diagnostics, arguments[0].location,
                    ValueType::integer,
                    infer_expression(arguments[0], symbols, diagnostics));
            report_type_mismatch(diagnostics, arguments[1].location,
                    ValueType::string,
                    infer_expression(arguments[1], symbols, diagnostics));
        }
        return true;
    }
    if (name == "set_control_par" || name == "get_control_par") {
        const auto expected = name == "set_control_par" ? 3u : 2u;
        if (arguments.size() != expected) {
            diagnostics.push_back({ location,
                    std::string(name) + " requires " +
                            (expected == 3 ? "three" : "two") + " arguments" });
        } else {
            for (std::size_t index = 0; index < arguments.size(); ++index) {
                report_type_mismatch(diagnostics, arguments[index].location,
                        ValueType::integer,
                        infer_expression(
                                arguments[index], symbols, diagnostics));
            }
        }
        return true;
    }
    if (name == "set_control_par_str" || name == "get_control_par_str") {
        const auto expected = name == "set_control_par_str" ? 3u : 2u;
        if (arguments.size() != expected) {
            diagnostics.push_back({ location,
                    std::string(name) + " requires " +
                            (expected == 3 ? "three" : "two") + " arguments" });
        } else {
            report_type_mismatch(diagnostics, arguments[0].location,
                    ValueType::integer,
                    infer_expression(arguments[0], symbols, diagnostics));
            report_type_mismatch(diagnostics, arguments[1].location,
                    ValueType::integer,
                    infer_expression(arguments[1], symbols, diagnostics));
            if (name == "set_control_par_str") {
                report_type_mismatch(diagnostics, arguments[2].location,
                        ValueType::string,
                        infer_expression(arguments[2], symbols, diagnostics));
            }
        }
        return true;
    }
    if (name == "set_control_par_arr" || name == "get_control_par_arr" ||
            name == "get_control_par_str_arr" ||
            name == "get_control_par_real_arr") {
        const auto expected = name == "set_control_par_arr" ? 4u : 3u;
        if (arguments.size() != expected) {
            diagnostics.push_back(
                    { location, std::string(name) + " requires " +
                                        (expected == 4 ? "four" : "three") +
                                        " arguments" });
        } else {
            for (const auto &argument : arguments) {
                report_type_mismatch(diagnostics, argument.location,
                        ValueType::integer,
                        infer_expression(argument, symbols, diagnostics));
            }
        }
        return true;
    }
    if (name == "set_control_par_str_arr" ||
            name == "set_control_par_real_arr") {
        if (arguments.size() != 4) {
            diagnostics.push_back({ location,
                    std::string(name) + " requires four arguments" });
        } else {
            report_type_mismatch(diagnostics, arguments[0].location,
                    ValueType::integer,
                    infer_expression(arguments[0], symbols, diagnostics));
            report_type_mismatch(diagnostics, arguments[1].location,
                    ValueType::integer,
                    infer_expression(arguments[1], symbols, diagnostics));
            report_type_mismatch(diagnostics, arguments[2].location,
                    name == "set_control_par_str_arr" ? ValueType::string
                                                      : ValueType::real,
                    infer_expression(arguments[2], symbols, diagnostics));
            report_type_mismatch(diagnostics, arguments[3].location,
                    ValueType::integer,
                    infer_expression(arguments[3], symbols, diagnostics));
        }
        return true;
    }
    if (name == "get_ui_id") {
        if (arguments.size() != 1) {
            diagnostics.push_back(
                    { location, "get_ui_id requires one UI control argument" });
        } else if (!is_ui_control_argument(arguments[0], symbols, host)) {
            diagnostics.push_back({ arguments[0].location,
                    "get_ui_id requires a declared UI control" });
        }
        return true;
    }
    if (name == "add_menu_item") {
        if (arguments.size() != 3) {
            diagnostics.push_back({ location,
                    "add_menu_item requires a ui_menu, string, and integer" });
        } else {
            if (!is_ui_control_argument(
                        arguments[0], symbols, host, "ui_menu")) {
                diagnostics.push_back({ arguments[0].location,
                        "add_menu_item requires a declared ui_menu" });
            }
            report_type_mismatch(diagnostics, arguments[1].location,
                    ValueType::string,
                    infer_expression(arguments[1], symbols, diagnostics));
            report_type_mismatch(diagnostics, arguments[2].location,
                    ValueType::integer,
                    infer_expression(arguments[2], symbols, diagnostics));
        }
        return true;
    }
    if (name == "set_menu_item_value" || name == "set_menu_item_visibility") {
        if (arguments.size() != 3) {
            diagnostics.push_back({ location,
                    std::string(name) + " requires three integer arguments" });
        } else {
            for (const auto &argument : arguments) {
                report_type_mismatch(diagnostics, argument.location,
                        ValueType::integer,
                        infer_expression(argument, symbols, diagnostics));
            }
        }
        return true;
    }
    if (name == "set_menu_item_str") {
        if (arguments.size() != 3) {
            diagnostics.push_back({ location,
                    "set_menu_item_str requires two integers and a string" });
        } else {
            for (std::size_t index = 0; index < 2; ++index) {
                report_type_mismatch(diagnostics, arguments[index].location,
                        ValueType::integer,
                        infer_expression(
                                arguments[index], symbols, diagnostics));
            }
            report_type_mismatch(diagnostics, arguments[2].location,
                    ValueType::string,
                    infer_expression(arguments[2], symbols, diagnostics));
        }
        return true;
    }
    if (name == "get_menu_item_value" || name == "get_menu_item_visibility" ||
            name == "get_menu_item_str") {
        if (arguments.size() != 2) {
            diagnostics.push_back({ location,
                    std::string(name) + " requires two integer arguments" });
        } else {
            for (const auto &argument : arguments) {
                report_type_mismatch(diagnostics, argument.location,
                        ValueType::integer,
                        infer_expression(argument, symbols, diagnostics));
            }
        }
        return true;
    }
    if (name == "load_array" || name == "save_array" ||
            name == "load_array_str" || name == "save_array_str") {
        if (arguments.size() != 2) {
            diagnostics.push_back({ location,
                    std::string(name) + " requires two arguments" });
            return true;
        }
        if (!is_mutable_array(arguments[0], symbols, host)) {
            const auto array_name = arguments[0].source_name.empty()
                                            ? arguments[0].text
                                            : arguments[0].source_name;
            diagnostics.push_back({ arguments[0].location,
                    std::string(name) + "(): Array \"" + array_name +
                            "\" was not declared!" });
        }
        const auto string_source = name.ends_with("_str");
        report_type_mismatch(diagnostics, arguments[1].location,
                string_source ? ValueType::string : ValueType::integer,
                infer_expression(arguments[1], symbols, diagnostics));
        return true;
    }
    if (name == "play_note") {
        minimum = maximum = 4;
    } else if (name == "set_listener" || name == "change_listener_par") {
        minimum = maximum = 2;
    } else if (name == "reset_ksp_timer") {
        minimum = maximum = 0;
    } else if (name == "reset_nks_nav") {
        minimum = maximum = 0;
    } else if (name == "set_nks_nav_par") {
        minimum = maximum = 3;
    } else if (name == "get_zone_par") {
        minimum = maximum = 2;
    } else if (name == "get_purge_state") {
        minimum = maximum = 1;
    } else if (name == "purge_group") {
        minimum = maximum = 2;
    } else if (name == "is_zone_empty" || name == "get_zone_status") {
        minimum = maximum = 1;
    } else if (name == "get_zone_id") {
        minimum = maximum = 1;
    } else if (name == "ms_to_ticks" || name == "ticks_to_ms") {
        minimum = maximum = 1;
    } else if (name == "get_sample_length" || name == "num_slices" ||
               name == "num_slices_zone") {
        minimum = maximum = 1;
    } else if (name == "slice_start" || name == "slice_length" ||
               name == "slice_idx_loop_start" || name == "slice_idx_loop_end" ||
               name == "slice_loop_count" || name == "zone_slice_start" ||
               name == "zone_slice_length" ||
               name == "zone_slice_idx_loop_start" ||
               name == "zone_slice_idx_loop_end" ||
               name == "zone_slice_loop_count") {
        minimum = maximum = 2;
    } else if (name == "set_zone_par" || name == "get_loop_par") {
        minimum = maximum = 3;
    } else if (name == "set_loop_par") {
        minimum = maximum = 4;
    } else if (name == "get_engine_par" || name == "get_engine_par_disp" ||
               name == "get_engine_par_m" || name == "get_engine_par_disp_m") {
        minimum = maximum = 4;
    } else if (name == "get_engine_par_disp_ext" || name == "set_engine_par" ||
               name == "set_engine_par_m") {
        minimum = maximum = 5;
    } else if (name == "note_off" || name == "note_off_no_cb") {
        minimum = 1;
        maximum = 2;
    } else if (name == "ignore_event") {
        minimum = maximum = 1;
    } else if (name == "change_note" || name == "change_velo" ||
               name == "change_time_with_pitch" || name == "get_event_mark" ||
               name == "set_event_mark" || name == "delete_event_mark" ||
               name == "get_event_par") {
        minimum = maximum = 2;
    } else if (name == "set_event_par") {
        minimum = maximum = 3;
    } else if (name == "get_event_par_arr") {
        minimum = maximum = 3;
    } else if (name == "set_event_par_arr") {
        minimum = maximum = 4;
    } else if (name == "change_vol" || name == "change_pan" ||
               name == "change_tune") {
        minimum = maximum = 3;
    } else if (name == "set_kill_priority") {
        minimum = maximum = 2;
    } else if (name == "set_controller" || name == "set_poly_at" ||
               name == "set_rpn" || name == "set_nrpn") {
        minimum = maximum = 2;
    } else if (name == "set_note_controller") {
        minimum = maximum = 3;
    } else if (name == "set_midi" || name == "send_midi") {
        minimum = maximum = 4;
    } else if (name == "redirect_output" || name == "redirect_midi") {
        minimum = maximum = 3;
    } else if (name == "cc_delivery_request") {
        minimum = maximum = 1;
    } else if (name == "ignore_controller" || name == "ignore_midi") {
        minimum = maximum = 0;
    } else if (name == "allow_group" || name == "disallow_group" ||
               name == "dont_use_machine_mode" ||
               name == "_reset_rls_trig_counter" ||
               name == "reset_rls_trig_counter" ||
               name == "_will_never_terminate") {
        minimum = maximum = 1;
    } else if (name == "set_ui_height" || name == "set_ui_height_px" ||
               name == "set_ui_width_px") {
        minimum = maximum = 1;
    } else if (name == "set_key_pressed_support") {
        minimum = maximum = 1;
    } else if (name == "set_key_type" || name == "set_key_color" ||
               name == "set_key_pressed") {
        minimum = maximum = 2;
    } else if (name == "remove_keyrange") {
        minimum = maximum = 1;
    } else if (name == "set_touchstrip_par") {
        minimum = maximum = 3;
    } else if (name == "set_snapshot_type" || name == "set_num_user_zones") {
        minimum = maximum = 1;
    } else if (name == "get_key_color" || name == "get_key_triggerstate" ||
               name == "get_key_type") {
        minimum = maximum = 1;
    } else if (name == "get_keyrange_min_note" ||
               name == "get_keyrange_max_note") {
        minimum = maximum = 1;
    } else if (name == "get_voice_limit") {
        minimum = maximum = 1;
    } else if (name == "guiidx_to_slotidx" || name == "slotidx_to_guiidx") {
        minimum = maximum = 1;
    } else if (name == "set_voice_limit") {
        minimum = maximum = 2;
    } else if (name == "fs_get_filename" || name == "fs_navigate") {
        minimum = maximum = 2;
    } else if (name == "get_group_par") {
        minimum = maximum = 2;
    } else if (name == "get_nks_nav_par" || name == "get_nks_nav_name") {
        minimum = maximum = 2;
    } else if (name == "get_scale_name" ||
               name == "circle_of_fifths_distance") {
        minimum = maximum = 2;
    } else if (name == "set_global_scale" || name == "set_global_key") {
        minimum = maximum = 1;
    } else if (name == "fade_in") {
        minimum = 2;
        maximum = 3;
    } else if (name == "fade_out") {
        minimum = 3;
        maximum = 4;
    } else {
        return false;
    }

    if (arguments.size() < minimum || arguments.size() > maximum) {
        std::string count;
        if (minimum == maximum)
            count = std::to_string(minimum);
        else if (minimum == 1 && maximum == 2)
            count = "one or two";
        else if (minimum == 2 && maximum == 3)
            count = "two or three";
        else
            count = "three or four";
        diagnostics.push_back(
                { location, std::string(name) + " requires " + count +
                                    " integer arguments" });
        return true;
    }
    for (const auto &argument : arguments) {
        report_type_mismatch(diagnostics, argument.location, ValueType::integer,
                infer_expression(argument, symbols, diagnostics));
    }
    return true;
}

void validate_special_expression(const Expression &expression,
        const SymbolTable &symbols, Host *host,
        std::vector<Diagnostic> &diagnostics) {
    if (expression.kind == Expression::Kind::call) {
        validate_engine_call_arguments(expression.text, expression.children,
                expression.location, symbols, host, diagnostics);
        const auto canonical_name = canonical_builtin_name(expression.text);
        if (canonical_name == "pgs_create_key" ||
                canonical_name == "pgs_create_str_key" ||
                canonical_name == "pgs_set_key_val" ||
                canonical_name == "pgs_set_str_key_val") {
            diagnostics.push_back({ expression.location,
                    canonical_name + " cannot be used as an expression" });
        }
        if (canonical_name == "set_engine_par_m" ||
                canonical_name == "load_ir_sample_m") {
            diagnostics.push_back({ expression.location,
                    canonical_name + " cannot be used as an expression" });
        }
        if (host && host->is_multi_script() &&
                (canonical_name == "get_key_color" ||
                        canonical_name == "get_key_triggerstate" ||
                        canonical_name == "get_key_type" ||
                        canonical_name == "play_note" ||
                        canonical_name == "get_group_par" ||
                        canonical_name == "get_engine_par" ||
                        canonical_name == "get_engine_par_disp" ||
                        canonical_name == "get_engine_par_disp_ext" ||
                        canonical_name == "set_engine_par" ||
                        canonical_name == "get_nks_nav_par" ||
                        canonical_name == "get_zone_par" ||
                        canonical_name == "get_loop_par" ||
                        canonical_name == "find_zone" ||
                        canonical_name == "is_zone_empty" ||
                        canonical_name == "get_zone_status" ||
                        canonical_name == "get_num_zones" ||
                        canonical_name == "get_zone_id" ||
                        canonical_name == "get_purge_state" ||
                        canonical_name == "purge_group" ||
                        canonical_name == "get_sample" ||
                        canonical_name == "set_sample" ||
                        canonical_name == "load_ir_sample" ||
                        canonical_name == "load_midi_file" ||
                        canonical_name == "get_control_par" ||
                        canonical_name == "get_control_par_str" ||
                        canonical_name == "get_control_par_arr" ||
                        canonical_name == "get_control_par_str_arr" ||
                        canonical_name == "get_control_par_real_arr" ||
                        canonical_name == "get_ui_wf_property" ||
                        canonical_name == "get_menu_item_value" ||
                        canonical_name == "get_menu_item_visibility" ||
                        canonical_name == "get_menu_item_str" ||
                        canonical_name == "fs_get_filename" ||
                        canonical_name == "get_sample_length" ||
                        canonical_name == "get_keyrange_min_note" ||
                        canonical_name == "get_keyrange_max_note" ||
                        canonical_name == "get_voice_limit")) {
            const bool exact_one_parameter_context =
                    canonical_name == "get_key_color" ||
                    canonical_name == "get_key_triggerstate" ||
                    canonical_name == "get_key_type" ||
                    canonical_name == "get_group_par" ||
                    canonical_name == "get_zone_par" ||
                    canonical_name == "get_loop_par";
            diagnostics.push_back({ expression.location,
                    canonical_name +
                            (exact_one_parameter_context
                                            ? "() cannot be used in a multi script!"
                                            : " cannot be used in a multi script") });
        }
        if ((!host || !host->is_multi_script()) &&
                (canonical_name == "get_engine_par_m" ||
                        canonical_name == "get_engine_par_disp_m" ||
                        canonical_name == "guiidx_to_slotidx" ||
                        canonical_name == "slotidx_to_guiidx")) {
            diagnostics.push_back({ expression.location,
                    canonical_name + " can only be used in a multi script" });
        }
    }
    if (expression.kind == Expression::Kind::call &&
            expression.text == "num_elements") {
        if (expression.children.size() != 1 ||
                !is_unindexed_array_argument(expression.children.front(),
                        std::nullopt, symbols, host)) {
            diagnostics.push_back({ expression.location,
                    "num_elements requires one unindexed array argument" });
        }
    }
    if (expression.kind == Expression::Kind::call &&
            expression.text == "search") {
        const auto argument_count = expression.children.size();
        if (argument_count < 2 || argument_count > 4) {
            diagnostics.push_back({ expression.location,
                    "search requires an integer array, a "
                    "value, and optional bounds" });
        } else {
            if (!is_unindexed_array_argument(expression.children.front(),
                        ValueType::integer, symbols, host)) {
                diagnostics.push_back({ expression.children.front().location,
                        "search requires an unindexed integer array" });
            }
            for (std::size_t index = 1; index < argument_count; ++index) {
                report_type_mismatch(diagnostics,
                        expression.children[index].location, ValueType::integer,
                        infer_expression(expression.children[index], symbols,
                                diagnostics));
            }
        }
    }
    static const std::unordered_set<std::string> polymorphic_unary_functions = {
        "abs", "sgn", "signbit"
    };
    if (expression.kind == Expression::Kind::call &&
            polymorphic_unary_functions.contains(expression.text)) {
        if (expression.children.size() != 1) {
            diagnostics.push_back({ expression.location,
                    expression.text + " requires one numeric argument" });
        } else {
            const auto type = infer_expression(
                    expression.children.front(), symbols, diagnostics);
            if (type != ValueType::unknown && type != ValueType::integer &&
                    type != ValueType::real) {
                diagnostics.push_back({ expression.children.front().location,
                        expression.text +
                                " requires an integer or real expression" });
            }
        }
    }
    if (expression.kind == Expression::Kind::call &&
            (expression.text == "lsb" || expression.text == "msb")) {
        if (expression.children.size() != 1) {
            diagnostics.push_back({ expression.location,
                    expression.text + " requires one integer argument" });
        } else {
            report_type_mismatch(diagnostics,
                    expression.children.front().location, ValueType::integer,
                    infer_expression(
                            expression.children.front(), symbols, diagnostics));
        }
    }
    if (expression.kind == Expression::Kind::call &&
            (expression.text == "sh_left" || expression.text == "sh_right")) {
        if (expression.children.size() != 2) {
            diagnostics.push_back({ expression.location,
                    expression.text + " requires two integer arguments" });
        } else {
            for (const auto &argument : expression.children) {
                report_type_mismatch(diagnostics, argument.location,
                        ValueType::integer,
                        infer_expression(argument, symbols, diagnostics));
            }
        }
    }
    if (expression.kind == Expression::Kind::call &&
            (expression.text == "by_marks" || expression.text == "by_track" ||
                    expression.text == "event_status")) {
        if (expression.children.size() != 1) {
            diagnostics.push_back({ expression.location,
                    expression.text + " requires one integer argument" });
        } else {
            report_type_mismatch(diagnostics,
                    expression.children.front().location, ValueType::integer,
                    infer_expression(
                            expression.children.front(), symbols, diagnostics));
        }
    }
    if (expression.kind == Expression::Kind::call &&
            expression.text == "array_equal") {
        if (expression.children.size() != 2 ||
                !is_equality_testable_array(
                        expression.children[0], symbols, host) ||
                !is_equality_testable_array(
                        expression.children[1], symbols, host)) {
            diagnostics.push_back({ expression.location,
                    "array_equal requires two unindexed integer arrays" });
        }
    }
    if (expression.kind == Expression::Kind::call &&
            expression.text == "random") {
        if (expression.children.size() != 2) {
            diagnostics.push_back({ expression.location,
                    "random requires two integer arguments" });
        } else {
            for (const auto &argument : expression.children) {
                report_type_mismatch(diagnostics, argument.location,
                        ValueType::integer,
                        infer_expression(argument, symbols, diagnostics));
            }
        }
    }
    static const std::unordered_set<std::string> real_unary_functions = {
        "acos", "asin", "atan", "cbrt", "ceil", "cos", "exp", "exp2", "floor",
        "log", "log10", "log2", "round", "sin", "sqrt", "tan"
    };
    if (expression.kind == Expression::Kind::call &&
            real_unary_functions.contains(expression.text)) {
        if (expression.children.size() != 1) {
            diagnostics.push_back({ expression.location,
                    expression.text + " requires one real argument" });
        } else {
            report_type_mismatch(diagnostics,
                    expression.children.front().location, ValueType::real,
                    infer_expression(
                            expression.children.front(), symbols, diagnostics));
        }
    }
    if (expression.kind == Expression::Kind::call && expression.text == "pow") {
        if (expression.children.size() != 2) {
            diagnostics.push_back(
                    { expression.location, "pow requires two real arguments" });
        } else {
            for (const auto &argument : expression.children) {
                report_type_mismatch(diagnostics, argument.location,
                        ValueType::real,
                        infer_expression(argument, symbols, diagnostics));
            }
        }
    }
    if (expression.kind == Expression::Kind::call &&
            (expression.text == "real_to_int" || expression.text == "int")) {
        if (expression.children.size() != 1) {
            diagnostics.push_back({ expression.location,
                    expression.text + " requires one real argument" });
        } else {
            report_type_mismatch(diagnostics,
                    expression.children.front().location, ValueType::real,
                    infer_expression(
                            expression.children.front(), symbols, diagnostics));
        }
    }
    if (expression.kind == Expression::Kind::call &&
            (expression.text == "int_to_real" || expression.text == "real")) {
        if (expression.children.size() != 1) {
            diagnostics.push_back({ expression.location,
                    expression.text + " requires one integer argument" });
        } else {
            report_type_mismatch(diagnostics,
                    expression.children.front().location, ValueType::integer,
                    infer_expression(
                            expression.children.front(), symbols, diagnostics));
        }
    }
    if (expression.kind == Expression::Kind::call &&
            expression.text == "in_range") {
        if (expression.children.size() != 3) {
            diagnostics.push_back({ expression.location,
                    "in_range requires three integer or three real arguments" });
        } else {
            const auto expected = infer_expression(
                    expression.children.front(), symbols, diagnostics);
            if (expected == ValueType::unknown) {
                // we can't check the rest until the first argument has a type
            } else if (expected != ValueType::integer &&
                       expected != ValueType::real) {
                diagnostics.push_back({ expression.location,
                        "in_range requires three integer or "
                        "three real arguments" });
            } else {
                for (std::size_t index = 1; index < expression.children.size();
                        ++index) {
                    report_type_mismatch(diagnostics,
                            expression.children[index].location, expected,
                            infer_expression(expression.children[index],
                                    symbols, diagnostics));
                }
            }
        }
    }
    for (const auto &child : expression.children)
        validate_special_expression(child, symbols, host, diagnostics);
}

void collect_declarations(const std::vector<Statement> &statements,
        bool declaration_context, SymbolTable &symbols,
        std::vector<Diagnostic> &diagnostics, DeclarationCounts &counts) {
    for (const auto &statement : statements) {
        if (statement.kind == Statement::Kind::declare) {
            if (!declaration_context) {
                diagnostics.push_back(
                        { statement.location, "variable declarations are only "
                                              "allowed in the init callback" });
            }
            if (is_builtin_variable(statement.source_name)) {
                diagnostics.push_back({ statement.location,
                        "built-in variable cannot be declared: " +
                                statement.source_name });
            }
            if (lookup_builtin_integer_constant(statement.source_name) ||
                    lookup_builtin_real_constant(statement.source_name)) {
                diagnostics.push_back({ statement.location,
                        "built-in constant cannot be declared: " +
                                statement.source_name });
            }
            bool ui_limit_reached = false;
            if (declaration_context && statement.is_ui_control) {
                if (counts.file_selectors >= 4) {
                    diagnostics.push_back({ statement.location,
                            "A maximum of 4 ui_file_selectors can be declared "
                            "per script slot!" });
                    ui_limit_reached = true;
                } else if (counts.ui_controls[statement.ui_type] >= 999) {
                    diagnostics.push_back({ statement.location,
                            "Maximum number of " + statement.ui_type +
                                    " instances was reached (999)!" });
                    ui_limit_reached = true;
                }
            }
            const Symbol symbol{ type_from_sigil(statement.name),
                is_array_sigil(statement.name), statement.is_constant,
                statement.is_ui_control, statement.is_polyphonic,
                statement.ui_type, statement.location };
            const auto [existing, inserted] =
                    symbols.emplace(statement.name, symbol);
            if (!inserted) {
                diagnostics.push_back({ statement.location,
                        "variable already declared: " + statement.name });
            }
            if (declaration_context && statement.is_ui_control && inserted &&
                    !ui_limit_reached) {
                ++counts.ui_controls[statement.ui_type];
                if (statement.ui_type == "ui_file_selector")
                    ++counts.file_selectors;
            }
        }
        collect_declarations(statement.body, declaration_context, symbols,
                diagnostics, counts);
        collect_declarations(statement.alternate, declaration_context, symbols,
                diagnostics, counts);
        for (const auto &branch : statement.branches)
            collect_declarations(
                    branch, declaration_context, symbols, diagnostics, counts);
    }
}

void validate_statements(const std::vector<Statement> &statements,
        const SymbolTable &symbols, Host *host, int callback_type,
        const std::unordered_set<std::string> &functions,
        std::vector<Diagnostic> &diagnostics,
        std::unordered_set<std::string> &declared_constants,
        std::size_t loop_depth = 0) {
    const bool init_context = callback_type == 5;
    for (const auto &statement : statements) {
        const bool array_file_call = statement.name == "load_array" ||
                                     statement.name == "save_array" ||
                                     statement.name == "load_array_str" ||
                                     statement.name == "save_array_str";
        const auto invalid_array_file_expression =
                [&](const Expression &value) {
                    if (value.kind != Expression::Kind::call)
                        return false;
                    const auto &name = value.text;
                    const bool array_file_call = name == "load_array" ||
                                                 name == "save_array" ||
                                                 name == "load_array_str" ||
                                                 name == "save_array_str";
                    return array_file_call && callback_type != 5 &&
                           callback_type != 6 && callback_type != 8 &&
                           callback_type != 14 && callback_type != 15;
                };
        if (statement.index)
            validate_expression_context(
                    *statement.index, host, callback_type, diagnostics);
        if (statement.expr)
            validate_expression_context(
                    *statement.expr, host, callback_type, diagnostics);
        for (const auto &argument : statement.arguments)
            validate_expression_context(
                    argument, host, callback_type, diagnostics);
        for (const auto &argument : statement.ui_arguments)
            validate_expression_context(
                    argument, host, callback_type, diagnostics);
        for (const auto &value : statement.case_low)
            validate_expression_context(
                    value, host, callback_type, diagnostics);
        for (const auto &value : statement.case_high)
            validate_expression_context(
                    value, host, callback_type, diagnostics);
        if (statement.index)
            validate_special_expression(
                    *statement.index, symbols, host, diagnostics);
        if (statement.expr && !invalid_array_file_expression(*statement.expr))
            validate_special_expression(
                    *statement.expr, symbols, host, diagnostics);
        for (const auto &argument : statement.arguments)
            validate_special_expression(argument, symbols, host, diagnostics);
        for (const auto &argument : statement.ui_arguments)
            validate_special_expression(argument, symbols, host, diagnostics);
        if (statement.kind == Statement::Kind::call) {
            const bool invalid_array_file_callback =
                    array_file_call && callback_type != 5 &&
                    callback_type != 6 && callback_type != 8 &&
                    callback_type != 14 && callback_type != 15;
            validate_engine_call_context(statement.name, statement.location,
                    callback_type, diagnostics);
            if (!invalid_array_file_callback) {
                validate_engine_call_arguments(statement.name,
                        statement.arguments, statement.location, symbols, host,
                        diagnostics);
            }
            const bool multi_script = host && host->is_multi_script();
            const auto canonical_statement_name =
                    canonical_builtin_name(statement.name);
            if (!multi_script &&
                    (canonical_statement_name == "load_patch" ||
                            canonical_statement_name == "load_next_patch" ||
                            canonical_statement_name == "unload_slot" ||
                            canonical_statement_name == "reset_engine")) {
                diagnostics.push_back({ statement.location,
                        canonical_statement_name +
                                " can only be used in a multi script" });
            }
            if (!multi_script && statement.name == "set_engine_par_m") {
                diagnostics.push_back({ statement.location,
                        "set_engine_par_m can only be used in a multi script" });
            }
            if (!multi_script && statement.name == "load_ir_sample_m") {
                diagnostics.push_back({ statement.location,
                        "load_ir_sample_m can only be used in a multi script" });
            }
            if (statement.name == "ignore_midi" && !multi_script) {
                diagnostics.push_back({ statement.location,
                        "ignore_midi cannot be used in an instrument script" });
            }
            if (multi_script && (statement.name == "ignore_controller" ||
                                        statement.name == "set_controller" ||
                                        statement.name == "set_poly_at" ||
                                        statement.name == "set_rpn" ||
                                        statement.name == "set_nrpn")) {
                diagnostics.push_back({ statement.location,
                        statement.name + " cannot be used in a multi script" });
            }
            if (multi_script && (statement.name == "note_off" ||
                                        statement.name == "note_off_no_cb" ||
                                        statement.name == "set_midi" ||
                                        statement.name == "fs_navigate")) {
                diagnostics.push_back({ statement.location,
                        statement.name + " cannot be used in a multi script" });
            }
            if (multi_script && statement.name == "play_note") {
                diagnostics.push_back({ statement.location,
                        "play_note() cannot be used in a multi script!" });
            }
            if (multi_script && (statement.name == "change_vol" ||
                                        statement.name == "change_pan" ||
                                        statement.name == "change_tune")) {
                diagnostics.push_back({ statement.location,
                        statement.name +
                                "() cannot be used in a multi script!" });
            }
            if (multi_script && callback_type == 5 &&
                    statement.name == "set_ui_width_px") {
                diagnostics.push_back({ statement.location,
                        "set_ui_width_px() cannot be used in a multi script!" });
            }
            if (multi_script &&
                    (statement.name == "redirect_output" ||
                            statement.name == "cc_delivery_request")) {
                diagnostics.push_back({ statement.location,
                        statement.name + " cannot be used in a multi script" });
            }
            if (!multi_script && statement.name == "redirect_midi") {
                diagnostics.push_back({ statement.location,
                        "redirect_midi can only be used in a multi script" });
            }
            if (multi_script && (statement.name == "get_scale" ||
                                        statement.name == "set_global_scale" ||
                                        statement.name == "set_global_key")) {
                diagnostics.push_back({ statement.location,
                        statement.name + " cannot be used in a multi script" });
            }
            if (multi_script && statement.name == "purge_group") {
                diagnostics.push_back({ statement.location,
                        "purge_group cannot be used in a multi script" });
            }
            if (multi_script && statement.name == "set_key_pressed_support") {
                diagnostics.push_back({ statement.location,
                        "set_key_pressed_support() cannot be "
                        "used in a multi script!" });
            }
            if (multi_script && (statement.name == "set_key_type" ||
                                        statement.name == "set_key_color" ||
                                        statement.name == "set_key_pressed" ||
                                        statement.name == "set_key_name")) {
                diagnostics.push_back({ statement.location,
                        statement.name + " cannot be used in a multi script" });
            }
            if (multi_script && statement.name == "remove_keyrange") {
                diagnostics.push_back({ statement.location,
                        "remove_keyrange() cannot be used in a multi script!" });
            }
            if (multi_script &&
                    (statement.name == "set_keyrange" ||
                            statement.name == "set_sample" ||
                            statement.name == "set_nks_nav_name" ||
                            statement.name == "set_note_controller" ||
                            statement.name == "set_touchstrip_par" ||
                            statement.name == "set_zone_par" ||
                            statement.name == "set_nks_nav_par" ||
                            statement.name == "set_loop_par")) {
                diagnostics.push_back({ statement.location,
                        statement.name +
                                "() cannot be used in a multi script!" });
            }
            if (multi_script && statement.name == "mf_copy_export_area") {
                diagnostics.push_back(
                        { statement.location, "mf_copy_export_area() cannot be "
                                              "used in a multi script!" });
            }
            if (multi_script && callback_type == 5 &&
                    statement.name == "mf_set_num_export_areas") {
                diagnostics.push_back({ statement.location,
                        "mf_set_num_export_areas() cannot be "
                        "used in a multi script!" });
            }
            if (multi_script && statement.name == "reset_nks_nav") {
                diagnostics.push_back({ statement.location,
                        statement.name + " cannot be used in a multi script" });
            }
            if (multi_script && statement.name == "set_voice_limit") {
                diagnostics.push_back({ statement.location,
                        "set_voice_limit() cannot be used in a multi script!" });
            }
            if (multi_script && callback_type == 5 &&
                    (statement.name == "set_snapshot_type" ||
                            statement.name == "set_num_user_zones")) {
                diagnostics.push_back({ statement.location,
                        statement.name +
                                "() cannot be used in a multi script!" });
            }
            if (multi_script &&
                    (statement.name == "set_control_par" ||
                            statement.name == "set_control_par_str" ||
                            statement.name == "set_control_par_arr" ||
                            statement.name == "set_control_par_str_arr" ||
                            statement.name == "set_control_par_real_arr")) {
                diagnostics.push_back({ statement.location,
                        statement.name + " cannot be used in a multi script" });
            }
            if (multi_script &&
                    (statement.name == "add_menu_item" ||
                            statement.name == "set_menu_item_value" ||
                            statement.name == "set_menu_item_visibility" ||
                            statement.name == "set_menu_item_str")) {
                diagnostics.push_back({ statement.location,
                        statement.name + " cannot be used in a multi script" });
            }
            if (multi_script &&
                    (statement.name == "set_knob_label" ||
                            statement.name == "set_knob_unit" ||
                            statement.name == "set_table_steps_shown" ||
                            statement.name == "set_knob_defval")) {
                diagnostics.push_back({ statement.location,
                        statement.name + " cannot be used in a multi script" });
            }
            if (multi_script && (statement.name == "set_text" ||
                                        statement.name == "add_text_line")) {
                diagnostics.push_back({ statement.location,
                        statement.name + " cannot be used in a multi script" });
            }
            if (multi_script && statement.name == "set_ui_color") {
                diagnostics.push_back({ statement.location,
                        "set_ui_color() cannot be used in a multi script!" });
            }
            if (multi_script &&
                    (statement.name == "set_skin_offset" ||
                            statement.name == "set_map_editor_event_color" ||
                            statement.name == "set_ui_wf_property" ||
                            statement.name == "load_performance_view" ||
                            statement.name == "load_native_ui" ||
                            statement.name == "set_group_dyn_par_name")) {
                diagnostics.push_back({ statement.location,
                        statement.name + " cannot be used in a multi script" });
            }
            if (multi_script && canonical_statement_name ==
                                        "delay_event_for_loading_slots") {
                diagnostics.push_back({ statement.location,
                        "delay_event_for_loading_slots "
                        "cannot be used in a multi script" });
            }
        }
        if (statement.kind == Statement::Kind::call &&
                (statement.name == "wait" || statement.name == "wait_ticks" ||
                        statement.name == "wait_async")) {
            if (init_context) {
                diagnostics.push_back({ statement.location,
                        statement.name +
                                "() cannot be used in the init callback" });
            }
            if (statement.arguments.size() != 1) {
                diagnostics.push_back({ statement.location,
                        statement.name + " requires one integer argument" });
            } else {
                report_type_mismatch(diagnostics,
                        statement.arguments.front().location,
                        ValueType::integer,
                        infer_expression(statement.arguments.front(), symbols,
                                diagnostics));
            }
        }
        if (statement.kind == Statement::Kind::call &&
                statement.name == "stop_wait") {
            if (init_context) {
                diagnostics.push_back({ statement.location,
                        "stop_wait() cannot be used in the init callback" });
            }
            if (statement.arguments.size() != 2) {
                diagnostics.push_back({ statement.location,
                        "stop_wait requires two integer arguments" });
            } else {
                for (const auto &argument : statement.arguments) {
                    report_type_mismatch(diagnostics, argument.location,
                            ValueType::integer,
                            infer_expression(argument, symbols, diagnostics));
                }
            }
        }
        if (init_context && statement.kind == Statement::Kind::exit_) {
            diagnostics.push_back({ statement.location,
                    "exit cannot be used in the init callback" });
        }
        if (statement.kind == Statement::Kind::continue_ && loop_depth == 0) {
            diagnostics.push_back({ statement.location,
                    "'continue' statement can only be used "
                    "inside a while loop" });
        }
        if (init_context && statement.kind == Statement::Kind::call &&
                functions.contains(statement.name)) {
            diagnostics.push_back({ statement.location,
                    "function call cannot be used in the init callback" });
        }
        if (statement.kind == Statement::Kind::make_persistent ||
                statement.kind == Statement::Kind::read_persistent) {
            const auto command =
                    statement.kind == Statement::Kind::make_persistent
                            ? "make_persistent"
                            : "read_persistent_var";
            if (!init_context) {
                diagnostics.push_back({ statement.location,
                        std::string(command) +
                                " can only be used in the init callback" });
            }
            if (statement.arguments.size() != 1 ||
                    statement.arguments.front().kind !=
                            Expression::Kind::name ||
                    !statement.arguments.front().children.empty()) {
                diagnostics.push_back({ statement.location,
                        std::string(command) + " requires one variable" });
            } else {
                const auto &target = statement.arguments.front();
                const auto symbol = symbols.find(target.text);
                if (symbol == symbols.end()) {
                    if (is_builtin_variable(target.source_name) ||
                            lookup_builtin_integer_constant(
                                    target.source_name) ||
                            lookup_builtin_real_constant(target.source_name)) {
                        diagnostics.push_back({ statement.location,
                                std::string(command) +
                                        ": built-in variable cannot be persistent: " +
                                        target.source_name });
                    } else if (has_strict_symbol_host(host)) {
                        const auto external = host->resolve_symbol(target.text);
                        if (!external) {
                            diagnostics.push_back({ statement.location,
                                    "make_persistent(): Variable \"" +
                                            target.source_name +
                                            "\" was not declared!" });
                        } else if (!external->writable) {
                            diagnostics.push_back({ statement.location,
                                    "make_persistent(): Constant \"" +
                                            target.source_name +
                                            "\" cannot be made persistent!" });
                        }
                    }
                } else if (statement.kind == Statement::Kind::make_persistent) {
                    if (symbol->second.is_constant) {
                        diagnostics.push_back({ statement.location,
                                "make_persistent(): Constant \"" +
                                        target.source_name +
                                        "\" cannot be made persistent!" });
                    }
                    if (symbol->second.is_polyphonic) {
                        diagnostics.push_back({ statement.location,
                                "make_persistent(): Polyphonic variable \"" +
                                        target.source_name +
                                        "\" cannot be made persistent!" });
                    }
                }
            }
        }
        if (statement.kind == Statement::Kind::declare) {
            const auto expected = type_from_sigil(statement.name);
            if (statement.is_polyphonic &&
                    (expected != ValueType::integer ||
                            is_array_sigil(statement.name))) {
                diagnostics.push_back({ statement.location,
                        "polyphonic declarations require an "
                        "integer scalar variable" });
            }
            if (host && host->is_multi_script() && statement.is_ui_control) {
                diagnostics.push_back({ statement.location,
                        statement.ui_type +
                                " cannot be declared in a multi script" });
            }
            if (statement.is_ui_control) {
                const auto signature =
                        ui_declaration_signature(statement.ui_type);
                if (!signature) {
                    diagnostics.push_back({ statement.location,
                            "unknown UI declaration type: " +
                                    statement.ui_type });
                } else {
                    if (statement.name.empty() ||
                            statement.name.front() != signature->sigil) {
                        diagnostics.push_back({ statement.location,
                                statement.ui_type +
                                        " requires a variable beginning with " +
                                        std::string(1, signature->sigil) });
                    }
                    if (statement.index.has_value() != signature->array) {
                        diagnostics.push_back({ statement.location,
                                statement.ui_type +
                                        (signature->array
                                                        ? " requires an array declaration"
                                                        : " requires a scalar declaration") });
                    }
                    if (statement.ui_arguments.size() !=
                            signature->constructor_arguments) {
                        diagnostics.push_back({ statement.location,
                                statement.ui_type + " requires " +
                                        std::to_string(signature
                                                        ->constructor_arguments) +
                                        " constructor arguments" });
                    }
                    for (const auto &argument : statement.ui_arguments) {
                        report_type_mismatch(diagnostics, argument.location,
                                ValueType::integer,
                                infer_expression(
                                        argument, symbols, diagnostics));
                    }
                }
            }
            if (statement.index) {
                report_type_mismatch(diagnostics, statement.index->location,
                        ValueType::integer,
                        infer_expression(
                                *statement.index, symbols, diagnostics));
            }
            if (statement.expr) {
                report_type_mismatch(diagnostics, statement.expr->location,
                        expected,
                        infer_expression(
                                *statement.expr, symbols, diagnostics));
            }
            for (const auto &value : statement.arguments) {
                report_type_mismatch(diagnostics, value.location, expected,
                        infer_expression(value, symbols, diagnostics));
            }
            if (!statement.is_ui_control) {
                if (statement.index &&
                        !declaration_expression_uses_only_constants(
                                *statement.index, symbols, declared_constants,
                                host)) {
                    diagnostics.push_back({ statement.index->location,
                            "array size expression must be constant" });
                }
                if (statement.expr &&
                        !declaration_expression_uses_only_constants(
                                *statement.expr, symbols, declared_constants,
                                host)) {
                    diagnostics.push_back({ statement.expr->location,
                            "declaration initializer must be constant" });
                }
                for (const auto &value : statement.arguments) {
                    if (!declaration_expression_uses_only_constants(
                                value, symbols, declared_constants, host)) {
                        diagnostics.push_back({ value.location,
                                "declaration initializer must be constant" });
                    }
                }
                for (const auto &argument : statement.ui_arguments)
                    infer_expression(argument, symbols, diagnostics);
                if (statement.is_constant)
                    declared_constants.insert(statement.name);
            }
        } else if (statement.kind == Statement::Kind::assign) {
            const auto symbol = symbols.find(statement.name);
            auto assignment_type = type_from_sigil(statement.name);
            const bool targets_builtin =
                    is_builtin_variable(statement.source_name);
            const bool targets_constant =
                    lookup_builtin_integer_constant(statement.source_name)
                            .has_value() ||
                    lookup_builtin_real_constant(statement.source_name)
                            .has_value();
            if (targets_constant) {
                diagnostics.push_back({ statement.location,
                        "built-in constant cannot be assigned: " +
                                statement.source_name });
            } else if (is_runtime_managed_variable(statement.source_name)) {
                diagnostics.push_back({ statement.location,
                        "runtime-managed variable is read-only: " +
                                statement.source_name });
            } else if (targets_builtin && has_strict_symbol_host(host)) {
                const auto external =
                        host->resolve_symbol(statement.source_name);
                if (external && !external->writable) {
                    diagnostics.push_back({ statement.location,
                            "environment symbol is read-only: " +
                                    statement.source_name });
                }
            } else if (!targets_builtin && symbol != symbols.end()) {
                if (symbol->second.is_constant) {
                    diagnostics.push_back({ statement.location,
                            "constant cannot be assigned: " + statement.name });
                }
                const bool indexed = statement.index.has_value();
                if (symbol->second.is_array != indexed) {
                    diagnostics.push_back({ statement.location,
                            indexed ? "cannot index scalar " + statement.name
                                    : "array requires an index: " +
                                              statement.name });
                }
            } else if (!targets_builtin && has_strict_symbol_host(host)) {
                const auto external = host->resolve_symbol(statement.name);
                if (!external) {
                    diagnostics.push_back({ statement.location,
                            "variable was not declared: " +
                                    statement.source_name });
                } else {
                    if (!external->writable) {
                        diagnostics.push_back({ statement.location,
                                "environment symbol is read-only: " +
                                        statement.source_name });
                    }
                    const bool indexed = statement.index.has_value();
                    if (is_array_value(external->value) != indexed) {
                        diagnostics.push_back({ statement.location,
                                indexed ? "cannot index scalar " +
                                                  statement.source_name
                                        : "array requires an index: " +
                                                  statement.source_name });
                    }
                    if (callback_type == 17)
                        assignment_type = type_from_value(external->value);
                    report_type_mismatch(diagnostics, statement.location,
                            assignment_type, type_from_value(external->value));
                }
            }
            if (statement.index) {
                report_type_mismatch(diagnostics, statement.index->location,
                        ValueType::integer,
                        infer_expression(
                                *statement.index, symbols, diagnostics));
            }
            if (statement.expr) {
                const auto expression_type =
                        infer_expression(*statement.expr, symbols, diagnostics);
                if (callback_type != 17) {
                    report_type_mismatch(diagnostics, statement.expr->location,
                            assignment_type, expression_type);
                }
            }
        } else if (statement.kind == Statement::Kind::if_ ||
                   statement.kind == Statement::Kind::while_) {
            report_type_mismatch(diagnostics, statement.expr->location,
                    ValueType::integer,
                    infer_expression(*statement.expr, symbols, diagnostics));
        } else if (statement.kind == Statement::Kind::select_) {
            const auto selector =
                    infer_expression(*statement.expr, symbols, diagnostics);
            report_type_mismatch(diagnostics, statement.expr->location,
                    ValueType::integer, selector);
            for (const auto &value : statement.case_low)
                report_type_mismatch(diagnostics, value.location, selector,
                        infer_expression(value, symbols, diagnostics));
            for (const auto &value : statement.case_high)
                report_type_mismatch(diagnostics, value.location, selector,
                        infer_expression(value, symbols, diagnostics));
        } else if (statement.kind == Statement::Kind::call) {
            if (statement.name == "sort") {
                const bool valid_count = statement.arguments.size() == 2 ||
                                         statement.arguments.size() == 4;
                const bool valid_array =
                        !statement.arguments.empty() &&
                        (is_unindexed_array(statement.arguments.front(),
                                 ValueType::integer, symbols, host) ||
                                is_unindexed_array(statement.arguments.front(),
                                        ValueType::real, symbols, host));
                if (!valid_count || !valid_array) {
                    diagnostics.push_back({ statement.location,
                            "sort requires an integer or real "
                            "array, a direction, and "
                            "optionally both range bounds" });
                } else {
                    for (std::size_t index = 1;
                            index < statement.arguments.size(); ++index) {
                        report_type_mismatch(diagnostics,
                                statement.arguments[index].location,
                                ValueType::integer,
                                infer_expression(statement.arguments[index],
                                        symbols, diagnostics));
                    }
                }
            }
            if (statement.name == "inc" || statement.name == "dec") {
                if (statement.arguments.size() != 1 ||
                        !is_mutable_integer_target(
                                statement.arguments.front(), symbols, host)) {
                    diagnostics.push_back({ statement.location,
                            statement.name +
                                    " requires one mutable integer target" });
                }
            }
            if ((statement.name == "get_event_ids" ||
                        statement.name == "get_sel_zones_idx" ||
                        statement.name == "get_connected_slots") &&
                    (statement.arguments.size() != 1 ||
                            !is_mutable_integer_array(
                                    statement.arguments.front(), symbols,
                                    host))) {
                diagnostics.push_back({ statement.location,
                        statement.name +
                                " requires one mutable integer array" });
            }
            if (statement.name == "set_key_name") {
                if (statement.arguments.size() != 2) {
                    diagnostics.push_back({ statement.location,
                            "set_key_name requires an integer "
                            "and a string argument" });
                } else {
                    report_type_mismatch(diagnostics,
                            statement.arguments[0].location, ValueType::integer,
                            infer_expression(statement.arguments[0], symbols,
                                    diagnostics));
                    report_type_mismatch(diagnostics,
                            statement.arguments[1].location, ValueType::string,
                            infer_expression(statement.arguments[1], symbols,
                                    diagnostics));
                }
            }
            if (statement.name == "set_keyrange") {
                if (statement.arguments.size() != 3) {
                    diagnostics.push_back({ statement.location,
                            "set_keyrange requires two "
                            "integers and a string argument" });
                } else {
                    for (std::size_t index = 0; index < 2; ++index) {
                        report_type_mismatch(diagnostics,
                                statement.arguments[index].location,
                                ValueType::integer,
                                infer_expression(statement.arguments[index],
                                        symbols, diagnostics));
                    }
                    report_type_mismatch(diagnostics,
                            statement.arguments[2].location, ValueType::string,
                            infer_expression(statement.arguments[2], symbols,
                                    diagnostics));
                }
            }
            if (!array_file_call) {
                for (const auto &argument : statement.arguments)
                    infer_expression(argument, symbols, diagnostics);
            }
        }

        const auto body_loop_depth =
                loop_depth +
                (statement.kind == Statement::Kind::while_ ? 1 : 0);
        validate_statements(statement.body, symbols, host, callback_type,
                functions, diagnostics, declared_constants, body_loop_depth);
        validate_statements(statement.alternate, symbols, host, callback_type,
                functions, diagnostics, declared_constants, loop_depth);
        for (const auto &branch : statement.branches)
            validate_statements(branch, symbols, host, callback_type, functions,
                    diagnostics, declared_constants, loop_depth);
    }
}

} // namespace

void validate_program(const ParsedProgram &program, Host *host,
        std::vector<Diagnostic> &diagnostics) {
    SymbolTable symbols;
    DeclarationCounts declaration_counts;
    std::unordered_set<std::string> functions;
    for (const auto &[name, statements] : program.functions) {
        static_cast<void>(statements);
        functions.insert(name);
    }
    if (const auto init = program.callbacks.find("init");
            init != program.callbacks.end()) {
        collect_declarations(
                init->second, true, symbols, diagnostics, declaration_counts);
    }
    for (const auto &[name, statements] : program.callbacks) {
        if (name != "init")
            collect_declarations(statements, false, symbols, diagnostics,
                    declaration_counts);
    }
    for (const auto &[name, statements] : program.functions) {
        static_cast<void>(name);
        collect_declarations(
                statements, false, symbols, diagnostics, declaration_counts);
    }

    for (const auto &[variable, statements] : program.ui_callbacks) {
        static_cast<void>(statements);
        const auto symbol = symbols.find(variable);
        if (symbol != symbols.end()) {
            if (!symbol->second.is_ui_control) {
                diagnostics.push_back({ {},
                        "ui_control callback variable is not a UI control: " +
                                variable });
            }
            continue;
        }
        const auto external = has_strict_symbol_host(host)
                                      ? host->resolve_symbol(variable)
                                      : std::nullopt;
        if (has_strict_symbol_host(host) && !external) {
            diagnostics.push_back(
                    { {}, "ui_control callback variable was not declared: " +
                                    variable });
        } else if (external && !external->ui_control) {
            diagnostics.push_back(
                    { {}, "ui_control callback variable is not a UI control: " +
                                    variable });
        }
    }

    for (const auto &[name, statements] : program.callbacks) {
        std::unordered_set<std::string> declared_constants;
        validate_statements(statements, symbols, host, callback_type_id(name),
                functions, diagnostics, declared_constants);
    }
    for (const auto &[name, statements] : program.functions) {
        static_cast<void>(name);
        std::unordered_set<std::string> declared_constants;
        validate_statements(statements, symbols, host, 0, functions,
                diagnostics, declared_constants);
    }
    for (const auto &[name, statements] : program.ui_callbacks) {
        static_cast<void>(name);
        std::unordered_set<std::string> declared_constants;
        validate_statements(statements, symbols, host,
                callback_type_id("ui_control"), functions, diagnostics,
                declared_constants);
    }
}

} // namespace libksp::detail
