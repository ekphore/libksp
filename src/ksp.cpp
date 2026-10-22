// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "internal.h"

#include <array>
#include <atomic>
#include <charconv>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace libksp {
namespace {

using Expression = detail::Expression;
using ParsedProgram = detail::ParsedProgram;
using Statement = detail::Statement;
const ParsedProgram empty_program;

#include "scale_data.inc"

std::string lowercase(std::string_view text) { return detail::lowercase(text); }
std::int64_t as_integer(const Value &value) {
    return detail::as_integer(value);
}
std::int64_t wrap_integer(std::int64_t value) {
    return detail::wrap_integer(value);
}
double as_real(const Value &value) { return detail::as_real(value); }
std::string as_string(const Value &value) { return detail::as_string(value); }
bool as_boolean(const Value &value) { return detail::as_boolean(value); }
bool real_equal(double left, double right) {
    return detail::real_equal(left, right);
}

bool is_literal_dot(const Token &token) {
    return token.kind == TokenKind::unknown && token.text == ".";
}

std::vector<Token> normalize_line_continuations(std::vector<Token> tokens) {
    std::vector<Token> normalized;
    normalized.reserve(tokens.size());

    for (std::size_t index = 0; index < tokens.size();) {
        if (index + 3 < tokens.size() && is_literal_dot(tokens[index]) &&
                is_literal_dot(tokens[index + 1]) &&
                is_literal_dot(tokens[index + 2]) &&
                tokens[index + 3].kind == TokenKind::newline) {
            index += 4;
            continue;
        }

        if (index + 3 < tokens.size() &&
                tokens[index].kind == TokenKind::real &&
                tokens[index].text.ends_with('.') &&
                is_literal_dot(tokens[index + 1]) &&
                is_literal_dot(tokens[index + 2]) &&
                tokens[index + 3].kind == TokenKind::newline) {
            tokens[index].kind = TokenKind::integer;
            tokens[index].text.pop_back();
            normalized.push_back(std::move(tokens[index]));
            index += 4;
            continue;
        }

        normalized.push_back(std::move(tokens[index++]));
    }
    return normalized;
}

[[noreturn]] void throw_malformed_persistent_number() {
    throw std::logic_error(
            "Attempted to access the value of an uninitialized optional object.");
}

std::int64_t parse_persistent_integer(std::string_view text) {
    std::int32_t value{};
    const auto result =
            std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw_malformed_persistent_number();
    return value;
}

double parse_persistent_real(std::string_view text) {
    double value{};
    const auto result =
            std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw_malformed_persistent_number();
    return value;
}

std::vector<std::string> split_persistent_array(std::string_view text) {
    std::vector<std::string> values;
    for (std::size_t begin = 0; begin < text.size();) {
        while (begin < text.size() &&
                (text[begin] == ' ' || text[begin] == '\t' ||
                        text[begin] == '\r' || text[begin] == '\n'))
            ++begin;
        if (begin == text.size())
            break;
        auto end = begin;
        while (end < text.size() && text[end] != ' ' && text[end] != '\t' &&
                text[end] != '\r' && text[end] != '\n')
            ++end;
        values.emplace_back(text.substr(begin, end - begin));
        begin = end;
    }
    return values;
}

std::int32_t truncate_float_to_int32(float value) {
    constexpr float minimum = -2147483648.0F;
    constexpr float upper_bound = 2147483648.0F;
    if (std::isnan(value))
        return 0;
    if (value < minimum)
        return std::numeric_limits<std::int32_t>::min();
    if (value >= upper_bound)
        return std::numeric_limits<std::int32_t>::max();
    return static_cast<std::int32_t>(value);
}

std::int64_t circle_of_fifths_distance(
        std::int64_t first, std::int64_t second) {
    if (first < 0)
        return 0;

    std::int64_t first_key = first;
    if (first >= 24)
        first_key -= second < 12 ? 24 : 12;

    const auto find_key = [](std::int64_t key) -> std::optional<std::size_t> {
        for (std::size_t index = 0; index < circle_keys.size(); ++index) {
            const auto mapped_key = circle_keys[index] + (index >= 12 ? 12 : 0);
            if (mapped_key == key)
                return index;
        }
        return std::nullopt;
    };
    const auto first_position = find_key(first_key);
    const auto second_position = find_key(second);
    if (!first_position || !second_position)
        throw std::out_of_range("map::at:  key not found");

    const auto interval = (*second_position - *first_position + 24) % 12;
    return circle_intervals[static_cast<std::size_t>(interval)];
}

std::atomic<std::uint32_t> next_callback_id{};
std::atomic<std::uint64_t> next_wait_id{ 1 };

} // namespace

std::uint32_t reserve_callback_id() {
    return next_callback_id.fetch_add(1, std::memory_order_relaxed);
}
std::uint64_t reserve_wait_id() {
    return next_wait_id.fetch_add(1, std::memory_order_relaxed);
}

std::int32_t random_integer(std::int32_t lower, std::int32_t upper) {
    if (lower > upper)
        std::swap(lower, upper);

    const auto first = static_cast<std::uint32_t>(std::rand());
    const auto second = static_cast<std::uint32_t>(std::rand());
    const auto third = static_cast<std::uint32_t>(std::rand());
    const auto random_value =
            ((second << 15) | (first << 30) | third) & 0x7fffffffU;
    const auto width =
            static_cast<std::int32_t>(static_cast<std::uint32_t>(upper) -
                                      static_cast<std::uint32_t>(lower) + 1U);
    if (width == 0)
        return static_cast<std::int32_t>(
                random_value + static_cast<std::uint32_t>(lower));
    return static_cast<std::int32_t>(
                   random_value % static_cast<std::uint32_t>(width)) +
           lower;
}

struct Runtime::Impl {
    enum class ExecutionResult { completed, suspended, failed };

    struct ExecutionFrame {
        enum class Kind { block, loop } kind{ Kind::block };
        const std::vector<Statement> *statements{};
        const Statement *loop{};
        std::size_t next_statement{};
        bool function_body{};
        std::uint32_t loop_iterations{};
    };

    struct ExecutionState {
        struct ControllerPagesRequest {
            std::array<std::string, 3> variables;
        };

        std::vector<ExecutionFrame> frames;
        std::vector<ControllerPagesRequest> controller_page_requests;
        std::int64_t callback_id{};
        std::int64_t callback_type{};
        std::uint64_t wait_id{};
        std::int64_t wait_duration{};
        WaitUnit wait_unit{ WaitUnit::microseconds };
        std::int64_t purge_async_id{ -1 };
        std::int64_t async_id{};
        std::int64_t async_type{};
        std::int64_t async_parameter_one{};
        std::int64_t async_parameter_two{};
        std::int64_t async_exit_status{};
        Value async_value{ std::int64_t{ 0 } };
        std::optional<std::int64_t> polyphonic_index;
    };

    struct AsyncValueAddress {
        std::int64_t type{};
        std::int64_t parameter_one{};
        std::int64_t parameter_two{};

        bool operator==(const AsyncValueAddress &) const = default;
    };

    Host *host{};
    std::vector<Diagnostic> diagnostics;
    // frames point into this program, so moving state must NOT move it, and
    // load cancels suspended frames before replacing it
    std::shared_ptr<const ParsedProgram> program;
    const ParsedProgram &code() const noexcept {
        return program ? *program : empty_program;
    }
    std::unordered_map<std::string, Value> variables;
    // host callback builtins keep their case, unlike user declarations
    std::unordered_map<std::string, Value> supplied_builtin_values;
    std::unordered_map<std::string, std::int64_t> polyphonic_defaults;
    std::unordered_map<std::string,
            std::unordered_map<std::int64_t, std::int64_t>>
            polyphonic_values;
    std::unordered_map<std::string, PersistenceMode> persistent_variables;
    std::vector<std::string> persistent_variable_order;
    std::vector<std::string> declared_variable_order;
    // lookup folds case, but NKA headers and resource filenames want the
    // spelling the script declared
    std::unordered_map<std::string, std::string> declared_variable_spellings;
    std::vector<PersistentVariable> pending_persistent_values;
    struct PendingArrayLoad {
        std::string target;
        bool value_applied{};
    };
    std::unordered_map<std::int64_t, PendingArrayLoad> pending_array_loads;
    std::unordered_set<std::int64_t> pending_array_saves;
    std::unordered_map<std::int64_t, std::string> pending_detections;
    std::unordered_map<std::int64_t, AsyncValueAddress> pending_async_values;
    std::unordered_set<std::int64_t> pending_engine_commands;
    std::vector<AsyncValueAddress> async_value_subscriptions;
    std::unordered_map<std::string, std::int64_t> script_variable_ids;
    std::unordered_map<std::string, std::int64_t> ui_control_ids;
    std::unordered_map<std::int64_t, std::string> ui_control_names;
    std::unordered_map<std::int64_t, std::string> ui_control_types;
    std::unordered_map<std::string, std::pair<std::int64_t, std::int64_t>>
            ui_integer_ranges;
    std::unordered_map<std::string, std::int64_t> ui_menu_item_counts;
    std::unordered_map<std::string, std::vector<std::int64_t>>
            ui_menu_item_values;
    std::unordered_map<std::string, std::int64_t> ui_menu_selected_indices;
    std::unordered_set<std::int64_t> ui_text_shifting_disabled;
    std::unordered_set<std::string> watched_variables;
    std::unordered_map<std::string, std::unordered_set<std::int64_t>>
            watched_array_indices;
    bool log_messages{ true };
    bool log_warnings{ true };
    bool log_watching{ true };
    std::int64_t next_variable_id{};
    std::int64_t current_callback_id{};
    std::int64_t current_callback_type{};
    std::int64_t current_async_id{};
    std::int64_t current_async_type{};
    std::int64_t current_async_parameter_one{};
    std::int64_t current_async_parameter_two{};
    std::int64_t current_async_exit_status{};
    Value current_async_value{ std::int64_t{ 0 } };
    ExecutionState *active_state{};
    std::unordered_map<std::uint64_t, ExecutionState> suspended;

    bool in_polyphonic_callback() const {
        return active_state && (active_state->callback_type == 1 ||
                                       active_state->callback_type == 2);
    }

    std::int64_t read_polyphonic(std::string_view name) const {
        if (!in_polyphonic_callback() || !active_state->polyphonic_index)
            return 0;
        const auto values = polyphonic_values.find(std::string(name));
        if (values != polyphonic_values.end()) {
            const auto value =
                    values->second.find(*active_state->polyphonic_index);
            if (value != values->second.end())
                return value->second;
        }
        const auto initial = polyphonic_defaults.find(std::string(name));
        return initial == polyphonic_defaults.end() ? 0 : initial->second;
    }

    std::int64_t *write_polyphonic(
            std::string_view name, SourceLocation location) {
        if (!in_polyphonic_callback() || !active_state->polyphonic_index) {
            warn(location,
                    "Polyphonic variables can only be used in \"on note\" and \"on "
                    "release\" callbacks!");
            return nullptr;
        }
        const auto initial = polyphonic_defaults.find(std::string(name));
        auto &values = polyphonic_values[std::string(name)];
        const auto [value, inserted] = values.try_emplace(
                *active_state->polyphonic_index,
                initial == polyphonic_defaults.end() ? 0 : initial->second);
        (void)inserted;
        return &value->second;
    }

    void clone_polyphonic_values(
            std::optional<std::int64_t> source, std::int64_t destination) {
        for (const auto &[name, initial] : polyphonic_defaults) {
            auto &values = polyphonic_values[name];
            auto value = initial;
            if (source) {
                const auto existing = values.find(*source);
                if (existing != values.end())
                    value = existing->second;
            }
            values[destination] = value;
        }
    }

    ~Impl() { clear_suspended(); }

    void clear_suspended() {
        if (host) {
            for (const auto &[wait_id, state] : suspended) {
                (void)state;
                host->cancel_wait(wait_id);
            }
        }
        suspended.clear();
    }

    void store_suspended(ExecutionState state) {
        const auto wait_id = state.wait_id;
        const auto duration = state.wait_duration;
        const auto unit = state.wait_unit;
        suspended.emplace(wait_id, std::move(state));
        if (host)
            host->schedule_wait(wait_id, duration, unit);
    }

    void use_named_ui_control(std::vector<Value> &arguments) const {
        if (arguments.empty())
            return;
        const auto control_name =
                ui_control_names.find(as_integer(arguments[0]));
        if (control_name != ui_control_names.end())
            arguments[0] = control_name->second;
    }

    void refresh_control_value_after_set(
            std::string_view name, const std::vector<Value> &arguments) {
        if (!host || name != "set_control_par" || arguments.size() != 3 ||
                as_integer(arguments[1]) != 12)
            return;
        auto named = arguments;
        use_named_ui_control(named);
        const auto *control = std::get_if<std::string>(&named[0]);
        if (!control)
            return;
        const auto key = lowercase(*control);
        const auto local = variables.find(key);
        if (local == variables.end() || !ui_control_ids.contains(key))
            return;
        // read back what the host accepted, the declared value can be stale
        const auto id = ui_control_ids.at(key);
        const auto &type = ui_control_types.at(id);
        if (type != "ui_button" && type != "ui_switch" && type != "ui_knob" &&
                type != "ui_slider" && type != "ui_value_edit" &&
                type != "ui_menu")
            return;
        const std::array<Value, 2> query{ key, std::int64_t{ 12 } };
        if (const auto live = host->call("get_control_par", query)) {
            auto value = wrap_integer(as_integer(*live));
            if (type == "ui_menu") {
                const auto index = as_integer(value);
                ui_menu_selected_indices[key] = index;
                const auto items = ui_menu_item_values.find(key);
                value = items != ui_menu_item_values.end() && index >= 0 &&
                                        index < static_cast<std::int64_t>(
                                                        items->second.size())
                                ? wrap_integer(items->second[static_cast<
                                          std::size_t>(index)])
                                : std::int64_t{ 0 };
                host->ui_menu_selection_changed(key, index, value);
            } else {
                host->ui_control_value_changed(key, value);
            }
            local->second = value;
            notify_variable_watch(key, std::nullopt, value);
        }
    }

    std::optional<std::string> resolved_ui_control_type(
            std::int64_t control_id) const {
        if (const auto local = ui_control_types.find(control_id);
                local != ui_control_types.end()) {
            return local->second;
        }
        if (host)
            return host->ui_control_type(control_id);
        return std::nullopt;
    }

    bool ui_control_is_declared(std::int64_t control_id) const {
        return (control_id >= 0x4001 && control_id <= 0x4007) ||
               resolved_ui_control_type(control_id).has_value();
    }

    std::optional<std::int64_t> ui_menu_item_count(const Value &control) const {
        std::string control_name;
        if (const auto name = std::get_if<std::string>(&control)) {
            control_name = *name;
        } else {
            const auto known_name = ui_control_names.find(as_integer(control));
            if (known_name == ui_control_names.end())
                return std::nullopt;
            control_name = known_name->second;
        }
        control_name = lowercase(control_name);
        if (const auto count = ui_menu_item_counts.find(control_name);
                count != ui_menu_item_counts.end()) {
            return count->second;
        }
        if (host)
            return host->ui_menu_item_count(control_name);
        return std::nullopt;
    }

    void import_host_menu_entries(const std::string &name) {
        if (!host || ui_menu_item_values.contains(name))
            return;
        const auto count = host->ui_menu_item_count(name);
        if (!count)
            return;
        auto &values = ui_menu_item_values[name];
        for (std::int64_t index = 0; index < *count; ++index) {
            const std::array<Value, 2> arguments{ name, index };
            const auto value = host->call("get_menu_item_value", arguments);
            values.push_back(value ? as_integer(*value) : 0);
        }
        ui_menu_item_counts[name] = static_cast<std::int64_t>(values.size());
    }

    void update_ui_menu_selected_index(
            std::string_view name, std::int64_t logical_value) {
        const auto normalized_name = lowercase(name);
        const auto menu = ui_menu_item_values.find(normalized_name);
        if (menu == ui_menu_item_values.end())
            return;
        const auto item = std::ranges::find(menu->second, logical_value);
        if (item != menu->second.end()) {
            ui_menu_selected_indices[normalized_name] =
                    std::distance(menu->second.begin(), item);
        }
    }

    void refresh_ui_menu_payload(const std::string &name) {
        const auto selected = ui_menu_selected_indices.find(name);
        if (selected == ui_menu_selected_indices.end())
            return;
        const auto items = ui_menu_item_values.find(name);
        const auto index = selected->second;
        Value payload = std::int64_t{ 0 };
        if (items != ui_menu_item_values.end() && index >= 0 &&
                index < static_cast<std::int64_t>(items->second.size()))
            payload = wrap_integer(
                    items->second[static_cast<std::size_t>(index)]);
        if (variables.contains(name))
            variables[name] = payload;
        if (host)
            host->ui_menu_selection_changed(name, index, payload);
    }

    bool has_errors() const {
        return std::any_of(diagnostics.begin(), diagnostics.end(),
                [](const Diagnostic &diagnostic) {
                    return diagnostic.severity == DiagnosticSeverity::error;
                });
    }

    void warn(SourceLocation location, std::string message) {
        if (!log_warnings)
            return;
        diagnostics.push_back(
                { location, std::move(message), DiagnosticSeverity::warning });
    }

    void notify_variable_watch(std::string_view name,
            std::optional<std::int64_t> index, const Value &value) {
        if (!host || !log_watching)
            return;
        const bool watch_whole_variable =
                watched_variables.contains(std::string(name));
        const auto watched_indices =
                watched_array_indices.find(std::string(name));
        const bool watch_index =
                index && watched_indices != watched_array_indices.end() &&
                watched_indices->second.contains(*index);
        if (watch_whole_variable || watch_index)
            host->watched_variable_changed(name, index, value);
    }

    Value read_array_element(
            const Value &array, std::int64_t index, SourceLocation location) {
        const auto valid_index = [&](std::size_t size) {
            return index >= 0 && index < static_cast<std::int64_t>(size);
        };
        if (const auto values =
                        std::get_if<std::vector<std::int64_t>>(&array)) {
            if (valid_index(values->size()))
                return (*values)[static_cast<std::size_t>(index)];
            warn(location, "Array index out of range (using default value 0)!");
            return std::int64_t{ 0 };
        }
        if (const auto values = std::get_if<std::vector<double>>(&array)) {
            if (valid_index(values->size()))
                return (*values)[static_cast<std::size_t>(index)];
            warn(location, "Array index out of range (using default value 0)!");
            return 0.0;
        }
        if (const auto values = std::get_if<std::vector<std::string>>(&array)) {
            if (valid_index(values->size()))
                return (*values)[static_cast<std::size_t>(index)];
            warn(location, "Array index out of range (using default value 0)!");
            return std::string{};
        }
        diagnostics.push_back({ location, "cannot index scalar value" });
        return std::int64_t{ 0 };
    }

    double checked_real_result(double value, SourceLocation location) {
        if (std::isinf(value)) {
            warn(location, "The result of the calculation is " +
                                   as_string(Value{ value }) + "!");
        }
        return value;
    }

    Value checked_value(Value value, SourceLocation location) {
        if (auto real = std::get_if<double>(&value))
            *real = checked_real_result(*real, location);
        return value;
    }

    void collect_persistent_variables(
            const std::vector<Statement> &statements) {
        for (const auto &statement : statements) {
            if (statement.kind == Statement::Kind::make_persistent &&
                    statement.arguments.size() == 1 &&
                    statement.arguments.front().kind ==
                            Expression::Kind::name) {
                const auto &name = statement.arguments.front().text;
                if (!persistent_variables.contains(name))
                    persistent_variable_order.push_back(name);
                persistent_variables[name] = statement.persistence_mode;
            }
            collect_persistent_variables(statement.body);
            collect_persistent_variables(statement.alternate);
            for (const auto &branch : statement.branches)
                collect_persistent_variables(branch);
        }
    }

    void collect_declared_variable_order(
            const std::vector<Statement> &statements) {
        for (const auto &statement : statements) {
            if (statement.kind == Statement::Kind::declare) {
                declared_variable_order.push_back(statement.name);
                declared_variable_spellings.emplace(statement.name,
                        statement.source_name.empty() ? statement.name
                                                      : statement.source_name);
            }
            collect_declared_variable_order(statement.body);
            collect_declared_variable_order(statement.alternate);
            for (const auto &branch : statement.branches)
                collect_declared_variable_order(branch);
        }
    }

    void process_parser_directives(std::vector<Statement> &statements) {
        for (auto statement = statements.begin();
                statement != statements.end();) {
            if (statement->kind == Statement::Kind::call &&
                    statement->name == "make_perfview") {
                if (host)
                    host->make_performance_view();
                statement = statements.erase(statement);
                continue;
            }
            if (statement->kind == Statement::Kind::call &&
                    statement->name == "expose_controls") {
                if (host)
                    host->expose_controls();
                statement = statements.erase(statement);
                continue;
            }
            if (statement->kind == Statement::Kind::call &&
                    statement->name == "show_library_tab") {
                if (host)
                    host->show_library_tab();
                statement = statements.erase(statement);
                continue;
            }
            if (statement->kind == Statement::Kind::call &&
                    statement->name == "hide_inst_icon") {
                if (host)
                    host->hide_instrument_icon();
                statement = statements.erase(statement);
                continue;
            }
            if (statement->kind == Statement::Kind::call &&
                    statement->name == "load_performance_view") {
                if (host && statement->arguments.size() == 1 &&
                        std::holds_alternative<std::string>(
                                statement->arguments.front().value)) {
                    const auto &path = std::get<std::string>(
                            statement->arguments.front().value);
                    if (const auto loaded = host->load_performance_view(path);
                            loaded && !*loaded) {
                        diagnostics.push_back({ statement->location,
                                "load_performance_view could not load the "
                                "requested resource" });
                    }
                }
                statement = statements.erase(statement);
                continue;
            }
            if (statement->kind == Statement::Kind::call &&
                    statement->name == "load_native_ui") {
                // multi scripts don't load native UI
                if (host && !host->is_multi_script() &&
                        statement->arguments.size() == 1 &&
                        std::holds_alternative<std::string>(
                                statement->arguments.front().value)) {
                    host->load_native_ui(std::get<std::string>(
                            statement->arguments.front().value));
                }
                statement = statements.erase(statement);
                continue;
            }
            if (statement->kind == Statement::Kind::call &&
                    detail::canonical_builtin_name(statement->name) ==
                            "delay_event_for_loading_slots") {
                if (host && !host->is_multi_script())
                    host->delay_event_for_loading_slots();
                statement = statements.erase(statement);
                continue;
            }
            if (statement->kind == Statement::Kind::call &&
                    statement->name == "watch_var" &&
                    statement->arguments.size() == 1) {
                if (!host || !host->is_multi_script())
                    watched_variables.insert(statement->arguments.front().text);
                statement = statements.erase(statement);
                continue;
            }
            if (statement->kind == Statement::Kind::call &&
                    statement->name == "watch_array_idx" && host &&
                    host->is_multi_script()) {
                statement = statements.erase(statement);
                continue;
            }
            if (statement->kind == Statement::Kind::call &&
                    statement->name == "disable_logging" &&
                    statement->arguments.size() == 1) {
                if (!host || !host->is_multi_script()) {
                    const auto event_type =
                            as_integer(evaluate(statement->arguments[0]));
                    if (event_type == 0)
                        log_watching = false;
                    else if (event_type == 1)
                        log_messages = false;
                    else if (event_type == 2)
                        log_warnings = false;
                }
                statement = statements.erase(statement);
                continue;
            }

            process_parser_directives(statement->body);
            process_parser_directives(statement->alternate);
            for (auto &branch : statement->branches)
                process_parser_directives(branch);
            ++statement;
        }
    }

    void process_parser_directives(ParsedProgram &program) {
        for (auto &[name, statements] : program.callbacks)
            process_parser_directives(statements);
        for (auto &[name, statements] : program.ui_callbacks)
            process_parser_directives(statements);
        for (auto &[name, statements] : program.functions)
            process_parser_directives(statements);
    }

    Value merge_persistent_value(
            const Value &current, const Value &imported, bool ui_control) {
        if (std::holds_alternative<std::int64_t>(current)) {
            if (const auto text = std::get_if<std::string>(&imported)) {
                return ui_control ? wrap_integer(as_integer(imported))
                                  : parse_persistent_integer(*text);
            }
            return wrap_integer(as_integer(imported));
        }
        if (std::holds_alternative<double>(current)) {
            if (const auto text = std::get_if<std::string>(&imported))
                return parse_persistent_real(*text);
            return as_real(imported);
        }
        if (std::holds_alternative<std::string>(current))
            return as_string(imported);

        if (const auto values =
                        std::get_if<std::vector<std::int64_t>>(&current)) {
            auto merged = *values;
            std::vector<std::int64_t> converted;
            const auto *incoming =
                    std::get_if<std::vector<std::int64_t>>(&imported);
            if (!incoming) {
                std::vector<std::string> serialized;
                const auto *text =
                        std::get_if<std::vector<std::string>>(&imported);
                if (!text) {
                    if (const auto scalar =
                                    std::get_if<std::string>(&imported)) {
                        serialized = split_persistent_array(*scalar);
                        text = &serialized;
                    }
                }
                if (text) {
                    converted.reserve(text->size());
                    for (const auto &element : *text)
                        converted.push_back(wrap_integer(as_integer(element)));
                    incoming = &converted;
                }
            }
            if (!incoming)
                return merged;
            const auto count = std::min(merged.size(), incoming->size());
            std::copy_n(incoming->begin(), count, merged.begin());
            const auto fill =
                    incoming->empty() ? std::int64_t{ 0 } : incoming->back();
            std::fill(merged.begin() + count, merged.end(), fill);
            return merged;
        }
        if (const auto values = std::get_if<std::vector<double>>(&current)) {
            auto merged = *values;
            std::vector<double> converted;
            const auto *incoming = std::get_if<std::vector<double>>(&imported);
            if (!incoming) {
                std::vector<std::string> serialized;
                const auto *text =
                        std::get_if<std::vector<std::string>>(&imported);
                if (!text) {
                    if (const auto scalar =
                                    std::get_if<std::string>(&imported)) {
                        serialized = split_persistent_array(*scalar);
                        text = &serialized;
                    }
                }
                if (text) {
                    converted.reserve(text->size());
                    for (const auto &element : *text)
                        converted.push_back(parse_persistent_real(element));
                    incoming = &converted;
                }
            }
            if (!incoming)
                return merged;
            const auto count = std::min(merged.size(), incoming->size());
            std::copy_n(incoming->begin(), count, merged.begin());
            const auto fill = incoming->empty() ? 0.0 : incoming->back();
            std::fill(merged.begin() + count, merged.end(), fill);
            return merged;
        }
        auto merged = std::get<std::vector<std::string>>(current);
        if (const auto incoming =
                        std::get_if<std::vector<std::string>>(&imported)) {
            const auto count = std::min(merged.size(), incoming->size());
            std::copy_n(incoming->begin(), count, merged.begin());
        }
        return merged;
    }

    void assign_persistent_value(std::string_view name, const Value &value) {
        const auto normalized_name = lowercase(name);
        if (const auto target = variables.find(normalized_name);
                target != variables.end()) {
            auto assigned = merge_persistent_value(target->second, value,
                    ui_control_ids.contains(normalized_name));
            const auto control_id = ui_control_ids.find(normalized_name);
            if (control_id != ui_control_ids.end()) {
                const auto control_type =
                        ui_control_types.find(control_id->second);
                const auto items = ui_menu_item_values.find(normalized_name);
                if (control_type != ui_control_types.end() &&
                        control_type->second == "ui_menu") {
                    const auto index = as_integer(assigned);
                    // keep the index even if add_menu_item hasn't filled the
                    // menu yet, scripts read zero until it does
                    ui_menu_selected_indices[normalized_name] = index;
                    assigned = std::int64_t{ 0 };
                    if (items != ui_menu_item_values.end() && index >= 0 &&
                            index < static_cast<std::int64_t>(
                                            items->second.size())) {
                        assigned = wrap_integer(
                                items->second[static_cast<std::size_t>(index)]);
                    }
                }
            }
            target->second = assigned;
            // we restore an index but callbacks query the payload, so hand it
            // to the host now, init and read_persistent_var both end up here
            if (host && control_id != ui_control_ids.end()) {
                if (const auto selected =
                                ui_menu_selected_indices.find(normalized_name);
                        selected != ui_menu_selected_indices.end())
                    host->ui_menu_selection_changed(
                            normalized_name, selected->second, assigned);
                else
                    host->ui_control_value_changed(normalized_name, assigned);
            }
            return;
        }
        if (!host)
            return;
        const auto symbol = host->resolve_symbol(normalized_name);
        if (!symbol)
            return;
        auto assigned = merge_persistent_value(
                symbol->value, value, symbol->ui_control);
        if (symbol->ui_type == "ui_menu") {
            // resource menus persist an index too, not the payload
            const auto index = as_integer(assigned);
            import_host_menu_entries(normalized_name);
            ui_menu_selected_indices[normalized_name] = index;
            assigned = std::int64_t{ 0 };
            if (const auto items = ui_menu_item_values.find(normalized_name);
                    items != ui_menu_item_values.end()) {
                if (index >= 0 &&
                        index < static_cast<std::int64_t>(items->second.size()))
                    assigned = wrap_integer(
                            items->second[static_cast<std::size_t>(index)]);
            } else {
                const std::array<Value, 2> arguments{ normalized_name, index };
                if (const auto payload =
                                host->call("get_menu_item_value", arguments))
                    assigned = wrap_integer(as_integer(*payload));
            }
            host->ui_menu_selection_changed(normalized_name, index, assigned);
            return;
        }
        if (!host->assign_symbol(normalized_name, assigned)) {
            diagnostics.push_back({ {},
                    "host rejected persistent value for " + normalized_name });
        }
    }

    bool complete_array_load(
            std::int64_t async_id, const Value &value, bool succeeded) {
        const auto pending = pending_array_loads.find(async_id);
        if (pending == pending_array_loads.end())
            return false;
        if (succeeded && !pending->second.value_applied)
            assign_persistent_value(pending->second.target, value);
        pending_array_loads.erase(pending);
        return run_async_complete(async_id, {}, std::int64_t{ 0 }, succeeded);
    }

    bool complete_array_save(std::int64_t async_id, bool succeeded) {
        if (pending_array_saves.erase(async_id) == 0)
            return false;
        return run_async_complete(async_id, {}, std::int64_t{ 0 }, succeeded);
    }

    bool complete_detection(
            std::int64_t async_id, const Value &value, bool succeeded) {
        const auto pending = pending_detections.find(async_id);
        if (pending == pending_detections.end())
            return false;
        if (succeeded)
            assign_persistent_value(pending->second, value);
        pending_detections.erase(pending);
        return run_async_complete(async_id, {}, std::int64_t{ 0 }, succeeded);
    }

    void activate_async_waits(std::int64_t async_id) {
        if (async_id < 0)
            return;
        for (auto &[wait_id, waiting] : suspended) {
            if (waiting.wait_unit != WaitUnit::async_completion ||
                    waiting.wait_duration != async_id) {
                continue;
            }
            waiting.wait_duration = 0;
            waiting.wait_unit = WaitUnit::immediate;
            if (host) {
                host->cancel_wait(wait_id);
                host->schedule_wait(wait_id, 0, WaitUnit::immediate);
            }
        }
    }

    bool run_async_complete(std::int64_t async_id,
            const AsyncValueAddress &address, const Value &value,
            bool succeeded) {
        activate_async_waits(async_id);
        const auto callback = code().callbacks.find("async_complete");
        if (callback == code().callbacks.end())
            return true;

        ExecutionState state;
        state.callback_type = detail::callback_type_id("async_complete");
        state.callback_id = static_cast<std::int32_t>(
                host ? host->reserve_callback_id() : reserve_callback_id());
        state.async_id = async_id;
        state.async_type = address.type;
        state.async_parameter_one = address.parameter_one;
        state.async_parameter_two = address.parameter_two;
        state.async_exit_status = succeeded ? 1 : 0;
        state.async_value = value;
        state.frames.push_back(
                { ExecutionFrame::Kind::block, &callback->second });
        const auto result = execute(state);
        if (result == ExecutionResult::suspended)
            store_suspended(std::move(state));
        return result != ExecutionResult::failed && !has_errors();
    }

    bool complete_async_value(
            std::int64_t async_id, const Value &value, bool succeeded) {
        const auto pending = pending_async_values.find(async_id);
        if (pending == pending_async_values.end())
            return false;
        const auto address = pending->second;
        pending_async_values.erase(pending);
        return run_async_complete(async_id, address, value, succeeded);
    }

    bool complete_engine_command(std::int64_t async_id, bool succeeded) {
        if (pending_engine_commands.erase(async_id) == 0)
            return false;
        return run_async_complete(async_id, {}, std::int64_t{ 0 }, succeeded);
    }

    std::optional<std::int64_t> submit_engine_command(std::string_view command,
            std::span<const Value> arguments, bool synchronous) {
        if (!host)
            return std::nullopt;
        const auto async_id =
                host->start_engine_command(command, arguments, synchronous);
        if (!synchronous && async_id && *async_id >= 0)
            pending_engine_commands.insert(*async_id);
        return async_id;
    }

    void flush_controller_page_requests(ExecutionState &state) {
        auto requests = std::move(state.controller_page_requests);
        state.controller_page_requests.clear();
        for (const auto &request : requests) {
            const auto groups = variables.find(request.variables[0]);
            const auto parameters = variables.find(request.variables[1]);
            const auto assignments = variables.find(request.variables[2]);
            if (groups == variables.end() || parameters == variables.end() ||
                    assignments == variables.end()) {
                continue;
            }
            const auto *group_values =
                    std::get_if<std::vector<std::string>>(&groups->second);
            const auto *parameter_values =
                    std::get_if<std::vector<std::int64_t>>(&parameters->second);
            const auto *assignment_values =
                    std::get_if<std::vector<std::string>>(&assignments->second);
            if (!group_values || !parameter_values || !assignment_values)
                continue;
            const auto count = std::min({ group_values->size(),
                    parameter_values->size(), assignment_values->size() });
            std::array<Value, 3> arguments{
                std::vector<std::string>(
                        group_values->begin(), group_values->begin() + count),
                std::vector<std::int64_t>(parameter_values->begin(),
                        parameter_values->begin() + count),
                std::vector<std::string>(assignment_values->begin(),
                        assignment_values->begin() + count)
            };
            (void)submit_engine_command(
                    "set_controller_pages", arguments, false);
        }
    }

    std::optional<std::int64_t> start_engine_command(std::string_view command,
            std::span<const Value> arguments, bool synchronous) {
        if (active_state && !active_state->controller_page_requests.empty())
            flush_controller_page_requests(*active_state);
        return submit_engine_command(command, arguments, synchronous);
    }

    bool notify_async_value(std::int64_t type,
            std::optional<std::int64_t> parameter_one,
            std::optional<std::int64_t> parameter_two, const Value &value) {
        std::vector<AsyncValueAddress> matching_subscriptions;
        bool completed = true;
        for (const auto &subscription : async_value_subscriptions) {
            if (subscription.type == type &&
                    (!parameter_one ||
                            subscription.parameter_one == *parameter_one) &&
                    (!parameter_two ||
                            subscription.parameter_two == *parameter_two)) {
                matching_subscriptions.push_back(subscription);
            }
        }
        for (const auto &subscription : matching_subscriptions) {
            completed = run_async_complete(-1, subscription, value, false) &&
                        completed;
        }
        return !matching_subscriptions.empty() && completed;
    }

    void subscribe_to_async_value(const AsyncValueAddress &address) {
        async_value_subscriptions.push_back(address);
        if (host) {
            host->subscribe_async_value(
                    address.type, address.parameter_one, address.parameter_two);
        }
    }

    std::int64_t request_engine_value(const AsyncValueAddress &address) {
        const bool synchronous = current_callback_type == 5;
        if (!host)
            return -1;
        const auto async_id = host->request_async_value(address.type,
                address.parameter_one, address.parameter_two, synchronous);
        if (!synchronous && async_id && *async_id >= 0)
            pending_async_values[*async_id] = address;
        return synchronous || !async_id ? -1 : wrap_integer(*async_id);
    }

    std::int64_t set_engine_value(
            const AsyncValueAddress &address, const Value &value) {
        const bool synchronous = current_callback_type == 5;
        if (!host)
            return -1;
        const auto async_id =
                host->set_async_value(address.type, address.parameter_one,
                        address.parameter_two, value, synchronous);
        if (!synchronous && async_id && *async_id >= 0)
            pending_async_values[*async_id] = address;
        return synchronous || !async_id ? -1 : wrap_integer(*async_id);
    }

    bool apply_pending_persistent_value(
            std::string_view name, bool case_insensitive = false) {
        const auto normalized_name =
                case_insensitive ? lowercase(name) : std::string{ name };
        const auto pending = std::find_if(pending_persistent_values.begin(),
                pending_persistent_values.end(),
                [&](const PersistentVariable &variable) {
                    return (case_insensitive
                                           ? lowercase(variable.name)
                                           : variable.name) == normalized_name;
                });
        if (pending == pending_persistent_values.end())
            return false;
        assign_persistent_value(pending->name, pending->value);
        pending_persistent_values.erase(pending);
        return true;
    }

    void apply_all_pending_persistent_values() {
        for (const auto &variable : pending_persistent_values)
            assign_persistent_value(variable.name, variable.value);
        pending_persistent_values.clear();
    }

    Value coerce_scalar(std::string_view name, const Value &value) {
        if (name.empty())
            return value;
        if (name.front() == '$')
            return wrap_integer(as_integer(value));
        if (name.front() == '~')
            return as_real(value);
        if (name.front() == '@')
            return as_string(value);
        return value;
    }

    std::optional<EnvironmentSymbol> resolve_environment_symbol(
            std::string_view name, std::string_view source_name) const {
        if (detail::is_builtin_variable(source_name)) {
            const auto supplied =
                    supplied_builtin_values.find(std::string(source_name));
            if (supplied != supplied_builtin_values.end())
                return EnvironmentSymbol{ supplied->second, false };
        }
        if (!host)
            return std::nullopt;
        const auto lookup_name =
                detail::is_builtin_variable(source_name) ? source_name : name;
        return host->resolve_symbol(lookup_name);
    }

    std::string_view environment_lookup_name(
            std::string_view name, std::string_view source_name) const {
        return detail::is_builtin_variable(source_name) ? source_name : name;
    }

    enum class HostCallDisposition { forward, skip };

    HostCallDisposition normalize_engine_arguments(std::string_view name,
            std::vector<Value> &arguments, SourceLocation location) {
        const auto skip_with_warning = [&](std::string message) {
            warn(location, std::move(message));
            return HostCallDisposition::skip;
        };
        const auto clamp_ui_dimension = [&](std::int64_t minimum,
                                                std::int64_t maximum,
                                                std::string warning) {
            auto value = as_integer(arguments.front());
            if (value < minimum || value > maximum) {
                warn(location, std::move(warning));
                value = std::clamp(value, minimum, maximum);
            }
            arguments.front() = value;
        };
        if (name == "set_ui_height" && arguments.size() == 1) {
            clamp_ui_dimension(1, 8,
                    "set_ui_height(): Height of the performance "
                    "view must be between 1 "
                    "and 8 rows!");
        } else if (name == "set_ui_height_px" && arguments.size() == 1) {
            clamp_ui_dimension(50, 750,
                    "set_ui_height_px(): Height must be between 50 "
                    "and 750 pixels!");
        } else if (name == "set_ui_width_px" && arguments.size() == 1) {
            clamp_ui_dimension(633, 1000,
                    "set_ui_width_px(): Width must be between 633 "
                    "and 1000 pixels!");
        }
        if (name == "set_key_pressed_support" && arguments.size() == 1) {
            const auto enabled = as_integer(arguments.front());
            if (enabled < 0 || enabled > 1) {
                return skip_with_warning(
                        "set_key_pressed_support(): Value must be 0 or 1!");
            }
        }
        if (name == "set_group_dyn_par_name" && arguments.size() == 2) {
            const auto group =
                    static_cast<std::uint64_t>(as_integer(arguments[0]));
            if (group >= 64)
                return HostCallDisposition::skip;
            auto display_name = as_string(arguments[1]);
            if (display_name.size() > 320)
                display_name.resize(320);
            arguments[1] = std::move(display_name);
        }
        if (name == "fs_navigate" && arguments.size() == 2) {
            const auto control_id = as_integer(arguments[0]);
            const auto control_type = resolved_ui_control_type(control_id);

            if (!control_type) {
                return skip_with_warning(
                        "fs_navigate(): Control ID was not declared!");
            }
            if (*control_type != "ui_file_selector") {
                return skip_with_warning("fs_navigate(): Control ID does not "
                                         "belong to a ui_file_selector!");
            }
            arguments[1] =
                    std::int64_t{ as_integer(arguments[1]) > 0 ? 1 : -1 };
        }
        if ((name == "set_key_type" || name == "set_key_color" ||
                    name == "set_key_pressed") &&
                arguments.size() == 2) {
            const auto note = as_integer(arguments[0]);
            if (note < 0 || note > 127) {
                return skip_with_warning(
                        std::string(name) +
                        "(): Key number must be between 0 and 127!");
            }
            const auto value = as_integer(arguments[1]);
            const auto maximum = name == "set_key_type"    ? 2
                                 : name == "set_key_color" ? 20
                                                           : 1;
            if (value < 0 || value > maximum) {
                const auto message =
                        name == "set_key_type"
                                ? "set_key_type(): Invalid key type constant!"
                        : name == "set_key_color"
                                ? "set_key_color(): Invalid key color constant!"
                                : "set_key_pressed(): Key pressed state must be 0 or "
                                  "1!";
                return skip_with_warning(message);
            }
        }
        if (name == "set_key_name" && arguments.size() == 2) {
            const auto note = as_integer(arguments[0]);
            if (note < 0 || note > 127) {
                return skip_with_warning(
                        "set_key_name(): Key number must be between 0 and 127!");
            }
            arguments[1] = as_string(arguments[1]);
        }
        if (name == "set_keyrange" && arguments.size() == 3) {
            const auto first_note = as_integer(arguments[0]);
            const auto last_note = as_integer(arguments[1]);
            if (first_note < 0 || first_note > 127 || last_note < 0 ||
                    last_note > 127) {
                return skip_with_warning(
                        "set_keyrange(): Note number must be between 0 and 127!");
            }
            arguments[2] = as_string(arguments[2]);
        }
        if (name == "remove_keyrange" && arguments.size() == 1) {
            const auto note = as_integer(arguments[0]);
            if (note < 0 || note > 127) {
                return skip_with_warning("remove_keyrange(): Note number must "
                                         "be between 0 and 127!");
            }
        }
        if (name == "set_touchstrip_par" && arguments.size() == 3) {
            const auto touchstrip = as_integer(arguments[0]);
            if (touchstrip < 0 || touchstrip > 1) {
                return skip_with_warning(
                        "set_touchstrip_par(): Invalid touchstrip number!");
            }
            const auto parameter = as_integer(arguments[1]);
            if (parameter < 0 || parameter > 6) {
                return skip_with_warning(
                        "set_touchstrip_par(): Invalid touchstrip constant!");
            }
        }
        if (name == "set_snapshot_type" && arguments.size() == 1) {
            const auto snapshot_type = as_integer(arguments[0]);
            if (snapshot_type < 0 || snapshot_type > 3) {
                return skip_with_warning("set_snapshot_type(): Snapshot type "
                                         "value must be between 0 and 3!");
            }
        }
        if (name == "set_num_user_zones" && arguments.size() == 1) {
            const auto zone_count = as_integer(arguments[0]);
            if (zone_count < 0 || zone_count > 1024) {
                return skip_with_warning(
                        "set_num_user_zones() must be between 0 and 1024!");
            }
        }
        if (name == "set_voice_limit" && arguments.size() == 2) {
            const auto limit_type = as_integer(arguments[0]);
            if ((limit_type < 0 || limit_type > 1) &&
                    current_callback_type == 5)
                return HostCallDisposition::skip;
            if (limit_type >= 0 && limit_type <= 1) {
                arguments[1] = std::clamp<std::int64_t>(
                        as_integer(arguments[1]), 0, 128);
            }
        }
        if (name == "set_nks_nav_par" && arguments.size() == 3) {
            const auto page = as_integer(arguments[0]);
            const auto entry = as_integer(arguments[1]);
            if ((page < 0 || page > 127 || entry < 0 || entry > 47) &&
                    current_callback_type == 5)
                return HostCallDisposition::skip;
        }
        if (name == "set_nks_nav_name" && arguments.size() == 3) {
            const auto page = as_integer(arguments[0]);
            const auto entry = as_integer(arguments[1]);
            if ((page < 0 || page > 127 || entry < -1 || entry > 47) &&
                    current_callback_type == 5)
                return HostCallDisposition::skip;
            arguments[2] = as_string(arguments[2]);
        }
        if ((name == "load_ir_sample" || name == "load_ir_sample_m" ||
                    name == "load_midi_file" || name == "save_midi_file") &&
                !arguments.empty()) {
            arguments[0] = as_string(arguments[0]);
        }
        if (name == "load_ir_sample" && arguments.size() == 3) {
            auto selector = as_integer(arguments[2]);
            if (selector == 2)
                selector = 20;
            else if (selector >= 0x400 && selector <= 0x40f)
                selector -= 0x3fd;
            arguments[2] = selector;
        }
        if ((name == "set_control_help" || name == "set_knob_label" ||
                    name == "set_text" || name == "add_text_line") &&
                arguments.size() == 2)
            arguments[1] = as_string(arguments[1]);
        if (name == "set_script_title" && arguments.size() == 1)
            arguments[0] = as_string(arguments[0]);
        if (name == "set_skin_offset" && arguments.size() == 1) {
            const auto offset = as_integer(arguments[0]);
            if (offset < 0) {
                warn(location,
                        "set_skin_offset(): Offset cannot be a negative value!");
                arguments[0] = std::int64_t{ 0 };
            }
        }
        if (name == "set_ui_color" && arguments.size() == 1) {
            const auto color =
                    static_cast<std::uint32_t>(as_integer(arguments[0]));
            arguments[0] = static_cast<std::int64_t>(
                    static_cast<std::int32_t>(color | UINT32_C(0xff000000)));
        }
        if (name == "set_map_editor_event_color" && arguments.size() == 1) {
            const auto color =
                    static_cast<std::uint32_t>(as_integer(arguments[0]));
            arguments[0] = static_cast<std::int64_t>(static_cast<std::int32_t>(
                    (color & UINT32_C(0x00ffffff)) | UINT32_C(0x80000000)));
        }
        if (name == "set_ui_wf_property" && arguments.size() == 4) {
            const auto property = as_integer(arguments[1]);
            if (property < 1 || property > 10) {
                return skip_with_warning(
                        "set_wf_ui_property(): Invalid ui_waveform property!");
            }
            const auto index = as_integer(arguments[2]);
            if (property == 3 && static_cast<std::uint64_t>(index) >= 256) {
                return skip_with_warning("set_wf_ui_property(): ui_waveform "
                                         "table index is out of range!");
            }
            auto value = as_integer(arguments[3]);
            if (property == 6 || property == 7 || property == 9 ||
                    property == 10)
                value = static_cast<std::uint32_t>(value) &
                        UINT32_C(0x00ffffff);
            else if (property == 8)
                value = static_cast<std::uint32_t>(value) &
                        UINT32_C(0x000000ff);
            arguments[3] = value;
        }
        if (name == "attach_level_meter" && arguments.size() == 5) {
            const auto control_id = as_integer(arguments[0]);
            const auto control_type = resolved_ui_control_type(control_id);
            if (!control_type) {
                return skip_with_warning(
                        "attach_level_meter(): Control ID was not declared!");
            }
            if (*control_type != "ui_level_meter") {
                return skip_with_warning(
                        "attach_level_meter(): Control ID does not belong to a "
                        "ui_level_meter!");
            }

            const auto channel = as_integer(arguments[3]);
            if (static_cast<std::uint64_t>(channel) >= 16) {
                warn(location, "attach_level_meter(): Channel index must be "
                               "between 0 and 15!");
                return HostCallDisposition::forward;
            }
            const auto generic_index = as_integer(arguments[4]);
            if (generic_index < -2 || generic_index > 15) {
                warn(location,
                        "attach_level_meter(): Invalid generic index value!");
                return HostCallDisposition::forward;
            }
            if (generic_index == -2)
                arguments[4] = std::int64_t{ -20 };
        }
        if (name == "set_knob_unit" && arguments.size() == 2) {
            const auto unit =
                    static_cast<std::uint64_t>(as_integer(arguments[1]));
            if (unit > 6) {
                warn(location, "set_knob_unit(): Invalid knob unit constant!");
                arguments[1] = std::int64_t{ 0 };
            }
        }
        if (name == "set_table_steps_shown" && arguments.size() == 2) {
            auto steps = as_integer(arguments[1]);
            if (steps < 1) {
                warn(location,
                        "set_table_steps_shown(): Value must be at least 1!");
                steps = 1;
            }
            if (host) {
                const auto control =
                        ui_control_names.find(as_integer(arguments[0]));
                if (control != ui_control_names.end()) {
                    if (const auto count = host->ui_table_element_count(
                                control->second);
                            count && steps > *count) {
                        warn(location, "set_table_steps_shown(): Value cannot "
                                       "be larger than " +
                                               std::to_string(*count) + "!");
                        steps = *count;
                    }
                }
            }
            arguments[1] = steps;
        }
        if (name == "set_knob_defval" && arguments.size() == 2 && host) {
            const auto control =
                    ui_control_names.find(as_integer(arguments[0]));
            if (control != ui_control_names.end()) {
                if (const auto range =
                                host->ui_control_value_range(control->second)) {
                    const auto minimum = std::min(range->first, range->second);
                    const auto maximum = std::max(range->first, range->second);
                    const auto requested = as_integer(arguments[1]);
                    const auto clipped =
                            std::clamp(requested, minimum, maximum);
                    if (requested != clipped) {
                        warn(location, "set_knob_defval(): Invalid default "
                                       "value! Input value was clipped to " +
                                               std::to_string(clipped) + ".");
                        arguments[1] = clipped;
                    }
                }
            }
        }
        if ((name == "set_control_par_str" ||
                    name == "set_control_par_str_arr") &&
                arguments.size() >= 3)
            arguments[2] = as_string(arguments[2]);
        if (name == "add_menu_item" && arguments.size() == 3)
            arguments[1] = as_string(arguments[1]);
        if (name == "set_menu_item_str" && arguments.size() == 3)
            arguments[2] = as_string(arguments[2]);
        if ((name == "set_menu_item_value" ||
                    name == "set_menu_item_visibility" ||
                    name == "set_menu_item_str") &&
                arguments.size() == 3) {
            const auto warning_name = name == "set_menu_item_str"
                                              ? "set_menu_item_visibility"
                                              : name;
            const auto control_type =
                    resolved_ui_control_type(as_integer(arguments[0]));
            if (!control_type) {
                return skip_with_warning(std::string(warning_name) +
                                         "(): Control ID was not declared!");
            }
            if (*control_type != "ui_menu") {
                return skip_with_warning(
                        std::string(warning_name) +
                        "(): Control ID does not belong to a ui_menu!");
            }
            if (const auto count = ui_menu_item_count(arguments[0])) {
                const auto index = as_integer(arguments[1]);
                if (index < 0 || index >= *count)
                    return HostCallDisposition::skip;
            }
            if (name == "set_menu_item_visibility")
                arguments[2] = std::int64_t{ as_integer(arguments[2]) > 0 };
        }
        if ((name == "set_control_par" || name == "set_control_par_str" ||
                    name == "set_control_par_arr" ||
                    name == "set_control_par_str_arr" ||
                    name == "set_control_par_real_arr") &&
                arguments.size() >= 2) {
            if ((name == "set_control_par" || name == "set_control_par_str") &&
                    !ui_control_is_declared(as_integer(arguments[0]))) {
                const auto low_string_id = name == "set_control_par_str" &&
                                           as_integer(arguments[0]) < 0x8000;
                return skip_with_warning(std::string(name) +
                                         "(): Control ID was not declared" +
                                         (low_string_id ? "" : "!"));
            }
            const auto parameter = as_integer(arguments[1]);
            if (parameter < 0 || parameter > 118) {
                if (name == "set_control_par_real_arr") {
                    return skip_with_warning(
                            "set_control_par_real_arr(): Invalid control "
                            "parameter! Only $CONTROL_PAR_VALUE can be used with "
                            "this command.");
                }
                return skip_with_warning(
                        std::string(name) + "(): Invalid control parameter!");
            }
            const auto ordinary_control = as_integer(arguments[0]) >= 0x8000;
            const auto control_type =
                    resolved_ui_control_type(as_integer(arguments[0]));
            const auto file_selector =
                    control_type && *control_type == "ui_file_selector";
            if (ordinary_control &&
                    (name == "set_control_par" ||
                            name == "set_control_par_arr") &&
                    (parameter == 29 ||
                            (parameter == 34 && name == "set_control_par")) &&
                    current_callback_type != 5) {
                const auto constant = parameter == 29
                                              ? "$CONTROL_PAR_AUTOMATION_ID"
                                              : "$CONTROL_PAR_ALLOW_AUTOMATION";
                return skip_with_warning(
                        std::string(constant) +
                        " can only be used in \"on init\" callback!");
            }
            if (ordinary_control && parameter == 29 && host &&
                    host->is_multi_script()) {
                return skip_with_warning("$CONTROL_PAR_AUTOMATION_ID cannot be "
                                         "used in a multi script!");
            }
            if (ordinary_control && name == "set_control_par" &&
                    parameter == 56 && current_callback_type != 5) {
                return skip_with_warning("$CONTROL_PAR_Z_LAYER can only be "
                                         "used in \"on init\" callback!");
            }
            if (ordinary_control && parameter >= 110 && parameter <= 113 &&
                    current_callback_type != 5) {
                static constexpr std::array<const char *, 4> names{
                    "$CONTROL_PAR_NKS_TYPE", "$CONTROL_PAR_NKS_STYLE",
                    "$CONTROL_PAR_NKS_NUM_VALUES", "$CONTROL_PAR_NKS_STR_VALUES"
                };
                return skip_with_warning(
                        std::string(names[static_cast<std::size_t>(
                                parameter - 110)]) +
                        " can only be used in \"on init\" callback!");
            }
            if (ordinary_control && file_selector &&
                    current_callback_type != 5 &&
                    (name == "set_control_par" ||
                            name == "set_control_par_arr")) {
                if (parameter == 37) {
                    return skip_with_warning(
                            "$CONTROL_PAR_FILE_TYPE can only be used in \"on "
                            "init\" callback!");
                }
                if (parameter == 48) {
                    return skip_with_warning(
                            "$CONTROL_PAR_COLUMN_WIDTH can only be used in \"on "
                            "init\" callback!");
                }
            }
            if (ordinary_control && file_selector &&
                    current_callback_type != 5 &&
                    (name == "set_control_par_str" ||
                            name == "set_control_par_str_arr") &&
                    parameter == 55) {
                return skip_with_warning("$CONTROL_PAR_FILEPATH can only be "
                                         "used in \"on init\" callback!");
            }
            if (name == "set_control_par" && parameter == 117) {
                return skip_with_warning(
                        "The value of $CONTROL_PAR_TYPE cannot be changed! This "
                        "control parameter only works with get_control_par().");
            }
            if (name == "set_control_par" && parameter == 73 && control_type &&
                    (*control_type == "ui_button" ||
                            *control_type == "ui_switch")) {
                const auto control_id = as_integer(arguments[0]);
                if (as_integer(arguments[2]) == 0 &&
                        ui_text_shifting_disabled.contains(control_id)) {
                    return skip_with_warning(
                            "$CONTROL_PAR_DISABLE_TEXT_SHIFTING: Re-enabling text "
                            "shifting is not allowed!");
                }
                if (as_integer(arguments[2]) != 0)
                    ui_text_shifting_disabled.insert(control_id);
            }
            if (name == "set_control_par_real_arr") {
                const auto control_id = as_integer(arguments[0]);
                if (control_id >= 0x4001 && control_id <= 0x4007) {
                    return skip_with_warning(
                            "set_control_par_real_arr(): This command cannot be "
                            "used with internal control IDs!");
                }
                if (parameter != 12) {
                    return skip_with_warning(
                            "set_control_par_real_arr(): Invalid control "
                            "parameter! Only $CONTROL_PAR_VALUE can be used with "
                            "this command.");
                }
            }
            if ((name == "set_control_par_arr" ||
                        name == "set_control_par_str_arr" ||
                        name == "set_control_par_real_arr") &&
                    !ui_control_is_declared(as_integer(arguments[0]))) {
                const auto low_string_id = name == "set_control_par_str_arr" &&
                                           as_integer(arguments[0]) < 0x8000;
                return skip_with_warning(std::string(name) +
                                         "(): Control ID was not declared" +
                                         (low_string_id ? "" : "!"));
            }
            if (name == "set_control_par_arr") {
                const auto control =
                        resolved_ui_control_type(as_integer(arguments[0]));
                if (control && *control != "ui_table" &&
                        *control != "ui_menu" && *control != "ui_xy") {
                    return skip_with_warning(
                            "set_control_par_arr() can only be used with ui_table, "
                            "ui_menu and ui_xy controls!");
                }
            }
            if (name == "set_control_par_real_arr") {
                const auto control =
                        resolved_ui_control_type(as_integer(arguments[0]));
                if (control && *control != "ui_xy") {
                    return skip_with_warning(
                            "set_control_par_real_arr() can only be used with "
                            "ui_xy controls!");
                }
            }
        }
        if ((name == "move_control" || name == "move_control_px") &&
                arguments.size() == 3) {
            auto x = as_integer(arguments[1]);
            auto y = as_integer(arguments[2]);
            const auto maximum_x = name == "move_control" ? 6 : 1000;
            auto maximum_y = std::int64_t{ 750 };
            if (name == "move_control") {
                maximum_y = 6;
                if (host) {
                    if (const auto height = host->ui_height_grid();
                            height && *height > 0)
                        maximum_y = *height * 2;
                }
            }
            if (x < 0 || x > maximum_x) {
                warn(location,
                        std::string(name) +
                                (name == "move_control"
                                                ? "(): X position must be between 0 and 6!"
                                                : "(): X position must be between 0 and 1000 "
                                                  "pixels!"));
                x = std::clamp<std::int64_t>(x, 0, maximum_x);
            }
            if (y < 0 || y > maximum_y) {
                warn(location,
                        std::string(name) +
                                "(): Y position must be between 0 and " +
                                std::to_string(maximum_y) +
                                (name == "move_control" ? "!" : " pixels!"));
                y = std::clamp<std::int64_t>(y, 0, maximum_y);
            }
            arguments[1] = x;
            arguments[2] = y;
        }
        if (name == "attach_zone" && arguments.size() == 3 && host) {
            const auto zone = as_integer(arguments[1]);
            if (const auto exists = host->zone_exists(zone);
                    exists && !*exists) {
                warn(location, "attach_zone(): Zone ID was not found!");
                arguments[1] = std::int64_t{ -1 };
            }
        }
        if (name == "set_zone_par" && arguments.size() == 3) {
            const auto zone = as_integer(arguments[0]);
            const auto parameter = as_integer(arguments[1]);
            const auto value = as_integer(arguments[2]);
            const auto snapshot = host ? host->snapshot_type() : std::nullopt;
            const bool snapshot_allows_zone =
                    snapshot && (*snapshot == 2 || *snapshot == 3);
            if (host) {
                if (const auto exists = host->zone_exists(zone);
                        exists && !*exists && !snapshot_allows_zone) {
                    return skip_with_warning("set_zone_par(): Zone ID " +
                                             std::to_string(zone) +
                                             " was not found!");
                }
                if (const auto user_zone = host->zone_is_user(zone);
                        user_zone && !*user_zone && !snapshot_allows_zone) {
                    return skip_with_warning(
                            "set_zone_par() can only be used with user zones, or "
                            "with all zones in case of snapshot modes 2 and 3!");
                }
            }
            if (parameter == 17) {
                return skip_with_warning(
                        "set_zone_par(): $ZONE_PAR_SAMPLE_RATE is read only!");
            }
            if (parameter == 19) {
                return skip_with_warning(
                        "set_zone_par(): $ZONE_PAR_SELECTED is read only!");
            }
            if (parameter == 10) {
                const auto decibels = std::max(value * 0.01, -36.0);
                arguments[2] = std::exp(decibels * std::log(2.0) / 6.0);
            } else if (parameter == 11) {
                arguments[2] = std::max(value * 0.001, -100.0);
            } else if (parameter == 12) {
                arguments[2] = std::min(std::exp2(value * 0.01 / 12.0), 36.0);
            } else if (parameter == 18) {
                arguments[2] = std::max(value * 0.001, 0.1);
            }
        }
        if (name == "set_loop_par" && arguments.size() == 4) {
            const auto zone = as_integer(arguments[0]);
            const auto loop = as_integer(arguments[1]);
            const auto parameter = as_integer(arguments[2]);
            const auto value = as_integer(arguments[3]);
            if (host) {
                if (const auto exists = host->zone_exists(zone);
                        exists && !*exists) {
                    return skip_with_warning("set_loop_par(): Zone ID " +
                                             std::to_string(zone) +
                                             " was not found!");
                }
            }
            if (loop < 0 || loop > 7) {
                return skip_with_warning(
                        "set_loop_par(): Loop index must be between 0 and 7!");
            }
            if (host) {
                if (const auto user_zone = host->zone_is_user(zone);
                        user_zone && !*user_zone) {
                    return skip_with_warning(
                            "set_loop_par() can only be used with user zones!");
                }
            }
            if (parameter < 0x208 || parameter > 0x20d) {
                return current_callback_type == 5
                               ? HostCallDisposition::skip
                               : HostCallDisposition::forward;
            }
            if (parameter == 0x208) {
                if (value < 0 || value > 4) {
                    return current_callback_type == 5
                                   ? HostCallDisposition::skip
                                   : HostCallDisposition::forward;
                }
            } else if (parameter == 0x209 || parameter == 0x20b ||
                       parameter == 0x20c) {
                arguments[3] = std::max<std::int64_t>(value, 0);
            } else if (parameter == 0x20a) {
                arguments[3] = std::max<std::int64_t>(value, 4);
            } else {
                const auto exponent = std::max(value * 0.01 / 12.0, -12.0);
                arguments[3] = std::exp2(exponent);
            }
        }
        if ((name == "get_key_color" || name == "get_key_triggerstate" ||
                    name == "get_key_type") &&
                arguments.size() == 1) {
            const auto note = as_integer(arguments.front());
            if (note < 0 || note > 127)
                return HostCallDisposition::skip;
        }
        if ((name == "change_note" || name == "change_velo") &&
                arguments.size() == 2) {
            auto value = as_integer(arguments[1]);
            const auto minimum = name == "change_note" ? 0 : 1;
            if (value < minimum || value > 127) {
                warn(location,
                        std::string(name) +
                                (name == "change_note"
                                                ? "(): Note number must be between 0 and 127!"
                                                : "(): Velocity value must be between 1 and "
                                                  "127!"));
                value = std::clamp<std::int64_t>(value, minimum, 127);
            }
            arguments[1] = value;
            if (as_integer(arguments[0]) < 0) {
                warn(location, std::string(name) +
                                       "(): Addressing events via event "
                                       "marks is not allowed!");
                return HostCallDisposition::skip;
            }
            return HostCallDisposition::forward;
        }

        if (name == "play_note" && arguments.size() == 4) {
            auto note = as_integer(arguments[0]);
            auto velocity = as_integer(arguments[1]);
            if (note < -1 || note > 127) {
                warn(location,
                        "play_note(): Note number must be between 0 and 127!");
                note = std::clamp<std::int64_t>(note, -1, 127);
            }
            if (velocity == 0 || velocity < -1 || velocity > 127) {
                warn(location,
                        "play_note(): Velocity value must be between 1 and 127!");
                if (velocity == 0)
                    velocity = 1;
                else
                    velocity = std::clamp<std::int64_t>(velocity, -1, 127);
            }
            arguments[0] = note;
            arguments[1] = velocity;
            arguments[2] = std::max<std::int64_t>(0, as_integer(arguments[2]));
        }
        if ((name == "note_off" || name == "note_off_no_cb") &&
                arguments.size() == 2) {
            arguments[1] = std::max<std::int64_t>(0, as_integer(arguments[1]));
        }
        if (name == "set_event_par" && arguments.size() == 3) {
            const auto parameter = as_integer(arguments[1]);
            if (parameter >= 0x400) {
                warn(location, "set_event_par(): Invalid event parameter!");
                return HostCallDisposition::skip;
            }
            auto value = as_integer(arguments[2]);
            if (parameter == 7) {
                if (value < 0) {
                    value = 0;
                    warn(location,
                            "set_event_par(): Note number must be between 0 and "
                            "127! Input value was clipped to 0.");
                } else if (value > 127) {
                    value = 127;
                    warn(location,
                            "set_event_par(): Note number must be between 0 and "
                            "127! Input value was clipped to 127.");
                }
            }
            if ((parameter == 7 || parameter == 9) && value < 1) {
                value = 1;
                warn(location,
                        "set_event_par(): Velocity value must be between 1 and "
                        "127! Input value was clipped to 1.");
            } else if (parameter == 9 && value > 127) {
                value = 127;
                warn(location,
                        "set_event_par(): Velocity value must be between 1 and "
                        "127! Input value was clipped to 127.");
            }
            arguments[2] = value;
        }
        if (name == "set_event_par_arr" && arguments.size() == 4) {
            const auto parameter = as_integer(arguments[1]);
            if (parameter >= 0x404) {
                warn(location, "set_event_par_arr(): Invalid event parameter!");
                return HostCallDisposition::skip;
            }
            auto value = as_integer(arguments[2]);
            if (parameter == 10 && (value < 0 || value > 1)) {
                value = std::clamp<std::int64_t>(value, 0, 1);
                warn(location,
                        "set_event_par_arr(): $EVENT_PAR_ALLOW_GROUP value must "
                        "be 0 or 1! Input value was clipped to " +
                                std::to_string(value) + ".");
            }
            arguments[2] = value;
        }
        if (name == "get_event_mark" && arguments.size() == 2 &&
                as_integer(arguments[0]) < 0) {
            warn(location,
                    "get_event_mark(): Only unique event IDs are allowed!");
            return HostCallDisposition::skip;
        }
        if (name == "set_kill_priority" && arguments.size() == 2) {
            auto priority = as_integer(arguments[1]);
            if (priority < 1 || priority > 1000) {
                priority = std::clamp<std::int64_t>(priority, 1, 1000);
                warn(location,
                        "set_kill_priority(): Voice kill priority must be between "
                        "1 and 1000! Input value was clipped to " +
                                std::to_string(priority) + ".");
            }
            arguments[1] = priority;
        }
        const bool fade_in_curve = name == "fade_in" && arguments.size() == 3;
        const bool fade_out_curve = name == "fade_out" && arguments.size() == 4;
        if (fade_in_curve || fade_out_curve) {
            const auto curve_index = arguments.size() - 1;
            auto curve = static_cast<std::uint8_t>(
                    as_integer(arguments[curve_index]));
            if (curve > 4) {
                warn(location,
                        std::string(name) +
                                "(): unknown fade curve; using $NI_FADE_LINEAR");
                curve = 0;
            }
            arguments[curve_index] = static_cast<std::int64_t>(curve);
        }
        if (name == "set_controller" && arguments.size() == 2) {
            const auto controller = as_integer(arguments[0]);
            const auto value = as_integer(arguments[1]);
            if (controller < 0 || controller > 129) {
                return skip_with_warning("set_controller(): Controller number "
                                         "must be between 0 and 127!");
            }
            if (controller == 128) {
                if (value < -8192 || value > 8191) {
                    return skip_with_warning(
                            "set_controller(): Pitch bend value must be between "
                            "-8192 and 8191!");
                }
            } else if (value < 0 || value > 127) {
                return skip_with_warning("set_controller(): Controller value "
                                         "must be between 0 and 127!");
            }
        }
        if (name == "set_poly_at" && arguments.size() == 2) {
            if (as_integer(arguments[0]) < 0 ||
                    as_integer(arguments[0]) > 127) {
                return skip_with_warning(
                        "set_poly_at(): Note number must be between 0 and 127!");
            }
            if (as_integer(arguments[1]) < 0 ||
                    as_integer(arguments[1]) > 127) {
                return skip_with_warning(
                        "set_poly_at(): Value must be between 0 and 127!");
            }
        }
        if ((name == "set_rpn" || name == "set_nrpn") &&
                arguments.size() == 2) {
            const auto label = name == "set_rpn" ? "RPN" : "NRPN";
            if (as_integer(arguments[0]) < 0 ||
                    as_integer(arguments[0]) > 16383) {
                return skip_with_warning(
                        std::string(name) + "(): " + label +
                        " controller must be between 0 and 16383!");
            }
            if (as_integer(arguments[1]) < 0 ||
                    as_integer(arguments[1]) > 16383) {
                return skip_with_warning(std::string(name) + "(): " + label +
                                         " value must be between 0 and 16383!");
            }
        }
        if ((name == "set_midi" || name == "send_midi") &&
                arguments.size() == 4) {
            if (as_integer(arguments[0]) < 0 || as_integer(arguments[0]) > 63) {
                return skip_with_warning(
                        "set_midi(): MIDI channel must be between 0 and 63!");
            }
            auto command = as_integer(arguments[1]);
            if (command == 0x100)
                command = 0x20;
            else if (command == 0x101)
                command = 0x30;
            const bool wide_values = (command & ~std::int64_t{ 0x10 }) == 0x20;
            const auto maximum_value = wide_values ? 16383 : 127;
            if (!wide_values && (command < 0 || command > 255)) {
                const auto clipped = std::clamp<std::int64_t>(command, 0, 255);
                warn(location, "set_midi(): Invalid MIDI command! Using " +
                                       std::to_string(clipped) + " instead.");
                command = clipped;
            }
            arguments[1] = command;
            for (std::size_t index = 2; index < 4; ++index) {
                auto value = as_integer(arguments[index]);
                if (value < 0 || value > maximum_value) {
                    value = std::clamp<std::int64_t>(value, 0, maximum_value);
                    warn(location, "set_midi(): Invalid value for byte " +
                                           std::to_string(index - 1) +
                                           "! Using " + std::to_string(value) +
                                           " instead.");
                    arguments[index] = value;
                }
            }
        }
        if (name == "redirect_output" && arguments.size() == 3) {
            const auto output_type = as_integer(arguments[1]);
            const auto output_index = as_integer(arguments[2]);
            if (output_type < 0 || output_type > 3) {
                return skip_with_warning(
                        "redirect_output(): Incorrect output type!");
            }
            if (output_type == 1 && (output_index < 0 || output_index > 127)) {
                return skip_with_warning("redirect_output(): Output channel "
                                         "index must be between 0 and 127!");
            }
            if (output_type == 2 && (output_index < 0 || output_index > 3)) {
                return skip_with_warning("redirect_output(): Aux output index "
                                         "must be between 0 and 3!");
            }
            if (output_type == 3 && (output_index < 0 || output_index > 15)) {
                return skip_with_warning("redirect_output(): Instrument bus "
                                         "index must be between 0 and 15!");
            }
            if (host && output_type != 0) {
                const auto available = host->output_destination_available(
                        output_type, output_index);
                if (available && !*available) {
                    if (output_type == 1) {
                        return skip_with_warning("redirect_output(): Output "
                                                 "channel is not available!");
                    }
                    return HostCallDisposition::skip;
                }
            }
        }
        if (name == "redirect_midi" && arguments.size() == 3) {
            const auto slot = as_integer(arguments[1]);
            const auto sub_index = as_integer(arguments[2]);
            if (slot < 0 || slot > 63) {
                return skip_with_warning(
                        "redirect_midi(): Slot index must be between 0 and 63!");
            }
            if (sub_index < 0 || sub_index > 127) {
                return skip_with_warning(
                        "redirect_midi(): Sub-index must be between 0 and 127!");
            }
            if (host) {
                const auto exists =
                        host->midi_redirect_target_exists(slot, sub_index);
                if (exists && !*exists) {
                    return skip_with_warning(
                            "redirect_midi(): Target does not exist!");
                }
            }
        }
        if (name == "cc_delivery_request" && arguments.size() == 1) {
            const auto controller = as_integer(arguments[0]);
            if (controller < 0 || controller > 128) {
                return skip_with_warning("cc_delivery_request(): Controller "
                                         "number must be between 0 and 127!");
            }
        }
        if ((name == "allow_group" || name == "disallow_group") &&
                arguments.size() == 1) {
            const auto group = as_integer(arguments[0]);
            const auto available_groups =
                    host ? host->group_count() : std::nullopt;
            if (group < -1 ||
                    (available_groups && group >= *available_groups)) {
                return skip_with_warning(
                        std::string(name) + "(): Invalid group index!");
            }
        }
        if (name == "purge_group" && arguments.size() == 2) {
            const auto group = as_integer(arguments[0]);
            const auto available_groups =
                    host ? host->group_count() : std::nullopt;
            if (group < 0 || (available_groups && group >= *available_groups))
                return HostCallDisposition::skip;
        }
        if ((name == "set_listener" || name == "change_listener_par") &&
                arguments.size() == 2) {
            const auto signal = as_integer(arguments[0]);
            auto parameter =
                    static_cast<std::int32_t>(as_integer(arguments[1]));
            if (signal < 1 || signal > 4) {
                return skip_with_warning(std::string(name) +
                                         "(): Invalid listener signal type!");
            }
            if (signal == 3 && parameter > 0 && host) {
                if (const auto sample_rate = host->current_sample_rate_hz()) {
                    const auto frames = static_cast<float>(parameter) *
                                        1.0e-6F *
                                        static_cast<float>(*sample_rate);
                    arguments[1] = std::min<std::int64_t>(
                            static_cast<std::int64_t>(frames),
                            std::numeric_limits<std::int32_t>::max());
                }
            }
            if (signal == 4 && parameter != 0 &&
                    (parameter < 1 || parameter >= 25 || 96 % parameter != 0)) {
                return skip_with_warning(
                        std::string(name) + "(): Invalid beat timer divisor!");
            }
        }
        if ((name == "_reset_rls_trig_counter" ||
                    name == "reset_rls_trig_counter") &&
                arguments.size() == 1) {
            const auto note = as_integer(arguments[0]);
            if (note < 0 || note > 127) {
                return skip_with_warning("reset_rls_trig_counter(): Note "
                                         "number must be between 0 and 127!");
            }
        }
        return HostCallDisposition::forward;
    }

    Value evaluate_call(const Expression &expression) {
        static const std::unordered_set<std::string> detection_functions = {
            "detect_key", "detect_pitch", "detect_loudness", "detect_peak",
            "detect_rms", "detect_tempo", "detect_sample_type",
            "detect_drum_type", "detect_instrument_type"
        };
        if (detection_functions.contains(expression.text) &&
                expression.children.size() == 2 &&
                expression.children[1].kind == Expression::Kind::name &&
                expression.children[1].children.empty()) {
            const auto zone = as_integer(evaluate(expression.children[0]));
            const auto &target = expression.children[1].text;
            const bool synchronous = current_callback_type == 5;
            if (host) {
                if (const auto exists = host->zone_exists(zone);
                        exists && !*exists) {
                    warn(expression.location, expression.text + "(): Zone ID " +
                                                      std::to_string(zone) +
                                                      " was not found!");
                    return std::int64_t{ -1 };
                }
                if (const auto result = host->detect_audio(
                            expression.text, zone, synchronous)) {
                    if (result->detected_value)
                        assign_persistent_value(
                                target, *result->detected_value);
                    if (!synchronous && result->async_id >= 0) {
                        pending_detections[result->async_id] = target;
                    }
                    return synchronous ? std::int64_t{ -1 }
                                       : wrap_integer(result->async_id);
                }
            }
            return std::int64_t{ -1 };
        }
        if (expression.text == "subscribe_async" &&
                expression.children.size() == 3) {
            const AsyncValueAddress address{ as_integer(evaluate(
                                                     expression.children[0])),
                as_integer(evaluate(expression.children[1])),
                as_integer(evaluate(expression.children[2])) };
            subscribe_to_async_value(address);
            return std::int64_t{ 0 };
        }
        if (expression.text == "request_value_async" &&
                expression.children.size() == 3) {
            const AsyncValueAddress address{ as_integer(evaluate(
                                                     expression.children[0])),
                as_integer(evaluate(expression.children[1])),
                as_integer(evaluate(expression.children[2])) };
            return request_engine_value(address);
        }
        if ((expression.text == "set_value_async" ||
                    expression.text == "set_value_async_str") &&
                expression.children.size() == 4) {
            const AsyncValueAddress address{ as_integer(evaluate(
                                                     expression.children[0])),
                as_integer(evaluate(expression.children[1])),
                as_integer(evaluate(expression.children[2])) };
            Value value = evaluate(expression.children[3]);
            if (expression.text == "set_value_async")
                value = wrap_integer(as_integer(value));
            else
                value = as_string(value);
            return set_engine_value(address, value);
        }
        const auto name = detail::canonical_builtin_name(expression.text);
        if (name == "num_elements" && expression.children.size() == 1) {
            const auto &array = expression.children.front();
            // user arrays only need their length, don't copy the whole thing
            // on every loop condition, builtins still go through the checks
            if (array.kind == Expression::Kind::name &&
                    array.children.empty() &&
                    !detail::is_builtin_variable(array.source_name)) {
                if (const auto found = variables.find(array.text);
                        found != variables.end()) {
                    if (const auto values =
                                    std::get_if<std::vector<std::int64_t>>(
                                            &found->second))
                        return wrap_integer(values->size());
                    if (const auto values = std::get_if<std::vector<double>>(
                                &found->second))
                        return wrap_integer(values->size());
                    if (const auto values =
                                    std::get_if<std::vector<std::string>>(
                                            &found->second))
                        return wrap_integer(values->size());
                }
            }
        }
        if (name == "get_ui_id" && expression.children.size() == 1 &&
                expression.children.front().kind == Expression::Kind::name) {
            const auto &control_name = expression.children.front().text;
            if (const auto control = ui_control_ids.find(control_name);
                    control != ui_control_ids.end()) {
                return control->second;
            }
            if (host) {
                const std::array<Value, 1> named_argument{ Value{
                        control_name } };
                if (const auto result = host->call(name, named_argument)) {
                    const auto id = as_integer(*result);
                    if (host->ui_control_type(id))
                        ui_control_names[id] = control_name;
                    return wrap_integer(id);
                }
            }
            return std::int64_t{ 0 };
        }

        std::vector<Value> arguments;
        arguments.reserve(expression.children.size());
        for (std::size_t index = 0; index < expression.children.size();
                ++index) {
            const auto &argument = expression.children[index];
            if (index == 0 && detail::is_pgs_call_name(expression.text) &&
                    argument.kind == Expression::Kind::call &&
                    argument.children.empty()) {
                arguments.emplace_back(argument.text);
            } else {
                arguments.push_back(evaluate(argument));
            }
        }

        const auto first = [&]() -> const Value & { return arguments.at(0); };
        const auto first_real = [&]() { return as_real(first()); };

        if (name == "get_scale_name" && arguments.size() == 2) {
            const auto scale = as_integer(arguments[0]);
            if (scale < 0 ||
                    scale >= static_cast<std::int64_t>(scale_infos.size()))
                return std::string{};
            const auto &info = scale_infos[static_cast<std::size_t>(scale)];
            return std::string(as_integer(arguments[1]) == 0 ? info.name
                                                             : info.short_name);
        }
        if (name == "circle_of_fifths_distance" && arguments.size() == 2) {
            return circle_of_fifths_distance(
                    as_integer(arguments[0]), as_integer(arguments[1]));
        }

        if (name == "fs_get_filename" && arguments.size() == 2) {
            const auto control_id = as_integer(arguments[0]);
            const auto control_type = resolved_ui_control_type(control_id);

            if (!control_type) {
                warn(expression.location,
                        "fs_get_filename(): Control ID was not declared!");
                return std::string{};
            }
            if (*control_type != "ui_file_selector") {
                warn(expression.location, "fs_get_filename(): Control ID does "
                                          "not belong to a ui_file_selector!");
                return std::string{};
            }

            const auto return_parameter = as_integer(arguments[1]);
            if (return_parameter < 0 || return_parameter > 2) {
                warn(expression.location, "fs_get_filename(): Invalid file "
                                          "name return parameter used!");
                return std::string{};
            }
            if (host)
                if (const auto result = host->call(name, arguments))
                    return as_string(*result);
            return std::string{};
        }

        if (name == "num_elements") {
            if (const auto values =
                            std::get_if<std::vector<std::int64_t>>(&first()))
                return wrap_integer(values->size());
            if (const auto values = std::get_if<std::vector<double>>(&first()))
                return wrap_integer(values->size());
            if (const auto values =
                            std::get_if<std::vector<std::string>>(&first()))
                return wrap_integer(values->size());
            return std::int64_t{ 0 };
        }
        if (name == "search") {
            const auto values =
                    std::get_if<std::vector<std::int64_t>>(&first());
            if (!values || values->empty() || arguments.size() < 2)
                return std::int64_t{ -1 };
            auto start = arguments.size() > 2 ? as_integer(arguments[2]) : 0;
            auto end = arguments.size() > 3
                               ? as_integer(arguments[3])
                               : static_cast<std::int64_t>(values->size() - 1);
            start = std::max<std::int64_t>(0, start);
            if (end < 0 || end >= static_cast<std::int64_t>(values->size()))
                end = static_cast<std::int64_t>(values->size() - 1);
            start = std::min(
                    start, static_cast<std::int64_t>(values->size() - 1));
            const auto first_index = std::min(start, end);
            const auto last_index = std::max(start, end);
            const auto wanted = as_integer(arguments[1]);
            for (auto index = first_index; index <= last_index; ++index) {
                if ((*values)[static_cast<std::size_t>(index)] == wanted)
                    return wrap_integer(index);
            }
            return std::int64_t{ -1 };
        }
        if (name == "array_equal") {
            if (arguments.size() != 2)
                return std::int64_t{ 0 };
            const auto left =
                    std::get_if<std::vector<std::int64_t>>(&arguments[0]);
            const auto right =
                    std::get_if<std::vector<std::int64_t>>(&arguments[1]);
            if (left && right && left->size() != right->size()) {
                warn(expression.location,
                        "array_equal(): Array sizes don't match!");
            }
            return std::int64_t{ left && right && *left == *right };
        }
        if (name == "random") {
            auto lower = static_cast<std::int32_t>(as_integer(arguments.at(0)));
            auto upper = static_cast<std::int32_t>(as_integer(arguments.at(1)));
            return wrap_integer(host ? host->random_integer(lower, upper)
                                     : random_integer(lower, upper));
        }
        if (name == "in_range") {
            const bool uses_reals = std::any_of(
                    arguments.begin(), arguments.end(), [](const Value &value) {
                        return std::holds_alternative<double>(value);
                    });
            if (uses_reals) {
                const auto value = as_real(arguments.at(0));
                return std::int64_t{ as_real(arguments.at(1)) <= value &&
                                     value <= as_real(arguments.at(2)) };
            }
            const auto value =
                    static_cast<std::int32_t>(as_integer(arguments.at(0)));
            const auto lower =
                    static_cast<std::int32_t>(as_integer(arguments.at(1)));
            const auto upper =
                    static_cast<std::int32_t>(as_integer(arguments.at(2)));
            return std::int64_t{ lower <= value && value <= upper };
        }
        if (name == "by_marks") {
            const auto marks = static_cast<std::uint32_t>(as_integer(first()));
            return wrap_integer(marks | 0x80000000U);
        }
        if (name == "by_track")
            return wrap_integer(-as_integer(first()) - 2);

        if (name == "abs") {
            if (std::holds_alternative<double>(first()))
                return std::abs(first_real());
            const auto value = as_integer(first());
            return wrap_integer(value < 0 ? -value : value);
        }
        if (name == "signbit") {
            if (std::holds_alternative<double>(first()))
                return std::signbit(first_real()) ? 1.0 : 0.0;
            return std::int64_t{ as_integer(first()) < 0 };
        }
        if (name == "sgn") {
            if (std::holds_alternative<double>(first())) {
                const auto value = first_real();
                return static_cast<double>((value > 0.0) - (value < 0.0));
            }
            const auto value = as_integer(first());
            return std::int64_t{ (value > 0) - (value < 0) };
        }
        if (name == "lsb")
            return std::int64_t{ as_integer(first()) & 0x7f };
        if (name == "msb")
            return std::int64_t{ (as_integer(first()) >> 7) & 0x7f };
        if (name == "real_to_int" || name == "int") {
            const auto value = first_real();
            if (std::isnan(value))
                return std::int64_t{ 42 };

            auto converted = std::int64_t{};
            if (value <= static_cast<double>(
                                 std::numeric_limits<std::int32_t>::min()))
                converted = std::numeric_limits<std::int32_t>::min();
            else if (value >= static_cast<double>(
                                      std::numeric_limits<std::int32_t>::max()))
                converted = std::numeric_limits<std::int32_t>::max();
            else
                converted = static_cast<std::int32_t>(value);

            if (std::isinf(value)) {
                warn(expression.location,
                        "real_to_int(): Clipped " + as_string(Value{ value }) +
                                " to " + std::to_string(converted) + "!");
            }
            return converted;
        }
        if (name == "int_to_real" || name == "real")
            return static_cast<double>(
                    static_cast<std::int32_t>(as_integer(first())));
        if (name == "exp")
            return std::exp(first_real());
        if (name == "exp2")
            return std::exp2(first_real());
        if (name == "log")
            return std::log(first_real());
        if (name == "log2")
            return std::log2(first_real());
        if (name == "log10")
            return std::log10(first_real());
        if (name == "sqrt")
            return std::sqrt(first_real());
        if (name == "cbrt")
            return std::cbrt(first_real());
        if (name == "sin")
            return std::sin(first_real());
        if (name == "cos")
            return std::cos(first_real());
        if (name == "tan")
            return std::tan(first_real());
        if (name == "asin")
            return std::asin(first_real());
        if (name == "acos")
            return std::acos(first_real());
        if (name == "atan")
            return std::atan(first_real());
        if (name == "pow")
            return std::pow(first_real(), as_real(arguments.at(1)));
        if (name == "floor")
            return std::floor(first_real());
        if (name == "ceil")
            return std::ceil(first_real());
        if (name == "round")
            return std::round(first_real());
        if (name == "sh_left")
            return wrap_integer(static_cast<std::uint32_t>(as_integer(first()))
                                << (static_cast<std::uint32_t>(
                                            as_integer(arguments.at(1))) &
                                           31));
        if (name == "sh_right")
            return wrap_integer(
                    static_cast<std::int32_t>(as_integer(first())) >>
                    (static_cast<std::uint32_t>(as_integer(arguments.at(1))) &
                            31));
        if ((name == "ms_to_ticks" || name == "ticks_to_ms") && host) {
            if (const auto tempo = host->current_tempo_bpm()) {
                const auto input = static_cast<float>(
                        static_cast<std::int32_t>(as_integer(first())));
                const auto current_tempo = static_cast<float>(*tempo);
                const auto converted =
                        name == "ms_to_ticks"
                                ? current_tempo * input / 62500.0F
                                : input * 62500.0F / current_tempo;
                return wrap_integer(truncate_float_to_int32(converted + 0.5F));
            }
        }
        if (name == "get_engine_par" || name == "get_engine_par_disp" ||
                name == "get_engine_par_disp_ext" || name == "set_engine_par") {
            const auto parameter = as_integer(arguments[0]);
            if (!detail::is_valid_engine_parameter_index(parameter)) {
                if (name == "get_engine_par" || name == "set_engine_par")
                    return std::int64_t{ 0 };
                return std::string{};
            }
            const auto group_argument =
                    name == "set_engine_par" ||
                                    name == "get_engine_par_disp_ext"
                            ? 2u
                            : 1u;
            const auto group = as_integer(arguments[group_argument]);
            if (host) {
                if (const auto count = host->group_count();
                        count && group >= *count) {
                    const auto warning_name = name == "get_engine_par_disp_ext"
                                                      ? "get_engine_par_disp"
                                                      : name;
                    warn(expression.location,
                            std::string(warning_name) +
                                    "(): Invalid group index!");
                    if (name == "get_engine_par")
                        return std::int64_t{ 0 };
                    if (name == "set_engine_par")
                        return std::int64_t{ -1 };
                    return std::string{};
                }

                if (name == "set_engine_par") {
                    const bool synchronous = current_callback_type == 5;
                    const auto async_id =
                            start_engine_command(name, arguments, synchronous);
                    if (synchronous)
                        return std::int64_t{ -1 };
                    if (async_id)
                        return wrap_integer(*async_id);
                    return std::int64_t{ -1 };
                }
                const auto result = host->call(name, arguments);
                if (result) {
                    if (name == "get_engine_par")
                        return wrap_integer(as_integer(*result));
                    return as_string(*result);
                }
            }
            if (name == "get_engine_par")
                return std::int64_t{ 0 };
            if (name == "set_engine_par")
                return std::int64_t{ -1 };
            return std::string{};
        }
        if (name == "get_engine_par_m" || name == "get_engine_par_disp_m") {
            const auto parameter = as_integer(arguments[0]);
            if (!detail::is_valid_engine_parameter_index(parameter)) {
                if (name == "get_engine_par_m")
                    return std::int64_t{ 0 };
                return std::string{};
            }
            if (host) {
                if (const auto result = host->call(name, arguments)) {
                    if (name == "get_engine_par_m")
                        return wrap_integer(as_integer(*result));
                    return as_string(*result);
                }
            }
            if (name == "get_engine_par_m")
                return std::int64_t{ 0 };
            return std::string{};
        }
        if (name == "load_ir_sample" && arguments.size() == 3) {
            arguments[0] = as_string(arguments[0]);
            auto selector = as_integer(arguments[2]);
            if (selector == 2)
                selector = 20;
            else if (selector >= 0x400 && selector <= 0x40f)
                selector -= 0x3fd;
            arguments[2] = selector;
            const bool synchronous = current_callback_type == 5;
            const auto async_id =
                    start_engine_command(name, arguments, synchronous);
            if (synchronous)
                return std::int64_t{ -1 };
            if (async_id)
                return wrap_integer(*async_id);
            return std::int64_t{ -1 };
        }
        if ((name == "load_midi_file" || name == "save_midi_file") &&
                arguments.size() == 1) {
            arguments[0] = as_string(arguments[0]);
            const bool synchronous =
                    name == "load_midi_file" && current_callback_type == 5;
            const auto async_id =
                    start_engine_command(name, arguments, synchronous);
            if (synchronous)
                return std::int64_t{ -1 };
            if (async_id)
                return wrap_integer(*async_id);
            return std::int64_t{ -1 };
        }
        if (name == "get_font_id" && arguments.size() == 1)
            arguments[0] = as_string(arguments[0]);
        if ((name == "get_control_par" || name == "get_control_par_arr") &&
                (arguments.size() == 2 || arguments.size() == 3)) {
            const auto control_id = as_integer(arguments[0]);
            const auto parameter = as_integer(arguments[1]);
            if (control_id >= 0x4001 && control_id <= 0x4007 &&
                    (control_id != 0x4007 ||
                            (parameter != 3 && parameter != 4))) {
                return std::int64_t{ 0 };
            }
            if (!ui_control_is_declared(as_integer(arguments[0]))) {
                warn(expression.location,
                        "get_control_par(): Control ID was not declared!");
                return std::int64_t{ 0 };
            }
            if (parameter < 0 || parameter > 118) {
                warn(expression.location,
                        "get_control_par(): Invalid control parameter!");
                return std::int64_t{ 0 };
            }
            use_named_ui_control(arguments);
            if (host)
                if (const auto result = host->call(name, arguments))
                    return wrap_integer(as_integer(*result));
            return std::int64_t{ 0 };
        }
        if ((name == "get_control_par_str" ||
                    name == "get_control_par_str_arr") &&
                (arguments.size() == 2 || arguments.size() == 3)) {
            const auto control_id = as_integer(arguments[0]);
            const auto parameter = as_integer(arguments[1]);
            if (control_id >= 0x4001 && control_id <= 0x4007) {
                if (current_callback_type != 5) {
                    warn(expression.location,
                            "get_control_par() for pseudo-variable identifiers "
                            "can only be used in \"on init\" callback!");
                    return std::string{};
                }
                if (control_id != 0x4001 || parameter != 14)
                    return std::string{};
            }
            if (!ui_control_is_declared(as_integer(arguments[0]))) {
                warn(expression.location,
                        "get_control_par_str(): Control ID was not declared!");
                return std::string{};
            }
            if (parameter < 0 || parameter > 118) {
                warn(expression.location,
                        "get_control_par_str(): Invalid control parameter!");
                return std::string{};
            }
            if (parameter == 28 && current_callback_type != 5) {
                warn(expression.location, "$CONTROL_PAR_HELP can only be used "
                                          "in \"on init\" callback!");
                return std::string{};
            }
            use_named_ui_control(arguments);
            if (host)
                if (const auto result = host->call(name, arguments))
                    return as_string(*result);
            return std::string{};
        }
        if (name == "get_control_par_real_arr" && arguments.size() == 3) {
            const auto control_id = as_integer(arguments[0]);
            if (control_id >= 0x4001 && control_id <= 0x4007) {
                warn(expression.location,
                        "get_control_par_real_arr(): This command cannot be used "
                        "with internal control IDs!");
                return 0.0;
            }
            if (as_integer(arguments[1]) != 12) {
                warn(expression.location,
                        "get_control_par_real_arr(): Invalid control parameter! "
                        "Only $CONTROL_PAR_VALUE can be used with this command.");
                return 0.0;
            }
            if (!ui_control_is_declared(control_id)) {
                warn(expression.location,
                        "get_control_par_real_arr(): Control ID was not declared!");
                return 0.0;
            }
            use_named_ui_control(arguments);
            if (host)
                if (const auto result = host->call(name, arguments))
                    return as_real(*result);
            return 0.0;
        }
        if ((name == "get_menu_item_value" ||
                    name == "get_menu_item_visibility" ||
                    name == "get_menu_item_str") &&
                arguments.size() == 2) {
            const auto control_type =
                    resolved_ui_control_type(as_integer(arguments[0]));
            if (!control_type) {
                warn(expression.location,
                        std::string(name) + "(): Control ID was not declared!");
                return name == "get_menu_item_str" ? Value{ std::string{} }
                                                   : Value{ std::int64_t{ 0 } };
            }
            if (*control_type != "ui_menu") {
                warn(expression.location,
                        std::string(name) +
                                "(): Control ID does not belong to a ui_menu!");
                return name == "get_menu_item_str" ? Value{ std::string{} }
                                                   : Value{ std::int64_t{ 0 } };
            }
            if (const auto count = ui_menu_item_count(arguments[0])) {
                const auto index = as_integer(arguments[1]);
                if (index < 0 || index >= *count) {
                    warn(expression.location,
                            std::string(name) +
                                    "(): Menu item index is out of range!");
                    return name == "get_menu_item_str"
                                   ? Value{ std::string{} }
                                   : Value{ std::int64_t{ 0 } };
                }
            }
            use_named_ui_control(arguments);
            if (host)
                if (const auto result = host->call(name, arguments)) {
                    if (name == "get_menu_item_str")
                        return as_string(*result);
                    return wrap_integer(as_integer(*result));
                }
            return name == "get_menu_item_str" ? Value{ std::string{} }
                                               : Value{ std::int64_t{ 0 } };
        }
        if (name == "get_ui_wf_property" && arguments.size() == 3) {
            const auto property = as_integer(arguments[1]);
            if (property < 1 || property > 10) {
                warn(expression.location,
                        "get_wf_ui_property(): Invalid ui_waveform property!");
                return std::int64_t{ 0 };
            }
            const auto index = as_integer(arguments[2]);
            if (property == 3 && static_cast<std::uint64_t>(index) >= 256) {
                warn(expression.location, "get_wf_ui_property(): ui_waveform "
                                          "table index is out of range!");
                return std::int64_t{ 0 };
            }
            if (!expression.children.empty() &&
                    expression.children[0].kind == Expression::Kind::name)
                arguments[0] = expression.children[0].text;
            if (host) {
                if (const auto result = host->call(name, arguments)) {
                    auto value =
                            static_cast<std::uint32_t>(as_integer(*result));
                    if (property == 7)
                        value |= UINT32_C(0xff000000);
                    else if (property == 8)
                        value &= UINT32_C(0x000000ff);
                    else if (property == 6 || property == 9 || property == 10)
                        value &= UINT32_C(0x00ffffff);
                    return wrap_integer(value);
                }
            }
            return std::int64_t{ 0 };
        }
        if (name == "mf_get_last_filename" && arguments.empty()) {
            if (host)
                if (const auto result = host->call(name, arguments))
                    return as_string(*result);
            return std::string{};
        }
        static const std::unordered_set<std::string> midi_integer_queries = {
            "mf_get_num_tracks", "mf_get_buffer_size", "mf_get_channel",
            "mf_get_command", "mf_get_byte_one", "mf_get_byte_two",
            "mf_get_event_par", "mf_get_pos", "mf_get_track_idx", "mf_get_id",
            "mf_get_mark", "mf_get_note_length", "mf_get_length"
        };
        if (midi_integer_queries.contains(name)) {
            if (host)
                if (const auto result = host->call(name, arguments))
                    return wrap_integer(as_integer(*result));
            return std::int64_t{ 0 };
        }
        if (name == "mf_reset" || name == "mf_set_buffer_size") {
            const bool synchronous = current_callback_type == 5;
            const auto async_id =
                    start_engine_command(name, arguments, synchronous);
            if (synchronous)
                return std::int64_t{ 0 };
            return async_id ? wrap_integer(*async_id) : std::int64_t{ 0 };
        }
        if (name == "mf_insert_file" && arguments.size() == 4) {
            arguments[0] = as_string(arguments[0]);
            const auto mode = as_integer(arguments[3]);
            if (mode < 0 || mode > 2) {
                warn(expression.location,
                        "mf_insert_file(): Invalid MIDI file insert mode!");
                arguments[1] = std::int64_t{ 0 };
            }
            const bool synchronous = current_callback_type == 5;
            const auto async_id =
                    start_engine_command(name, arguments, synchronous);
            if (synchronous)
                return std::int64_t{ -1 };
            return async_id ? wrap_integer(*async_id) : std::int64_t{ -1 };
        }
        if (name == "pgs_key_exists" || name == "pgs_str_key_exists" ||
                name == "pgs_get_key_val" || name == "pgs_get_str_key_val") {
            if (host) {
                if (const auto returned = host->call(name, arguments)) {
                    if (name == "pgs_get_str_key_val")
                        return as_string(*returned);
                    return wrap_integer(as_integer(*returned));
                }
            }
            if (name == "pgs_get_str_key_val")
                return std::string{};
            return std::int64_t{ 0 };
        }
        if ((name == "load_array" || name == "save_array" ||
                    name == "load_array_str" || name == "save_array_str") &&
                arguments.size() == 2 &&
                expression.children[0].kind == Expression::Kind::name) {
            const auto &target = expression.children[0].text;
            const auto spelling = declared_variable_spellings.find(target);
            const auto &file_target =
                    spelling == declared_variable_spellings.end()
                            ? target
                            : spelling->second;
            const auto source =
                    name.ends_with("_str")
                            ? Value{ as_string(arguments[1]) }
                            : Value{ wrap_integer(as_integer(arguments[1])) };
            if (name == "save_array") {
                const auto mode = as_integer(source);
                if ((mode != 0 && mode != 1) ||
                        (mode == 0 && current_callback_type == 14)) {
                    warn(expression.location,
                            "save_array(): Mode must be 0 or 1!");
                    return std::int64_t{ -1 };
                }
            }
            if (name == "load_array") {
                const auto mode = as_integer(source);
                if (current_callback_type == 5 && (mode != 1 && mode != 2)) {
                    warn(expression.location,
                            "load_array(): Only modes 1 and 2 are allowed in \"on "
                            "init\" callback!");
                    return std::int64_t{ -1 };
                }
                if (current_callback_type != 5 &&
                        ((mode < 0 || mode > 2) ||
                                (mode == 0 && current_callback_type == 14))) {
                    warn(expression.location,
                            "load_array(): Mode must be between 0 and 2");
                    return std::int64_t{ -1 };
                }
                // restore persistence first, the file can overwrite it, and
                // if it's missing the value waits for the next init statement
                if (current_callback_type == 5 && (mode == 1 || mode == 2))
                    (void)apply_pending_persistent_value(file_target);
            }
            if (!host)
                return std::int64_t{ -1 };

            const bool loading = name.starts_with("load_");
            if (!loading) {
                if (const auto id = host->save_array_file(
                            file_target, arguments[0], source)) {
                    if (*id >= 0)
                        pending_array_saves.insert(*id);
                    return wrap_integer(*id);
                }
                return std::int64_t{ -1 };
            }

            if (const auto result = host->load_array_file(file_target,
                        evaluate(expression.children[0]), source,
                        current_callback_type == 5)) {
                const bool value_applied = result->loaded_value.has_value();
                if (value_applied)
                    assign_persistent_value(target, *result->loaded_value);
                if (result->async_id >= 0)
                    pending_array_loads[result->async_id] = { target,
                        value_applied };
                return wrap_integer(result->async_id);
            }
            return std::int64_t{ -1 };
        }
        if (name == "num_groups" && arguments.empty()) {
            if (host)
                if (const auto count = host->group_count())
                    return *count;
            return std::int64_t{ 0 };
        }
        if (name == "get_num_zones" && arguments.empty()) {
            if (host)
                if (const auto count = host->zone_count())
                    return *count;
            if (!host)
                return std::int64_t{ 0 };
        }
        if (name == "get_zone_id" && arguments.size() == 1) {
            const auto index = as_integer(arguments.front());
            if (index < 0)
                return std::int64_t{ -1 };
            if (host) {
                if (const auto count = host->zone_count();
                        count && index >= *count)
                    return std::int64_t{ -1 };
            }
        }
        if (name == "group_name" && arguments.size() == 1) {
            const auto group = as_integer(arguments.front());
            if (group < 0)
                return std::string{};
            if (host) {
                if (const auto count = host->group_count();
                        count && group >= *count)
                    return std::string{};
            }
        }
        if ((name == "get_purge_state" || name == "purge_group") &&
                !arguments.empty()) {
            const auto group = as_integer(arguments.front());
            if (group < 0)
                return name == "get_purge_state" ? std::int64_t{ 1 }
                                                 : std::int64_t{ -1 };
            if (host) {
                if (const auto count = host->group_count();
                        count && group >= *count) {
                    return name == "get_purge_state" ? std::int64_t{ 1 }
                                                     : std::int64_t{ -1 };
                }
                if (const auto result = host->call(name, arguments)) {
                    const auto returned_id = wrap_integer(as_integer(*result));
                    if (name != "purge_group" || !active_state)
                        return returned_id;
                    if (active_state->purge_async_id == -1) {
                        active_state->purge_async_id = returned_id;
                        if (returned_id >= 0)
                            pending_engine_commands.insert(returned_id);
                    }
                    return active_state->purge_async_id;
                }
            }
            return name == "get_purge_state" ? std::int64_t{ 1 }
                                             : std::int64_t{ -1 };
        }
        if ((name == "get_key_name" || name == "get_keyrange_name") &&
                arguments.size() == 1) {
            const auto note = as_integer(arguments.front());
            if (note < 0 || note > 127)
                return std::string{};
        }
        if (name == "output_channel_name" && arguments.size() == 1) {
            const auto channel = as_integer(arguments.front());
            if (channel < 0 || channel > 127) {
                warn(expression.location, "output_channel_name(): Output "
                                          "channel index out of range!");
                return std::string{};
            }
        }
        if (name == "get_folder" && arguments.size() == 1) {
            const auto folder = as_integer(arguments.front());
            if (folder < 0 || folder > 4)
                return std::string{};
        }
        if (name == "get_group_par" && arguments.size() == 2) {
            const auto group = as_integer(arguments[0]);
            const auto parameter = as_integer(arguments[1]);
            const auto invalid_group_result = [&] {
                if (parameter == 0)
                    return std::int64_t{ 20 };
                if (parameter == 3)
                    return std::int64_t{ 0 };
                return std::int64_t{ -1 };
            };
            if (parameter < 0 || parameter > 3)
                return std::int64_t{ -1 };
            if (group < 0)
                return invalid_group_result();
            if (host) {
                if (const auto count = host->group_count();
                        count && group >= *count)
                    return invalid_group_result();
            }
        }
        if (name == "get_nks_nav_par" && arguments.size() == 2) {
            if (as_integer(arguments[0]) < 0 || as_integer(arguments[1]) < 0)
                return std::int64_t{ -1 };
        }
        if (name == "get_nks_nav_name" && arguments.size() == 2) {
            if ((host && host->is_multi_script()) ||
                    as_integer(arguments[0]) < 0 ||
                    as_integer(arguments[1]) < 0)
                return std::string{};
        }
        if ((name == "get_keyrange_min_note" ||
                    name == "get_keyrange_max_note") &&
                arguments.size() == 1) {
            const auto note = as_integer(arguments.front());
            if (note < 0 || note > 127) {
                warn(expression.location,
                        std::string(name) +
                                "(): Note number must be between 0 or 127!");
                return std::int64_t{ -1 };
            }
        }
        if (name == "get_voice_limit" && arguments.size() == 1) {
            const auto limit_type = as_integer(arguments.front());
            if (limit_type < 0 || limit_type > 1) {
                warn(expression.location,
                        "get_voice_limit(): Voice limit type must be 0 or 1!");
                return std::int64_t{ -1 };
            }
        }
        if (name == "get_sample" && !arguments.empty()) {
            const auto zone = as_integer(arguments[0]);
            if (host) {
                if (const auto exists = host->zone_exists(zone);
                        exists && !*exists) {
                    warn(expression.location, "get_sample(): Zone ID " +
                                                      std::to_string(zone) +
                                                      " was not found!");
                    return std::string{};
                }
            }

            if (arguments.size() == 1)
                arguments.emplace_back(std::int64_t{ 1 });
            else {
                const auto selector = as_integer(arguments[1]);
                if (selector != 0 && selector != 2 && selector != 3)
                    arguments[1] = std::int64_t{ 1 };
            }
            if (host) {
                if (const auto value = host->call(name, arguments))
                    return as_string(*value);
            }
            return std::string{};
        }
        if (name == "set_sample" && arguments.size() == 2) {
            const auto zone = as_integer(arguments[0]);
            arguments[1] = as_string(arguments[1]);
            if (host) {
                if (const auto exists = host->zone_exists(zone);
                        exists && !*exists) {
                    warn(expression.location, "set_sample(): Zone ID " +
                                                      std::to_string(zone) +
                                                      " was not found!");
                    return std::int64_t{ -1 };
                }
                if (const auto user_zone = host->zone_is_user(zone);
                        user_zone && !*user_zone) {
                    warn(expression.location,
                            "set_sample() can only be used with user zones!");
                    return std::int64_t{ -1 };
                }
                const bool synchronous = current_callback_type == 5;
                const auto async_id =
                        start_engine_command(name, arguments, synchronous);
                if (synchronous)
                    return std::int64_t{ -1 };
                if (async_id)
                    return wrap_integer(*async_id);
            }
            return std::int64_t{ -1 };
        }
        if ((name == "get_zone_par" || name == "get_loop_par") && host) {
            const auto zone = as_integer(arguments[0]);
            if (const auto exists = host->zone_exists(zone);
                    exists && !*exists) {
                warn(expression.location, std::string(name) + "(): Zone ID " +
                                                  std::to_string(zone) +
                                                  " was not found!");
                return std::int64_t{ -1 };
            }
        }
        if ((name == "is_zone_empty" || name == "get_zone_status") && host) {
            const auto zone = as_integer(arguments[0]);
            if (const auto exists = host->zone_exists(zone);
                    exists && !*exists) {
                warn(expression.location, std::string(name) + "(): Zone ID " +
                                                  std::to_string(zone) +
                                                  " was not found!");
                return name == "is_zone_empty" ? std::int64_t{ 1 }
                                               : std::int64_t{ -1 };
            }
        }
        if (name == "get_loop_par" && arguments.size() == 3) {
            const auto loop = as_integer(arguments[1]);
            if (loop < 0 || loop > 7) {
                warn(expression.location,
                        "get_loop_par(): Loop index must be between 0 and 7!");
                return std::int64_t{ -1 };
            }
            const auto parameter = as_integer(arguments[2]);
            if (parameter < 0x208 || parameter > 0x20d) {
                warn(expression.location,
                        "get_loop_par(): Invalid loop parameter!");
                return std::int64_t{ 0 };
            }
        }
        if ((name == "find_group" || name == "get_group_idx" ||
                    name == "find_zone") &&
                arguments.size() == 1) {
            arguments.front() = as_string(arguments.front());
        }
        if ((name == "find_mod" || name == "find_target" ||
                    name == "get_mod_idx" || name == "get_target_idx") &&
                !arguments.empty()) {
            arguments.back() = as_string(arguments.back());
        }
        if (name == "change_time_with_pitch" && arguments.size() == 2) {
            const auto event_id = as_integer(arguments[0]);
            const auto time = wrap_integer(as_integer(arguments[1]));
            if (event_id < 0) {
                warn(expression.location, "change_time_with_pitch(): Multiple "
                                          "event adressing is not allowed!");
                return time;
            }
            if (host) {
                if (const auto result = host->call(name, arguments))
                    return wrap_integer(as_integer(*result));
            }
            warn(expression.location, "change_time_with_pitch(): No voice "
                                      "available (event not yet dispatched)!");
            return time;
        }
        if (normalize_engine_arguments(name, arguments, expression.location) ==
                HostCallDisposition::skip)
            return std::int64_t{ 0 };
        if (host)
            if (auto value = host->call(name, arguments)) {
                refresh_control_value_after_set(name, arguments);
                if ((name == "find_group" || name == "get_group_idx") &&
                        as_integer(*value) < 0) {
                    warn(expression.location,
                            std::string(name) + "(): Group \"" +
                                    as_string(arguments.front()) +
                                    "\" was not found. Index will be invalid!");
                    return name == "find_group" ? std::int64_t{ 0 }
                                                : std::int64_t{ -100 };
                }
                if (name == "find_zone" && as_integer(*value) < 0) {
                    warn(expression.location,
                            "find_zone(): Zone \"" +
                                    as_string(arguments.front()) +
                                    "\" was not found! Zone ID will be invalid.");
                }
                if ((name == "find_mod" || name == "find_target" ||
                            name == "get_mod_idx" ||
                            name == "get_target_idx") &&
                        as_integer(*value) < 0) {
                    warn(expression.location,
                            std::string(name) + "(): Object \"" +
                                    as_string(arguments.back()) +
                                    "\" was not found. Index will be invalid!");
                    return name == "find_mod" || name == "find_target"
                                   ? std::int64_t{ 0 }
                                   : std::int64_t{ -100 };
                }
                if (name == "get_zone_par" && arguments.size() == 2) {
                    const auto parameter = as_integer(arguments[1]);
                    const auto raw = as_real(*value);
                    if (parameter == 10) {
                        if (raw < 1.0e-9)
                            return std::int64_t{ 0 };
                        return wrap_integer(std::log2(raw) * 600.0);
                    }
                    if (parameter == 11)
                        return wrap_integer(raw * 1000.0);
                    if (parameter == 12)
                        return wrap_integer(std::log2(raw) * 1200.0);
                }
                if (name == "get_loop_par" && arguments.size() == 3 &&
                        as_integer(arguments[2]) == 0x20d) {
                    return wrap_integer(std::log2(as_real(*value)) * 1200.0);
                }
                return *value;
            }
        return std::int64_t{ 0 };
    }

    Value evaluate(const Expression &expression) {
        if (expression.kind == Expression::Kind::literal)
            return expression.value;
        if (expression.kind == Expression::Kind::name) {
            if (expression.source_name == "$NI_CALLBACK_ID")
                return current_callback_id;
            if (expression.source_name == "$NI_CALLBACK_TYPE")
                return current_callback_type;
            if (expression.source_name == "$NI_ASYNC_ID")
                return current_async_id;
            if (expression.source_name == "$NI_ASYNC_TYPE")
                return current_async_type;
            if (expression.source_name == "$NI_ASYNC_PAR_1")
                return current_async_parameter_one;
            if (expression.source_name == "$NI_ASYNC_PAR_2")
                return current_async_parameter_two;
            if (expression.source_name == "$NI_ASYNC_EXIT_STATUS")
                return current_async_exit_status;
            if (expression.source_name == "$NI_ASYNC_VALUE")
                return wrap_integer(as_integer(current_async_value));
            if (expression.source_name == "@NI_ASYNC_STR_VALUE")
                return as_string(current_async_value);
            if (const auto constant = detail::lookup_builtin_integer_constant(
                        expression.source_name)) {
                if (!expression.children.empty()) {
                    diagnostics.push_back({ expression.location,
                            "cannot index a built-in constant" });
                }
                return *constant;
            }
            if (const auto constant = detail::lookup_builtin_real_constant(
                        expression.source_name)) {
                if (!expression.children.empty()) {
                    diagnostics.push_back({ expression.location,
                            "cannot index a built-in constant" });
                }
                return *constant;
            }
            // check the exact builtin spelling first, otherwise a declared
            // $event_note would hijack $EVENT_NOTE
            const bool builtin =
                    detail::is_builtin_variable(expression.source_name);
            if (!builtin && polyphonic_defaults.contains(expression.text)) {
                if (!expression.children.empty()) {
                    diagnostics.push_back({ expression.location,
                            "cannot index a polyphonic variable" });
                    return std::int64_t{ 0 };
                }
                return read_polyphonic(expression.text);
            }
            auto variable = variables.find(expression.text);
            if (builtin || variable == variables.end()) {
                if (const auto symbol = resolve_environment_symbol(
                            expression.text, expression.source_name)) {
                    if (expression.children.empty())
                        return checked_value(
                                symbol->value, expression.location);
                    const auto index =
                            as_integer(evaluate(expression.children[0]));
                    return checked_value(read_array_element(symbol->value,
                                                 index, expression.location),
                            expression.location);
                }
                diagnostics.push_back({ expression.location,
                        "unknown variable " + expression.text });
                return std::int64_t{ 0 };
            }
            if (expression.children.empty())
                return checked_value(variable->second, expression.location);
            const auto index = as_integer(evaluate(expression.children[0]));
            return checked_value(read_array_element(variable->second, index,
                                         expression.location),
                    expression.location);
        }
        if (expression.kind == Expression::Kind::unary) {
            auto operand = evaluate(expression.children[0]);
            auto op = lowercase(expression.text);
            if (op == "not")
                return std::int64_t{ !as_boolean(operand) };
            if (op == ".not.")
                return std::int64_t{ ~static_cast<std::int32_t>(
                        as_integer(operand)) };
            if (expression.text == "-") {
                if (std::holds_alternative<double>(operand))
                    return checked_real_result(
                            -std::get<double>(operand), expression.location);
                return wrap_integer(-as_integer(operand));
            }
            return operand;
        }
        if (expression.kind == Expression::Kind::call) {
            auto result = evaluate_call(expression);
            if (const auto real = std::get_if<double>(&result))
                return checked_real_result(*real, expression.location);
            return result;
        }
        const auto operation = lowercase(expression.text);
        if (operation == "and") {
            const auto left = evaluate(expression.children[0]);
            return std::int64_t{ as_boolean(left) &&
                                 as_boolean(evaluate(expression.children[1])) };
        }
        if (operation == "or") {
            const auto left = evaluate(expression.children[0]);
            return std::int64_t{ as_boolean(left) ||
                                 as_boolean(evaluate(expression.children[1])) };
        }
        if (operation == "xor") {
            const auto left = as_boolean(evaluate(expression.children[0]));
            const auto right = as_boolean(evaluate(expression.children[1]));
            return std::int64_t{ left != right };
        }

        auto left_value = evaluate(expression.children[0]);
        auto right_value = evaluate(expression.children[1]);
        if (expression.text == "&")
            return as_string(left_value) + as_string(right_value);

        const bool uses_strings =
                std::holds_alternative<std::string>(left_value) ||
                std::holds_alternative<std::string>(right_value);
        const bool uses_reals = std::holds_alternative<double>(left_value) ||
                                std::holds_alternative<double>(right_value);
        if (expression.text == "=" || expression.text == "#") {
            const bool equal =
                    uses_strings
                            ? as_string(left_value) == as_string(right_value)
                    : uses_reals
                            ? real_equal(
                                      as_real(left_value), as_real(right_value))
                            : as_integer(left_value) == as_integer(right_value);
            return std::int64_t{ expression.text == "=" ? equal : !equal };
        }
        if (expression.text == "<")
            return std::int64_t{ as_real(left_value) < as_real(right_value) };
        if (expression.text == "<=")
            return std::int64_t{ as_real(left_value) <= as_real(right_value) };
        if (expression.text == ">")
            return std::int64_t{ as_real(left_value) > as_real(right_value) };
        if (expression.text == ">=")
            return std::int64_t{ as_real(left_value) >= as_real(right_value) };
        auto op = lowercase(expression.text);
        if (op.size() > 2 && op.front() == '.' && op.back() == '.')
            op = op.substr(1, op.size() - 2);
        auto left_integer = static_cast<std::int32_t>(as_integer(left_value)),
             right_integer = static_cast<std::int32_t>(as_integer(right_value));
        if (op == "and")
            return wrap_integer(left_integer & right_integer);
        if (op == "or")
            return wrap_integer(left_integer | right_integer);
        if (op == "xor")
            return wrap_integer(left_integer ^ right_integer);
        if (op == "sh_left")
            return wrap_integer(
                    static_cast<std::uint32_t>(left_integer)
                    << (static_cast<std::uint32_t>(right_integer) & 31));
        if (op == "sh_right")
            return wrap_integer(
                    left_integer >>
                    (static_cast<std::uint32_t>(right_integer) & 31));
        if (op == "mod") {
            if (uses_reals) {
                return checked_real_result(
                        std::fmod(as_real(left_value), as_real(right_value)),
                        expression.location);
            }
            if (right_integer == 0)
                return std::int64_t{ 0 };
            return wrap_integer(left_integer % right_integer);
        }
        const bool use_real_arithmetic = uses_reals;
        if (expression.text == "+")
            return use_real_arithmetic
                           ? Value{ checked_real_result(
                                     as_real(left_value) + as_real(right_value),
                                     expression.location) }
                           : Value{ wrap_integer(
                                     static_cast<std::int64_t>(left_integer) +
                                     right_integer) };
        if (expression.text == "-")
            return use_real_arithmetic
                           ? Value{ checked_real_result(
                                     as_real(left_value) - as_real(right_value),
                                     expression.location) }
                           : Value{ wrap_integer(
                                     static_cast<std::int64_t>(left_integer) -
                                     right_integer) };
        if (expression.text == "*")
            return use_real_arithmetic
                           ? Value{ checked_real_result(
                                     as_real(left_value) * as_real(right_value),
                                     expression.location) }
                           : Value{ wrap_integer(
                                     static_cast<std::int64_t>(left_integer) *
                                     right_integer) };
        if (expression.text == "/") {
            if (use_real_arithmetic) {
                return checked_real_result(
                        as_real(left_value) / as_real(right_value),
                        expression.location);
            }
            if (right_integer == 0)
                return std::int64_t{ 0 };
            return wrap_integer(left_integer / right_integer);
        }
        return std::int64_t{ 0 };
    }
    std::int64_t allocate_declared_variable(const Statement &statement) {
        if (const auto existing = script_variable_ids.find(statement.name);
                existing != script_variable_ids.end()) {
            return existing->second;
        }

        const auto variable_id = 0x8000 + next_variable_id++;
        script_variable_ids[statement.name] = variable_id;
        if (statement.is_ui_control) {
            ui_control_ids[statement.name] = variable_id;
            ui_control_names[variable_id] = statement.name;
            ui_control_types[variable_id] = statement.ui_type;
        }
        return variable_id;
    }

    void execute_declaration(const Statement &statement) {
        const auto variable_id = allocate_declared_variable(statement);

        std::optional<std::int64_t> array_size;
        if (statement.index) {
            auto evaluated_size = as_integer(evaluate(*statement.index));
            if (!statement.is_ui_control && evaluated_size < 1) {
                diagnostics.push_back({ statement.index->location,
                        "array size must be at least 1" });
                evaluated_size = 1;
            } else if (!statement.is_ui_control && evaluated_size > 1'000'000) {
                const auto array_type =
                        statement.name.front() == '%'   ? "Integer"
                        : statement.name.front() == '?' ? "Real"
                                                        : "String";
                diagnostics.push_back({ statement.index->location,
                        std::string(array_type) +
                                " array size cannot be larger than 1000000!" });
                evaluated_size = 1'000'000;
            }
            array_size = std::max<std::int64_t>(0, evaluated_size);
        }

        std::vector<std::int64_t> ui_arguments;
        ui_arguments.reserve(statement.ui_arguments.size());
        for (const auto &argument : statement.ui_arguments)
            ui_arguments.push_back(as_integer(evaluate(argument)));

        if (statement.ui_type == "ui_label" ||
                statement.ui_type == "ui_waveform") {
            ui_arguments[0] = std::clamp<std::int64_t>(ui_arguments[0], 1, 6);
            ui_arguments[1] = std::clamp<std::int64_t>(ui_arguments[1], 1, 16);
        } else if (statement.ui_type == "ui_table") {
            const auto normalize_dimension = [](std::int64_t value) {
                if (value > 5)
                    return std::int64_t{ 6 };
                if (value < 3)
                    return std::int64_t{ 2 };
                return value;
            };
            ui_arguments[0] = normalize_dimension(ui_arguments[0]);
            ui_arguments[1] = ui_arguments[1] == 1
                                      ? std::int64_t{ 1 }
                                      : normalize_dimension(ui_arguments[1]);
        } else if (statement.ui_type == "ui_value_edit") {
            auto low = ui_arguments[0];
            auto high = ui_arguments[1];
            auto step = ui_arguments[2];

            if (step == 0) {
                const auto normalized_low = std::max<std::int64_t>(low, 0);
                const auto normalized_high = std::min<std::int64_t>(high, 127);
                if (normalized_low <= normalized_high) {
                    low = normalized_low;
                    high = normalized_high;
                } else {
                    low = 0;
                    high = 127;
                }
            } else if (step < 0) {
                step = 1;
            }

            ui_arguments = { low, high, step };
        }

        if (statement.is_ui_control) {
            if ((statement.ui_type == "ui_knob" ||
                        statement.ui_type == "ui_slider" ||
                        statement.ui_type == "ui_value_edit") &&
                    ui_arguments.size() >= 2)
                ui_integer_ranges[statement.name] = { std::min(ui_arguments[0],
                                                              ui_arguments[1]),
                    std::max(ui_arguments[0], ui_arguments[1]) };
            else if (statement.ui_type == "ui_table" &&
                     ui_arguments.size() >= 3)
                ui_integer_ranges[statement.name] = { std::min<std::int64_t>(0,
                                                              ui_arguments[2]),
                    std::abs(ui_arguments[2]) };
        }
        if (statement.is_ui_control && host)
            host->declare_ui_control(statement.name, statement.ui_type,
                    variable_id, array_size, ui_arguments);

        if (!statement.index) {
            if (statement.is_polyphonic) {
                const auto initial =
                        statement.expr ? as_integer(evaluate(*statement.expr))
                                       : std::int64_t{ 0 };
                polyphonic_defaults[statement.name] = wrap_integer(initial);
                polyphonic_values[statement.name].clear();
                variables[statement.name] = wrap_integer(initial);
                return;
            }
            if (statement.is_ui_control &&
                    (statement.ui_type == "ui_knob" ||
                            statement.ui_type == "ui_slider" ||
                            statement.ui_type == "ui_value_edit")) {
                variables[statement.name] = wrap_integer(ui_arguments[0]);
            } else if (statement.expr) {
                variables[statement.name] = coerce_scalar(
                        statement.name, evaluate(*statement.expr));
            } else if (statement.name[0] == '@') {
                variables[statement.name] = std::string{};
            } else if (statement.name[0] == '~') {
                variables[statement.name] = 0.0;
            } else {
                variables[statement.name] = std::int64_t{ 0 };
            }
            return;
        }

        const auto size = static_cast<std::size_t>(*array_size);
        const char sigil = statement.name[0];

        if (sigil == '%') {
            std::vector<std::int64_t> values(size);
            for (std::size_t i = 0;
                    i < values.size() && i < statement.arguments.size(); ++i) {
                values[i] = as_integer(evaluate(statement.arguments[i]));
            }
            if (!statement.arguments.empty() &&
                    statement.arguments.size() < size) {
                std::fill(values.begin() + statement.arguments.size(),
                        values.end(), values[statement.arguments.size() - 1]);
            }
            if (statement.arguments.size() > size)
                warn(statement.location,
                        "too many integer array initializer values");
            variables[statement.name] = std::move(values);
            return;
        }

        if (sigil == '?') {
            std::vector<double> values(size);
            for (std::size_t i = 0;
                    i < values.size() && i < statement.arguments.size(); ++i) {
                values[i] = as_real(evaluate(statement.arguments[i]));
            }
            if (!statement.arguments.empty() &&
                    statement.arguments.size() < size) {
                std::fill(values.begin() + statement.arguments.size(),
                        values.end(), values[statement.arguments.size() - 1]);
            }
            if (statement.arguments.size() > size)
                warn(statement.location,
                        "too many real array initializer values");
            variables[statement.name] = std::move(values);
            return;
        }

        std::vector<std::string> values(size);
        for (std::size_t i = 0;
                i < values.size() && i < statement.arguments.size(); ++i) {
            values[i] = as_string(evaluate(statement.arguments[i]));
        }
        variables[statement.name] = std::move(values);
    }

    void initialize_source_declarations(
            const std::vector<Statement> &statements) {
        for (const auto &statement : statements) {
            if (statement.kind == Statement::Kind::declare) {
                allocate_declared_variable(statement);
                if (!statement.is_ui_control)
                    execute_declaration(statement);
            }
            initialize_source_declarations(statement.body);
            initialize_source_declarations(statement.alternate);
            for (const auto &branch : statement.branches)
                initialize_source_declarations(branch);
        }
    }

    void execute_assignment(const Statement &statement) {
        auto value = evaluate(*statement.expr);
        if (!statement.index) {
            if (polyphonic_defaults.contains(statement.name)) {
                if (auto *target = write_polyphonic(
                            statement.name, statement.location)) {
                    *target = wrap_integer(as_integer(value));
                    notify_variable_watch(
                            statement.name, std::nullopt, *target);
                }
                return;
            }
            auto local = variables.find(statement.name);
            // a local wins over host symbols, so don't rescan the UI controls
            // on every assignment for nothing
            const auto external =
                    local == variables.end()
                            ? resolve_environment_symbol(
                                      statement.name, statement.source_name)
                            : std::nullopt;
            if (local != variables.end() || !external) {
                auto assigned = coerce_scalar(statement.name, value);
                variables[statement.name] = assigned;
                update_ui_menu_selected_index(
                        statement.name, as_integer(assigned));
                notify_variable_watch(statement.name, std::nullopt, assigned);
                if (host && ui_control_ids.contains(statement.name))
                    host->ui_control_value_changed(
                            environment_lookup_name(
                                    statement.name, statement.source_name),
                            assigned);
            } else {
                auto assigned = coerce_scalar(statement.name, value);
                if (external->ui_type == "ui_menu")
                    import_host_menu_entries(statement.name);
                update_ui_menu_selected_index(
                        statement.name, as_integer(assigned));
                if (current_callback_type == 17) {
                    if (std::holds_alternative<double>(external->value))
                        assigned = as_real(value);
                    else if (std::holds_alternative<std::int64_t>(
                                     external->value))
                        assigned = wrap_integer(as_integer(value));
                    else if (std::holds_alternative<std::string>(
                                     external->value))
                        assigned = as_string(value);
                }
                if (!host->assign_symbol(environment_lookup_name(statement.name,
                                                 statement.source_name),
                            assigned)) {
                    diagnostics.push_back({ statement.location,
                            "host rejected assignment to " +
                                    statement.source_name });
                } else {
                    notify_variable_watch(
                            statement.name, std::nullopt, assigned);
                }
            }
            return;
        }

        const auto index = as_integer(evaluate(*statement.index));
        auto variable = variables.find(statement.name);
        if (variable == variables.end()) {
            auto external = resolve_environment_symbol(
                    statement.name, statement.source_name);
            if (external) {
                bool assigned = false;
                if (auto values = std::get_if<std::vector<std::int64_t>>(
                            &external->value);
                        values && index >= 0 &&
                        index < static_cast<std::int64_t>(values->size())) {
                    (*values)[static_cast<std::size_t>(index)] =
                            as_integer(value);
                    assigned = true;
                } else if (auto values = std::get_if<std::vector<double>>(
                                   &external->value);
                        values && index >= 0 &&
                        index < static_cast<std::int64_t>(values->size())) {
                    (*values)[static_cast<std::size_t>(index)] = as_real(value);
                    assigned = true;
                } else if (auto values = std::get_if<std::vector<std::string>>(
                                   &external->value);
                        values && index >= 0 &&
                        index < static_cast<std::int64_t>(values->size())) {
                    (*values)[static_cast<std::size_t>(index)] =
                            as_string(value);
                    assigned = true;
                }
                if (!assigned) {
                    warn(statement.location,
                            "Array index out of range (assignment has no effect)!");
                } else if (!host->assign_symbol(
                                   environment_lookup_name(statement.name,
                                           statement.source_name),
                                   external->value)) {
                    diagnostics.push_back({ statement.location,
                            "host rejected assignment to " +
                                    statement.source_name });
                } else {
                    Value assigned;
                    if (const auto values =
                                    std::get_if<std::vector<std::int64_t>>(
                                            &external->value))
                        assigned = (*values)[static_cast<std::size_t>(index)];
                    else if (const auto values =
                                     std::get_if<std::vector<double>>(
                                             &external->value))
                        assigned = (*values)[static_cast<std::size_t>(index)];
                    else
                        assigned = std::get<std::vector<std::string>>(
                                external->value)[static_cast<std::size_t>(
                                index)];
                    notify_variable_watch(statement.name, index, assigned);
                }
                return;
            }
            diagnostics.push_back(
                    { statement.location, "unknown array " + statement.name });
            return;
        }

        if (auto values = std::get_if<std::vector<std::int64_t>>(
                    &variable->second);
                values && index >= 0 &&
                index < static_cast<std::int64_t>(values->size())) {
            (*values)[static_cast<std::size_t>(index)] = as_integer(value);
            notify_variable_watch(statement.name, index,
                    (*values)[static_cast<std::size_t>(index)]);
            if (host && ui_control_ids.contains(statement.name))
                host->ui_control_value_changed(
                        environment_lookup_name(
                                statement.name, statement.source_name),
                        variable->second);
        } else if (auto values =
                           std::get_if<std::vector<double>>(&variable->second);
                values && index >= 0 &&
                index < static_cast<std::int64_t>(values->size())) {
            (*values)[static_cast<std::size_t>(index)] = as_real(value);
            notify_variable_watch(statement.name, index,
                    (*values)[static_cast<std::size_t>(index)]);
            if (host && ui_control_ids.contains(statement.name))
                host->ui_control_value_changed(
                        environment_lookup_name(
                                statement.name, statement.source_name),
                        variable->second);
        } else if (auto values = std::get_if<std::vector<std::string>>(
                           &variable->second);
                values && index >= 0 &&
                index < static_cast<std::int64_t>(values->size())) {
            (*values)[static_cast<std::size_t>(index)] = as_string(value);
            notify_variable_watch(statement.name, index,
                    (*values)[static_cast<std::size_t>(index)]);
            if (host && ui_control_ids.contains(statement.name))
                host->ui_control_value_changed(
                        environment_lookup_name(
                                statement.name, statement.source_name),
                        variable->second);
        } else {
            warn(statement.location,
                    "Array index out of range (assignment has no effect)!");
        }
    }

    ExecutionResult execute_call(
            const Statement &statement, ExecutionState &state) {
        const auto canonical_name =
                detail::canonical_builtin_name(statement.name);
        if (statement.name == "sort") {
            execute_sort(statement);
            return ExecutionResult::completed;
        }
        if (statement.name == "inc" || statement.name == "dec") {
            execute_increment(statement, statement.name == "inc" ? 1 : -1);
            return ExecutionResult::completed;
        }
        if (statement.name == "get_event_ids") {
            execute_integer_array_output(statement, 0, true);
            return ExecutionResult::completed;
        }
        if (statement.name == "get_sel_zones_idx") {
            execute_integer_array_output(statement, -1, true);
            return ExecutionResult::completed;
        }
        if (statement.name == "get_connected_slots") {
            execute_integer_array_output(statement, -1, false);
            return ExecutionResult::completed;
        }
        if (statement.name == "get_scale") {
            execute_get_scale(statement);
            return ExecutionResult::completed;
        }
        if (statement.name == "watch_array_idx") {
            if (statement.arguments.size() == 2) {
                const auto index = as_integer(evaluate(statement.arguments[1]));
                const auto variable =
                        variables.find(statement.arguments[0].text);
                std::size_t size{};
                if (variable != variables.end()) {
                    std::visit(
                            [&](const auto &stored) {
                                if constexpr (requires { stored.size(); })
                                    size = stored.size();
                            },
                            variable->second);
                }
                if (index < 0 || index >= static_cast<std::int64_t>(size)) {
                    warn(statement.location, "watch_array_idx(): The watched "
                                             "array index is out of range!");
                } else {
                    watched_array_indices[statement.arguments[0].text].insert(
                            index);
                }
            }
            return ExecutionResult::completed;
        }
        if (statement.name == "get_controller_pages") {
            execute_get_controller_pages(statement);
            return ExecutionResult::completed;
        }
        if (statement.name == "set_controller_pages") {
            execute_set_controller_pages(statement, state);
            return ExecutionResult::completed;
        }

        // the async ID gets thrown away here but the load/save still has to
        // happen, so go through the same path as expressions
        if (statement.name == "load_array" || statement.name == "save_array" ||
                statement.name == "load_array_str" ||
                statement.name == "save_array_str") {
            Expression expression;
            expression.kind = Expression::Kind::call;
            expression.text = statement.name;
            expression.children = statement.arguments;
            expression.location = statement.location;
            (void)evaluate_call(expression);
            return ExecutionResult::completed;
        }

        std::vector<Value> arguments;
        arguments.reserve(statement.arguments.size());
        for (std::size_t index = 0; index < statement.arguments.size();
                ++index) {
            const auto &expression = statement.arguments[index];
            if (index == 0 && detail::is_pgs_call_name(statement.name) &&
                    expression.kind == Expression::Kind::call &&
                    expression.children.empty()) {
                arguments.emplace_back(expression.text);
            } else {
                arguments.push_back(evaluate(expression));
            }
        }
        if (canonical_name == "load_patch" ||
                canonical_name == "load_next_patch" ||
                canonical_name == "unload_slot" ||
                canonical_name == "reset_engine") {
            if (canonical_name == "load_next_patch" && arguments.size() == 2) {
                const auto slot = as_integer(arguments[1]);
                if (slot < 0 || slot > 64) {
                    warn(statement.location, "load_patch(): Program slot index "
                                             "must be between 0 and 64!");
                    return ExecutionResult::completed;
                }
            }
            const bool synchronous = current_callback_type == 5;
            (void)start_engine_command(canonical_name, arguments, synchronous);
            return ExecutionResult::completed;
        }
        if (statement.name == "subscribe_async" && arguments.size() == 3) {
            const AsyncValueAddress address{ as_integer(arguments[0]),
                as_integer(arguments[1]), as_integer(arguments[2]) };
            subscribe_to_async_value(address);
            return ExecutionResult::completed;
        }
        if (canonical_name == "set_engine_par_m" && !arguments.empty()) {
            const auto parameter = as_integer(arguments[0]);
            if (!detail::is_valid_engine_parameter_index(parameter))
                return ExecutionResult::completed;
        }
        if (statement.name == "request_value_async" && arguments.size() == 3) {
            const AsyncValueAddress address{ as_integer(arguments[0]),
                as_integer(arguments[1]), as_integer(arguments[2]) };
            (void)request_engine_value(address);
            return ExecutionResult::completed;
        }
        if ((statement.name == "set_value_async" ||
                    statement.name == "set_value_async_str") &&
                arguments.size() == 4) {
            const AsyncValueAddress address{ as_integer(arguments[0]),
                as_integer(arguments[1]), as_integer(arguments[2]) };
            Value value =
                    statement.name == "set_value_async"
                            ? Value{ wrap_integer(as_integer(arguments[3])) }
                            : Value{ as_string(arguments[3]) };
            (void)set_engine_value(address, value);
            return ExecutionResult::completed;
        }
        if ((statement.name == "set_global_scale" ||
                    statement.name == "set_global_key") &&
                arguments.size() == 1) {
            const std::int64_t maximum =
                    statement.name == "set_global_scale" ? 73 : 11;
            arguments[0] = std::clamp(
                    as_integer(arguments[0]), std::int64_t{ 0 }, maximum);
            if (host)
                (void)host->call(statement.name, arguments);
            return ExecutionResult::completed;
        }
        if ((statement.name == "set_knob_label" ||
                    statement.name == "set_knob_unit" ||
                    statement.name == "set_table_steps_shown" ||
                    statement.name == "set_knob_defval" ||
                    statement.name == "set_text" ||
                    statement.name == "add_text_line" ||
                    statement.name == "set_ui_wf_property") &&
                !statement.arguments.empty() &&
                statement.arguments[0].kind == Expression::Kind::name) {
            const auto control =
                    ui_control_ids.find(statement.arguments[0].text);
            if (control != ui_control_ids.end())
                arguments[0] = control->second;
        }
        if (statement.name == "hide_part" && arguments.size() == 2) {
            const auto mask = as_integer(arguments[1]);
            if (static_cast<std::uint32_t>(mask) > 32) {
                warn(statement.location,
                        "hide_part(): Invalid $HIDE_PART_* bitmask!");
                arguments[1] = std::int64_t{ 0 };
            }
        }
        if (normalize_engine_arguments(statement.name, arguments,
                    statement.location) == HostCallDisposition::skip)
            return ExecutionResult::completed;
        if (canonical_name == "load_ir_sample" && arguments.size() == 3) {
            (void)start_engine_command(
                    canonical_name, arguments, current_callback_type == 5);
            return ExecutionResult::completed;
        }
        if (canonical_name == "set_engine_par" && arguments.size() == 5 &&
                current_callback_type != 5 &&
                (as_integer(arguments[0]) == 246 ||
                        as_integer(arguments[0]) == 247)) {
            // effect type changes are async outside init, even as statements
            (void)start_engine_command(canonical_name, arguments, false);
            return ExecutionResult::completed;
        }
        static const std::unordered_set<std::string> async_engine_statements = {
            "set_voice_limit", "set_zone_par", "set_loop_par",
            "set_nks_nav_par", "set_nks_nav_name", "reset_nks_nav"
        };
        if (current_callback_type != 5 &&
                async_engine_statements.contains(canonical_name)) {
            (void)start_engine_command(canonical_name, arguments, false);
            return ExecutionResult::completed;
        }
        if (statement.name == "message" && !log_messages)
            return ExecutionResult::completed;
        if (statement.name == "set_control_par" ||
                statement.name == "set_control_par_str" ||
                statement.name == "set_control_par_arr" ||
                statement.name == "set_control_par_str_arr" ||
                statement.name == "set_control_par_real_arr")
            use_named_ui_control(arguments);
        if (statement.name == "set_menu_item_value" ||
                statement.name == "set_menu_item_visibility" ||
                statement.name == "set_menu_item_str")
            use_named_ui_control(arguments);
        if (statement.name == "attach_level_meter")
            use_named_ui_control(arguments);
        if ((statement.name == "mf_copy_export_area" ||
                    statement.name == "mf_set_num_export_areas") &&
                !arguments.empty() && as_integer(arguments.front()) < 0)
            return ExecutionResult::completed;
        if ((statement.name == "move_control" ||
                    statement.name == "move_control_px" ||
                    statement.name == "hide_part" ||
                    statement.name == "set_control_help" ||
                    statement.name == "set_knob_label" ||
                    statement.name == "set_knob_unit" ||
                    statement.name == "set_table_steps_shown" ||
                    statement.name == "set_knob_defval" ||
                    statement.name == "set_text" ||
                    statement.name == "add_text_line" ||
                    statement.name == "set_ui_wf_property" ||
                    statement.name == "attach_zone" ||
                    statement.name == "add_menu_item") &&
                !statement.arguments.empty() &&
                statement.arguments[0].kind == Expression::Kind::name) {
            arguments[0] = statement.arguments[0].text;
        }
        if (statement.name == "add_menu_item" && !arguments.empty()) {
            const auto control_name = lowercase(as_string(arguments[0]));
            import_host_menu_entries(control_name);
            ++ui_menu_item_counts[control_name];
            if (arguments.size() == 3)
                ui_menu_item_values[control_name].push_back(
                        as_integer(arguments[2]));
        }
        if (statement.name == "set_menu_item_value" && arguments.size() == 3) {
            const auto control_name = lowercase(as_string(arguments[0]));
            import_host_menu_entries(control_name);
            const auto item = as_integer(arguments[1]);
            auto values = ui_menu_item_values.find(control_name);
            if (values != ui_menu_item_values.end() && item >= 0 &&
                    item < static_cast<std::int64_t>(values->second.size()))
                values->second[static_cast<std::size_t>(item)] =
                        as_integer(arguments[2]);
        }

        if (statement.name == "wait_async") {
            const auto async_id = arguments.empty()
                                          ? std::int64_t{ 0 }
                                          : as_integer(arguments.front());
            if (!host || !host->is_async_active(async_id))
                return ExecutionResult::completed;
            state.wait_id = host ? host->reserve_wait_id() : reserve_wait_id();
            state.wait_duration = async_id;
            state.wait_unit = WaitUnit::async_completion;
            return ExecutionResult::suspended;
        }

        if (statement.name == "stop_wait") {
            const auto callback_id = arguments.empty()
                                             ? std::int64_t{ -1 }
                                             : as_integer(arguments[0]);
            const auto mode = arguments.size() < 2 ? std::int64_t{ 0 }
                                                   : as_integer(arguments[1]);
            std::vector<std::uint64_t> matching_waits;
            for (const auto &[wait_id, waiting] : suspended) {
                if (waiting.callback_id == callback_id)
                    matching_waits.push_back(wait_id);
            }

            for (const auto wait_id : matching_waits) {
                auto waiting = suspended.find(wait_id);
                if (waiting == suspended.end())
                    continue;
                if (host)
                    host->cancel_wait(wait_id);
                if (mode == 1) {
                    suspended.erase(waiting);
                    continue;
                }
                waiting->second.wait_duration = 0;
                waiting->second.wait_unit = WaitUnit::immediate;
                if (host)
                    host->schedule_wait(wait_id, 0, WaitUnit::immediate);
            }
            return ExecutionResult::completed;
        }

        if (statement.name == "wait" || statement.name == "wait_ticks") {
            auto duration = arguments.empty() ? std::int64_t{ 0 }
                                              : as_integer(arguments.front());
            if (duration < 1) {
                warn(statement.location,
                        "wait(): Wait time cannot be smaller than 1 microsecond!");
                duration = 1;
            }
            state.wait_id = host ? host->reserve_wait_id() : reserve_wait_id();
            state.wait_duration = duration;
            state.wait_unit = statement.name == "wait" ? WaitUnit::microseconds
                                                       : WaitUnit::ticks;
            return ExecutionResult::suspended;
        }

        const auto function = code().functions.find(statement.name);
        if (function != code().functions.end()) {
            state.frames.push_back({ ExecutionFrame::Kind::block,
                    &function->second, nullptr, 0, true });
        } else if (host) {
            host->call(
                    detail::canonical_builtin_name(statement.name), arguments);
            refresh_control_value_after_set(statement.name, arguments);
        }
        // the selected index stays put but its payload might not, so push it
        // again once the host has the entry, early restore included
        if ((statement.name == "add_menu_item" ||
                    statement.name == "set_menu_item_value") &&
                !arguments.empty())
            refresh_ui_menu_payload(lowercase(as_string(arguments[0])));
        return ExecutionResult::completed;
    }

    void execute_increment(const Statement &statement, std::int64_t amount) {
        if (statement.arguments.size() != 1)
            return;
        const auto &target = statement.arguments.front();
        if (target.kind != Expression::Kind::name || target.children.size() > 1)
            return;

        if (polyphonic_defaults.contains(target.text)) {
            if (!target.children.empty())
                return;
            if (auto *value =
                            write_polyphonic(target.text, statement.location)) {
                *value = wrap_integer(*value + amount);
                notify_variable_watch(target.text, std::nullopt, *value);
            }
            return;
        }

        auto local = variables.find(target.text);
        auto external = local == variables.end()
                                ? resolve_environment_symbol(
                                          target.text, target.source_name)
                                : std::nullopt;
        Value *value = local != variables.end() ? &local->second
                       : external               ? &external->value
                                                : nullptr;
        if (!value)
            return;

        std::optional<std::int64_t> changed_index;
        Value changed_value;
        const auto ui_id = ui_control_ids.find(target.text);
        const auto type = ui_id == ui_control_ids.end()
                                  ? std::string_view{}
                                  : std::string_view{ ui_control_types.at(
                                            ui_id->second) };
        const auto clip_ui = [&](std::int64_t requested) {
            auto range = ui_integer_ranges.find(target.text);
            if (range == ui_integer_ranges.end())
                return requested;
            auto bounds = range->second;
            if (host)
                if (const auto current =
                                host->ui_control_value_range(target.text))
                    bounds = { std::min(current->first, current->second),
                        std::max(current->first, current->second) };
            const auto clipped =
                    std::clamp(requested, bounds.first, bounds.second);
            if (clipped != requested) {
                const auto boundary =
                        type == "ui_knob" || type == "ui_value_edit"
                                ? (requested < bounds.first ? "minimum"
                                                            : "maximum")
                                : "minimum";
                // clang-format off
                warn(statement.location, std::string(type == "ui_value_edit" ?
                        "invalid " : "Invalid ") + std::string(type) +
                        " value! Input value was clipped to " + boundary +
                        " declared value!");
                // clang-format on
            }
            return clipped;
        };
        if (target.children.empty()) {
            if (auto scalar = std::get_if<std::int64_t>(value)) {
                auto requested = wrap_integer(*scalar + amount);
                if (type == "ui_button" || type == "ui_switch")
                    requested = amount > 0 ? 1 : 0;
                else if (type == "ui_waveform" || type == "ui_file_selector")
                    requested = std::clamp<std::int64_t>(requested, 0, 1);
                else if (type == "ui_menu") {
                    const auto menu = ui_menu_item_values.find(target.text);
                    if (menu == ui_menu_item_values.end() ||
                            menu->second.empty())
                        return;
                    const auto selected =
                            ui_menu_selected_indices.find(target.text);
                    const auto index =
                            selected == ui_menu_selected_indices.end()
                                    ? 0
                                    : selected->second;
                    if (index < 0 || static_cast<std::size_t>(index) >=
                                             menu->second.size())
                        return;
                    requested = wrap_integer(menu->second[index] + amount);
                    if (std::ranges::find(menu->second, requested) ==
                            menu->second.end()) {
                        warn(statement.location,
                                "Invalid ui_menu value! Assignment has no effect.");
                        return;
                    }
                }
                *scalar = clip_ui(requested);
                changed_value = *scalar;
            } else {
                return;
            }
        } else {
            auto values = std::get_if<std::vector<std::int64_t>>(value);
            const auto index = as_integer(evaluate(target.children.front()));
            if (!values || index < 0 ||
                    index >= static_cast<std::int64_t>(values->size())) {
                warn(statement.location,
                        statement.name + "(): Array index is out of bounds!");
                return;
            }
            auto &element = (*values)[static_cast<std::size_t>(index)];
            element = clip_ui(wrap_integer(element + amount));
            changed_index = index;
            changed_value = element;
        }

        if (external && host) {
            if (!host->assign_symbol(environment_lookup_name(
                                             target.text, target.source_name),
                        *value)) {
                diagnostics.push_back({ statement.location,
                        "host rejected assignment to " + target.source_name });
                return;
            }
        }
        if (!changed_index)
            update_ui_menu_selected_index(
                    target.text, as_integer(changed_value));
        notify_variable_watch(target.text, changed_index, changed_value);
        if (host && ui_id != ui_control_ids.end())
            host->ui_control_value_changed(
                    environment_lookup_name(target.text, target.source_name),
                    *value);
    }

    void execute_integer_array_output(const Statement &statement,
            std::int64_t terminator, bool reserve_terminator) {
        if (!host || statement.arguments.size() != 1)
            return;
        const auto &target = statement.arguments.front();
        if (target.kind != Expression::Kind::name || !target.children.empty())
            return;

        auto local = variables.find(target.text);
        auto external = local == variables.end()
                                ? resolve_environment_symbol(
                                          target.text, target.source_name)
                                : std::nullopt;
        Value *value = local != variables.end() ? &local->second
                       : external               ? &external->value
                                                : nullptr;
        auto *target_values =
                value ? std::get_if<std::vector<std::int64_t>>(value) : nullptr;
        if (!target_values)
            return;

        const auto result = host->call(statement.name, {});
        if (!result)
            return;
        const auto *event_ids =
                std::get_if<std::vector<std::int64_t>>(&*result);
        if (!event_ids)
            return;

        const auto output_capacity =
                reserve_terminator && !target_values->empty()
                        ? target_values->size() - 1
                        : target_values->size();
        const auto copy_count = std::min(event_ids->size(), output_capacity);
        std::copy_n(event_ids->begin(), copy_count, target_values->begin());
        if (copy_count < target_values->size())
            (*target_values)[copy_count] = terminator;
        if (statement.name == "get_connected_slots" &&
                target_values->size() < event_ids->size() + 1) {
            warn(statement.location,
                    "get_connected_slots(): array must be larger than "
                    "num_connected_slots");
        }

        if (external &&
                !host->assign_symbol(environment_lookup_name(
                                             target.text, target.source_name),
                        *value)) {
            diagnostics.push_back({ statement.location,
                    "host rejected assignment to " + target.source_name });
        }
    }

    void execute_get_scale(const Statement &statement) {
        if (statement.arguments.size() != 2)
            return;
        const auto &target = statement.arguments[1];
        if (target.kind != Expression::Kind::name || !target.children.empty())
            return;

        auto local = variables.find(target.text);
        auto external = local == variables.end()
                                ? resolve_environment_symbol(
                                          target.text, target.source_name)
                                : std::nullopt;
        Value *value = local != variables.end() ? &local->second
                       : external               ? &external->value
                                                : nullptr;
        auto *target_values =
                value ? std::get_if<std::vector<std::int64_t>>(value) : nullptr;
        if (!target_values)
            return;

        std::fill(target_values->begin(), target_values->end(), -1);
        const auto scale = as_integer(evaluate(statement.arguments[0]));
        if (scale >= 0 &&
                scale < static_cast<std::int64_t>(scale_infos.size())) {
            const auto &notes =
                    scale_infos[static_cast<std::size_t>(scale)].notes;
            std::copy_n(notes.begin(),
                    std::min(notes.size(), target_values->size()),
                    target_values->begin());
        }

        if (external && host &&
                !host->assign_symbol(environment_lookup_name(
                                             target.text, target.source_name),
                        *value)) {
            diagnostics.push_back({ statement.location,
                    "host rejected assignment to " + target.source_name });
        }
    }

    void execute_get_controller_pages(const Statement &statement) {
        if (!host || statement.arguments.size() != 3)
            return;
        auto group_names = variables.find(statement.arguments[0].text);
        auto parameter_ids = variables.find(statement.arguments[1].text);
        auto assignment_names = variables.find(statement.arguments[2].text);
        if (group_names == variables.end() ||
                parameter_ids == variables.end() ||
                assignment_names == variables.end()) {
            return;
        }
        auto *groups =
                std::get_if<std::vector<std::string>>(&group_names->second);
        auto *parameters =
                std::get_if<std::vector<std::int64_t>>(&parameter_ids->second);
        auto *assignments = std::get_if<std::vector<std::string>>(
                &assignment_names->second);
        if (!groups || !parameters || !assignments)
            return;
        const auto pages = host->get_controller_pages();
        if (!pages)
            return;
        const auto count = std::min({ groups->size(), parameters->size(),
                assignments->size(), pages->group_names.size(),
                pages->parameter_ids.size(), pages->assignment_names.size() });
        std::copy_n(pages->group_names.begin(), count, groups->begin());
        std::copy_n(pages->parameter_ids.begin(), count, parameters->begin());
        std::copy_n(
                pages->assignment_names.begin(), count, assignments->begin());
    }

    void execute_set_controller_pages(
            const Statement &statement, ExecutionState &state) {
        if (!host || statement.arguments.size() != 3)
            return;
        state.controller_page_requests.push_back(
                { { { statement.arguments[0].text, statement.arguments[1].text,
                        statement.arguments[2].text } } });
    }

    void execute_sort(const Statement &statement) {
        if (statement.arguments.size() != 2 && statement.arguments.size() != 4)
            return;
        const auto &target = statement.arguments.front();
        if (target.kind != Expression::Kind::name || !target.children.empty())
            return;

        auto local = variables.find(target.text);
        auto external = local == variables.end()
                                ? resolve_environment_symbol(
                                          target.text, target.source_name)
                                : std::nullopt;
        Value *value = local != variables.end() ? &local->second
                       : external               ? &external->value
                                                : nullptr;
        if (!value)
            return;

        const bool descending =
                as_integer(evaluate(statement.arguments[1])) != 0;
        const auto sort_values = [&](auto &values) {
            if (values.empty())
                return;
            auto from = std::int64_t{ 0 };
            auto to = static_cast<std::int64_t>(values.size() - 1);
            if (statement.arguments.size() == 4) {
                from = as_integer(evaluate(statement.arguments[2]));
                to = as_integer(evaluate(statement.arguments[3]));
                const auto last = static_cast<std::int64_t>(values.size() - 1);
                const bool out_of_range =
                        from < 0 || from > last || to < 0 || to > last;
                from = std::clamp(from, std::int64_t{ 0 }, last);
                to = std::clamp(to, std::int64_t{ 0 }, last);
                if (out_of_range) {
                    warn(statement.location,
                            "sort(): From and/or To arguments are out of range. "
                            "Using "
                            "first or last array index, respectively!");
                }
                if (from > to)
                    std::swap(from, to);
            }
            auto first = values.begin() + from;
            auto last = values.begin() + to + 1;
            if (descending)
                std::sort(first, last, std::greater<>{});
            else
                std::sort(first, last);
        };

        if (auto values = std::get_if<std::vector<std::int64_t>>(value))
            sort_values(*values);
        else if (auto values = std::get_if<std::vector<double>>(value))
            sort_values(*values);
        else
            return;

        if (external && host &&
                !host->assign_symbol(environment_lookup_name(
                                             target.text, target.source_name),
                        *value)) {
            diagnostics.push_back({ statement.location,
                    "host rejected assignment to " + target.source_name });
        }
    }

    const std::vector<Statement> *select_branch(const Statement &statement) {
        const auto value = as_integer(evaluate(*statement.expr));
        for (std::size_t i = 0; i < statement.branches.size(); ++i) {
            const auto low = as_integer(evaluate(statement.case_low[i]));
            const auto high = as_integer(evaluate(statement.case_high[i]));
            // descending ranges are inclusive too, eg 0 to -45000
            if (value >= std::min(low, high) && value <= std::max(low, high))
                return &statement.branches[i];
        }
        return nullptr;
    }

    void leave_function(ExecutionState &state) {
        while (!state.frames.empty()) {
            const bool boundary = state.frames.back().function_body;
            state.frames.pop_back();
            if (boundary)
                return;
        }
    }

    void continue_nearest_loop(ExecutionState &state) {
        while (!state.frames.empty() &&
                state.frames.back().kind != ExecutionFrame::Kind::loop) {
            state.frames.pop_back();
        }
    }

    ExecutionResult execute(ExecutionState &state) {
        struct ActiveStateGuard {
            ExecutionState *&active_state;
            ~ActiveStateGuard() { active_state = nullptr; }
        } guard{ active_state };
        active_state = &state;
        current_callback_id = state.callback_id;
        current_callback_type = state.callback_type;
        current_async_id = state.async_id;
        current_async_type = state.async_type;
        current_async_parameter_one = state.async_parameter_one;
        current_async_parameter_two = state.async_parameter_two;
        current_async_exit_status = state.async_exit_status;
        current_async_value = state.async_value;

        while (!state.frames.empty()) {
            auto &frame = state.frames.back();
            if (frame.kind == ExecutionFrame::Kind::loop) {
                if (!as_boolean(evaluate(*frame.loop->expr))) {
                    state.frames.pop_back();
                    continue;
                }
                if (frame.loop_iterations >= 10'000'000) {
                    warn(frame.loop->location,
                            "While loop was terminated after 10 million iterations "
                            "without a wait(). Check the loop iterator and "
                            "condition, or "
                            "use a wait() statement!");
                    state.frames.pop_back();
                    continue;
                }
                ++frame.loop_iterations;
                state.frames.push_back(
                        { ExecutionFrame::Kind::block, &frame.loop->body });
                continue;
            }

            if (frame.next_statement == frame.statements->size()) {
                state.frames.pop_back();
                continue;
            }

            const auto &statement = (*frame.statements)[frame.next_statement++];
            if (statement.kind == Statement::Kind::exit_) {
                leave_function(state);
                continue;
            }

            if (statement.kind == Statement::Kind::continue_) {
                continue_nearest_loop(state);
                continue;
            }

            if (statement.kind == Statement::Kind::make_persistent)
                continue;

            if (statement.kind == Statement::Kind::read_persistent) {
                if (!statement.arguments.empty()) {
                    const auto &target = statement.arguments.front();
                    apply_pending_persistent_value(
                            target.source_name.empty() ? target.text
                                                       : target.source_name);
                }
                continue;
            }

            if (statement.kind == Statement::Kind::declare) {
                if (statement.is_ui_control)
                    execute_declaration(statement);
                continue;
            }

            if (statement.kind == Statement::Kind::assign) {
                execute_assignment(statement);
                continue;
            }

            if (statement.kind == Statement::Kind::call) {
                const auto result = execute_call(statement, state);
                if (result != ExecutionResult::completed) {
                    flush_controller_page_requests(state);
                    return result;
                }
                continue;
            }

            if (statement.kind == Statement::Kind::if_) {
                const auto &branch = as_boolean(evaluate(*statement.expr))
                                             ? statement.body
                                             : statement.alternate;
                state.frames.push_back(
                        { ExecutionFrame::Kind::block, &branch });
                continue;
            }

            if (statement.kind == Statement::Kind::while_) {
                state.frames.push_back(
                        { ExecutionFrame::Kind::loop, nullptr, &statement });
                continue;
            }

            if (statement.kind == Statement::Kind::select_) {
                if (const auto branch = select_branch(statement))
                    state.frames.push_back(
                            { ExecutionFrame::Kind::block, branch });
                continue;
            }
        }
        flush_controller_page_requests(state);
        return ExecutionResult::completed;
    }
};

Runtime::Runtime(Host *host) : impl(std::make_unique<Impl>()) {
    impl->host = host;
}
Runtime::~Runtime() = default;
Runtime::Runtime(Runtime &&) noexcept = default;
Runtime &Runtime::operator=(Runtime &&) noexcept = default;
Runtime Runtime::clone_state(Host &independent_host) const {
    if (!impl || impl->active_state)
        throw std::logic_error(
                "cannot clone a moved-from or executing KSP runtime");
    if (impl->host == &independent_host)
        throw std::invalid_argument(
                "KSP state clone requires an independent host");
    Runtime result(&independent_host);
    auto state = std::make_unique<Impl>(*impl);
    // rebind before the copy can die, otherwise its wait cleanup cancels the
    // waits on the source host
    state->host = &independent_host;
    result.impl = std::move(state);
    return result;
}
bool Runtime::load(std::string_view source) {
    impl->diagnostics.clear();
    if (source.empty() || source.front() == '\0')
        return false;

    impl->variables.clear();
    impl->supplied_builtin_values.clear();
    impl->polyphonic_defaults.clear();
    impl->polyphonic_values.clear();
    impl->persistent_variables.clear();
    impl->persistent_variable_order.clear();
    impl->declared_variable_order.clear();
    impl->declared_variable_spellings.clear();
    impl->pending_persistent_values.clear();
    impl->pending_array_loads.clear();
    impl->pending_array_saves.clear();
    impl->pending_detections.clear();
    impl->pending_async_values.clear();
    impl->pending_engine_commands.clear();
    impl->async_value_subscriptions.clear();
    impl->script_variable_ids.clear();
    impl->ui_control_ids.clear();
    impl->ui_control_names.clear();
    impl->ui_control_types.clear();
    impl->ui_integer_ranges.clear();
    impl->ui_menu_item_counts.clear();
    impl->ui_menu_item_values.clear();
    impl->ui_menu_selected_indices.clear();
    impl->ui_text_shifting_disabled.clear();
    impl->watched_variables.clear();
    impl->watched_array_indices.clear();
    impl->log_messages = true;
    impl->log_warnings = true;
    impl->log_watching = true;
    impl->next_variable_id = 0;
    impl->clear_suspended();
    try {
        auto tokens =
                normalize_line_continuations(lex(source, &impl->diagnostics));
        auto program =
                detail::parse_program(tokens, impl->diagnostics, impl->host);
        detail::validate_program(program, impl->host, impl->diagnostics);
        if (const auto init = program.callbacks.find("init");
                init != program.callbacks.end()) {
            impl->collect_declared_variable_order(init->second);
            impl->collect_persistent_variables(init->second);
        }
        impl->process_parser_directives(program);
        if (!impl->has_errors()) {
            if (const auto init = program.callbacks.find("init");
                    init != program.callbacks.end()) {
                impl->current_callback_type = detail::callback_type_id("init");
                impl->initialize_source_declarations(init->second);
                impl->current_callback_type = 0;
            }
        }
        impl->program =
                std::make_shared<const ParsedProgram>(std::move(program));
    } catch (const std::bad_alloc &) {
        impl->current_callback_type = 0;
        impl->program.reset();
        impl->variables.clear();
        impl->diagnostics.clear();
        impl->diagnostics.push_back({ {}, "memory exhausted" });
    }
    return !impl->has_errors();
}
bool Runtime::has_callback(std::string_view callback) const {
    const auto callback_name =
            detail::canonical_callback_name(lowercase(callback));
    return impl->code().callbacks.contains(callback_name);
}
bool Runtime::has_callback(
        std::string_view callback, std::string_view callback_variable) const {
    if (detail::canonical_callback_name(lowercase(callback)) != "ui_control")
        return false;
    return impl->code().ui_callbacks.contains(lowercase(callback_variable));
}
bool Runtime::has_ui_control(std::string_view name) const {
    return impl->ui_control_ids.contains(lowercase(name));
}
bool Runtime::run(std::string_view callback) {
    const auto callback_name =
            detail::canonical_callback_name(lowercase(callback));
    auto it = impl->code().callbacks.find(callback_name);
    if (it == impl->code().callbacks.end()) {
        impl->diagnostics.push_back(
                { {}, "callback not found: " + std::string(callback) });
        return false;
    }
    Impl::ExecutionState state;
    state.callback_type = detail::callback_type_id(callback_name);
    state.callback_id = static_cast<std::int32_t>(
            impl->host ? impl->host->reserve_callback_id()
                       : reserve_callback_id());
    state.frames.push_back({ Impl::ExecutionFrame::Kind::block, &it->second });
    if (state.callback_type == 1 || state.callback_type == 2) {
        auto context = impl->host ? impl->host->polyphonic_event_context()
                                  : std::nullopt;
        if (!context)
            context = PolyphonicEventContext{ 0, std::nullopt };
        if (context->index >= 0 && context->index < 8192) {
            state.polyphonic_index = context->index;
            if (state.callback_type == 1) {
                auto source = context->source_index;
                if (source && (*source < 0 || *source >= 8192))
                    source.reset();
                impl->clone_polyphonic_values(source, context->index);
            }
        }
    }
    if (impl->host && (state.callback_type == 1 || state.callback_type == 2))
        impl->host->refresh_affected_groups(state.callback_type);
    const auto result = impl->execute(state);
    if (result == Impl::ExecutionResult::suspended)
        impl->store_suspended(std::move(state));
    if (callback_name == "init" && result == Impl::ExecutionResult::completed)
        impl->apply_all_pending_persistent_values();
    return result != Impl::ExecutionResult::failed && !impl->has_errors();
}
bool Runtime::run(
        std::string_view callback, std::string_view callback_variable) {
    if (detail::canonical_callback_name(lowercase(callback)) != "ui_control") {
        impl->diagnostics.push_back(
                { {}, "only ui_control callbacks take a callback variable" });
        return false;
    }
    const auto callback_body =
            impl->code().ui_callbacks.find(lowercase(callback_variable));
    if (callback_body == impl->code().ui_callbacks.end()) {
        impl->diagnostics.push_back(
                { {}, "ui_control callback not found: " +
                                std::string(callback_variable) });
        return false;
    }
    Impl::ExecutionState state;
    state.callback_type = detail::callback_type_id("ui_control");
    state.callback_id = static_cast<std::int32_t>(
            impl->host ? impl->host->reserve_callback_id()
                       : reserve_callback_id());
    state.frames.push_back(
            { Impl::ExecutionFrame::Kind::block, &callback_body->second });
    const auto result = impl->execute(state);
    if (result == Impl::ExecutionResult::suspended)
        impl->store_suspended(std::move(state));
    return result != Impl::ExecutionResult::failed && !impl->has_errors();
}
bool Runtime::resume_wait(std::uint64_t wait_id) {
    const auto waiting = impl->suspended.find(wait_id);
    if (waiting == impl->suspended.end()) {
        impl->diagnostics.push_back({ {}, "wait event not found" });
        return false;
    }

    auto state = std::move(waiting->second);
    impl->suspended.erase(waiting);
    if (impl->host) {
        if (const auto timestamp = impl->host->restore_wait_context(
                    wait_id, state.callback_type)) {
            const auto restored_timestamp =
                    std::max<std::int64_t>(0, *timestamp);
            impl->host->set_callback_timestamp(restored_timestamp);
            if (*timestamp < 0) {
                impl->warn({},
                        "wait(): Faulty timestamp after resuming the callback!");
            }
        }
        if (state.callback_type == 1 || state.callback_type == 2)
            impl->host->refresh_affected_groups(state.callback_type);
    }
    state.wait_id = 0;
    const auto result = impl->execute(state);
    if (result == Impl::ExecutionResult::suspended)
        impl->store_suspended(std::move(state));
    return result != Impl::ExecutionResult::failed && !impl->has_errors();
}
bool Runtime::complete_array_load(
        std::int64_t async_id, Value value, bool succeeded) {
    return impl->complete_array_load(async_id, value, succeeded);
}
bool Runtime::complete_array_save(std::int64_t async_id, bool succeeded) {
    return impl->complete_array_save(async_id, succeeded);
}
bool Runtime::complete_detection(
        std::int64_t async_id, Value value, bool succeeded) {
    return impl->complete_detection(async_id, value, succeeded);
}
bool Runtime::complete_async_value(
        std::int64_t async_id, Value value, bool succeeded) {
    return impl->complete_async_value(async_id, value, succeeded);
}
bool Runtime::complete_engine_command(std::int64_t async_id, bool succeeded) {
    return impl->complete_engine_command(async_id, succeeded);
}
bool Runtime::notify_async_value(std::int64_t type, std::int64_t parameter_one,
        std::int64_t parameter_two, Value value) {
    return notify_async_value(type, std::optional{ parameter_one },
            std::optional{ parameter_two }, std::move(value));
}
bool Runtime::notify_async_value(std::int64_t type,
        std::optional<std::int64_t> parameter_one,
        std::optional<std::int64_t> parameter_two, Value value) {
    return impl->notify_async_value(type, parameter_one, parameter_two, value);
}
void Runtime::import_persistent_variables(
        std::span<const PersistentVariable> variables) {
    impl->pending_persistent_values.assign(variables.begin(), variables.end());
}
std::vector<PersistentVariable> Runtime::export_persistent_variables() const {
    std::vector<PersistentVariable> result;
    result.reserve(impl->persistent_variables.size());
    std::unordered_set<std::string> exported_names;
    const auto export_variable = [&](const std::string &name) {
        const auto persistence = impl->persistent_variables.find(name);
        if (persistence == impl->persistent_variables.end() ||
                !exported_names.insert(name).second)
            return;

        const auto value = impl->variables.find(name);
        if (value != impl->variables.end()) {
            if (const auto selected = impl->ui_menu_selected_indices.find(name);
                    selected != impl->ui_menu_selected_indices.end()) {
                result.push_back(
                        { name, selected->second, persistence->second });
            } else {
                result.push_back({ name, value->second, persistence->second });
            }
        } else if (impl->host) {
            if (const auto external = impl->host->resolve_symbol(name)) {
                const auto selected = impl->ui_menu_selected_indices.find(name);
                result.push_back({ name,
                        selected != impl->ui_menu_selected_indices.end()
                                ? Value{ selected->second }
                                : external->value,
                        persistence->second });
            }
        }
    };

    for (const auto &name : impl->declared_variable_order)
        export_variable(name);
    for (const auto &name : impl->persistent_variable_order)
        export_variable(name);
    return result;
}
void Runtime::set(std::string_view name, Value value) {
    if (detail::is_builtin_variable(name)) {
        impl->supplied_builtin_values[std::string(name)] =
                impl->coerce_scalar(name, value);
        return;
    }
    const auto normalized_name = lowercase(name);
    impl->variables[normalized_name] =
            impl->coerce_scalar(normalized_name, value);
    impl->update_ui_menu_selected_index(
            normalized_name, as_integer(impl->variables[normalized_name]));
}
std::optional<Value> Runtime::get(std::string_view name) const {
    if (name == "$NI_CALLBACK_ID")
        return impl->current_callback_id;
    if (name == "$NI_CALLBACK_TYPE")
        return impl->current_callback_type;
    if (name == "$NI_ASYNC_ID")
        return impl->current_async_id;
    if (name == "$NI_ASYNC_TYPE")
        return impl->current_async_type;
    if (name == "$NI_ASYNC_PAR_1")
        return impl->current_async_parameter_one;
    if (name == "$NI_ASYNC_PAR_2")
        return impl->current_async_parameter_two;
    if (name == "$NI_ASYNC_EXIT_STATUS")
        return impl->current_async_exit_status;
    if (name == "$NI_ASYNC_VALUE")
        return wrap_integer(as_integer(impl->current_async_value));
    if (name == "@NI_ASYNC_STR_VALUE")
        return as_string(impl->current_async_value);
    if (detail::is_builtin_variable(name)) {
        if (const auto symbol =
                        impl->resolve_environment_symbol(lowercase(name), name))
            return symbol->value;
        return std::nullopt;
    }
    auto it = impl->variables.find(lowercase(name));
    if (it != impl->variables.end())
        return it->second;
    if (impl->host) {
        const auto normalized_name = lowercase(name);
        if (const auto symbol = impl->host->resolve_symbol(normalized_name))
            return symbol->value;
    }
    return std::nullopt;
}
const std::vector<Diagnostic> &Runtime::diagnostics() const noexcept {
    return impl->diagnostics;
}
void Runtime::clear_diagnostics() { impl->diagnostics.clear(); }

} // namespace libksp
