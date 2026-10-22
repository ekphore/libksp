// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace libksp {

struct SourceLocation {
    std::size_t offset{}, line{ 1 }, column{ 1 };
};

enum class TokenKind : std::uint8_t {
    eof,
    newline,
    identifier,
    variable,
    integer,
    real,
    string,
    lparen,
    rparen,
    lbracket,
    rbracket,
    comma,
    assign,
    plus,
    minus,
    star,
    slash,
    mod,
    concat,
    bit_and,
    bit_or,
    bit_xor,
    shift_left,
    shift_right,
    logical_not,
    logical_and,
    logical_or,
    logical_xor,
    equal,
    not_equal,
    less,
    less_equal,
    greater,
    greater_equal,
    unknown
};

struct Token {
    TokenKind kind{ TokenKind::eof };
    std::string text;
    SourceLocation location;
};

enum class DiagnosticSeverity : std::uint8_t { error, warning };

struct Diagnostic {
    SourceLocation location;
    std::string message;
    DiagnosticSeverity severity{ DiagnosticSeverity::error };
};

[[nodiscard]] std::vector<Token> lex(std::string_view source,
        std::vector<Diagnostic> *diagnostics = nullptr);

// bounds are inclusive, reversed ones get swapped. this uses the same libc
// rand state as Runtime so a cloned runtime still shares it
[[nodiscard]] std::int32_t random_integer(
        std::int32_t lower, std::int32_t upper);
// IDs never go back to the pool, hosts can journal them for replay
[[nodiscard]] std::uint32_t reserve_callback_id();
[[nodiscard]] std::uint64_t reserve_wait_id();

using Value = std::variant<std::int64_t, double, std::string,
        std::vector<std::int64_t>, std::vector<double>,
        std::vector<std::string>>;

struct ArrayLoadResult {
    std::int64_t async_id{ -1 };
    std::optional<Value> loaded_value;
};

struct DetectionResult {
    std::int64_t async_id{ -1 };
    std::optional<Value> detected_value;
};

struct ControllerPages {
    std::vector<std::string> group_names;
    std::vector<std::int64_t> parameter_ids;
    std::vector<std::string> assignment_names;
};

struct EnvironmentSymbol {
    Value value{ std::int64_t{ 0 } };
    bool writable{ true };
    bool ui_control{};
    std::string ui_type;
};

enum class PersistenceMode : std::uint8_t { snapshot, instrument };

struct PersistentVariable {
    std::string name;
    Value value{ std::int64_t{ 0 } };
    PersistenceMode mode{ PersistenceMode::snapshot };
};

enum class WaitUnit : std::uint8_t {
    microseconds,
    ticks,
    async_completion,
    immediate
};

struct PolyphonicEventContext {
    std::int64_t index{};
    std::optional<std::int64_t> source_index;
};

class Host {
    // the instrument's script parsers share these conditions
    std::set<std::string, std::less<>> script_conditions;

  public:
    virtual ~Host() = default;
    void define_script_condition(std::string name) {
        script_conditions.insert(std::move(name));
    }
    void undefine_script_condition(std::string_view name) {
        const auto found = script_conditions.find(name);
        if (found != script_conditions.end())
            script_conditions.erase(found);
    }
    bool script_condition_defined(std::string_view name) const {
        return script_conditions.contains(name);
    }
    // the default burns three libc rand calls and cloning doesn't copy that
    // state, so override this if a clone needs its own random source
    virtual std::int32_t random_integer(
            std::int32_t lower, std::int32_t upper) {
        return libksp::random_integer(lower, upper);
    }
    virtual std::uint32_t reserve_callback_id() {
        return libksp::reserve_callback_id();
    }
    virtual std::uint64_t reserve_wait_id() {
        return libksp::reserve_wait_id();
    }
    virtual std::optional<EnvironmentSymbol> resolve_symbol(std::string_view) {
        return std::nullopt;
    }
    virtual bool assign_symbol(std::string_view, const Value &) {
        return false;
    }
    virtual void ui_control_value_changed(std::string_view, const Value &) {}
    // two entries can share a payload, so you get the index too
    virtual void ui_menu_selection_changed(
            std::string_view name, std::int64_t, const Value &value) {
        ui_control_value_changed(name, value);
    }
    virtual bool defer_unknown_ui_control_types() { return false; }
    virtual bool is_multi_script() { return false; }
    virtual std::optional<std::int64_t> group_count() { return std::nullopt; }
    virtual std::optional<std::int64_t> zone_count() { return std::nullopt; }
    virtual std::optional<bool> zone_exists(std::int64_t) {
        return std::nullopt;
    }
    virtual std::optional<bool> zone_is_user(std::int64_t) {
        return std::nullopt;
    }
    virtual std::optional<std::int64_t> snapshot_type() { return std::nullopt; }
    virtual std::optional<bool> output_destination_available(
            std::int64_t, std::int64_t) {
        return std::nullopt;
    }
    virtual std::optional<ControllerPages> get_controller_pages() {
        return std::nullopt;
    }
    virtual std::optional<bool> midi_redirect_target_exists(
            std::int64_t, std::int64_t) {
        return std::nullopt;
    }
    virtual void watched_variable_changed(
            std::string_view, std::optional<std::int64_t>, const Value &) {}
    virtual std::optional<std::int64_t> start_engine_command(
            std::string_view name, std::span<const Value> arguments, bool) {
        const auto result = call(name, arguments);
        if (result) {
            if (const auto value = std::get_if<std::int64_t>(&*result))
                return *value;
        }
        return std::nullopt;
    }
    virtual std::optional<double> current_tempo_bpm() { return std::nullopt; }
    virtual std::optional<double> current_sample_rate_hz() {
        return std::nullopt;
    }
    virtual std::optional<std::int64_t> ui_height_grid() {
        return std::nullopt;
    }
    virtual std::optional<std::int64_t> ui_menu_item_count(std::string_view) {
        return std::nullopt;
    }
    virtual std::optional<std::int64_t> ui_table_element_count(
            std::string_view) {
        return std::nullopt;
    }
    virtual void declare_ui_control(std::string_view, std::string_view,
            std::int64_t, std::optional<std::int64_t>,
            std::span<const std::int64_t>) {}
    virtual std::optional<std::pair<std::int64_t, std::int64_t>>
    ui_control_value_range(std::string_view) {
        return std::nullopt;
    }
    virtual std::optional<std::string> ui_control_type(std::int64_t) {
        return std::nullopt;
    }
    virtual void make_performance_view() {}
    virtual void expose_controls() {}
    virtual void show_library_tab() {}
    virtual void hide_instrument_icon() {}
    virtual std::optional<bool> load_performance_view(std::string_view) {
        return std::nullopt;
    }
    virtual void load_native_ui(std::string_view) {}
    virtual void delay_event_for_loading_slots() {}
    virtual bool is_async_active(std::int64_t) { return false; }
    virtual void schedule_wait(std::uint64_t, std::int64_t, WaitUnit) {}
    virtual void cancel_wait(std::uint64_t) {}
    virtual std::optional<std::int64_t> restore_wait_context(
            std::uint64_t, std::int64_t) {
        return std::nullopt;
    }
    virtual void set_callback_timestamp(std::int64_t) {}
    virtual void refresh_affected_groups(std::int64_t) {}
    virtual std::optional<PolyphonicEventContext> polyphonic_event_context() {
        return std::nullopt;
    }
    // you get the declared spelling since NKA headers and paths care about
    // case, lookup and async completion still fold it
    virtual std::optional<ArrayLoadResult> load_array_file(
            std::string_view, const Value &, const Value &, bool) {
        return std::nullopt;
    }
    virtual std::optional<std::int64_t> save_array_file(
            std::string_view, const Value &, const Value &) {
        return std::nullopt;
    }
    virtual std::optional<DetectionResult> detect_audio(
            std::string_view, std::int64_t, bool) {
        return std::nullopt;
    }
    virtual std::optional<std::int64_t> request_async_value(
            std::int64_t, std::int64_t, std::int64_t, bool) {
        return std::nullopt;
    }
    virtual void subscribe_async_value(
            std::int64_t, std::int64_t, std::int64_t) {}
    virtual std::optional<std::int64_t> set_async_value(
            std::int64_t, std::int64_t, std::int64_t, const Value &, bool) {
        return std::nullopt;
    }
    virtual std::optional<Value> call(
            std::string_view name, std::span<const Value> arguments) = 0;
};

class Runtime {
  public:
    explicit Runtime(Host *host = nullptr);
    ~Runtime();
    Runtime(Runtime &&) noexcept;
    Runtime &operator=(Runtime &&) noexcept;
    Runtime(const Runtime &) = delete;
    Runtime &operator=(const Runtime &) = delete;

    // NOT from the realtime thread, and the source must not be running. code
    // is shared, variables/frames/pending operations get copied. we don't
    // touch the host at all, so you have to give us a separate one with its
    // wait/async state already copied, and it has to outlive the clone.
    // cloning during a callback or onto the same host throws
    [[nodiscard]] Runtime clone_state(Host &independent_host) const;

    [[nodiscard]] bool load(std::string_view source);
    [[nodiscard]] bool has_callback(std::string_view callback) const;
    [[nodiscard]] bool has_callback(std::string_view callback,
            std::string_view callback_variable) const;
    [[nodiscard]] bool has_ui_control(std::string_view name) const;
    [[nodiscard]] bool run(std::string_view callback);
    [[nodiscard]] bool run(
            std::string_view callback, std::string_view callback_variable);
    [[nodiscard]] bool resume_wait(std::uint64_t wait_id);
    [[nodiscard]] bool complete_array_load(
            std::int64_t async_id, Value value, bool succeeded = true);
    [[nodiscard]] bool complete_array_save(
            std::int64_t async_id, bool succeeded);
    [[nodiscard]] bool complete_detection(
            std::int64_t async_id, Value value, bool succeeded = true);
    [[nodiscard]] bool complete_async_value(
            std::int64_t async_id, Value value, bool succeeded = true);
    [[nodiscard]] bool complete_engine_command(
            std::int64_t async_id, bool succeeded = true);
    [[nodiscard]] bool notify_async_value(std::int64_t type,
            std::int64_t parameter_one, std::int64_t parameter_two,
            Value value);
    [[nodiscard]] bool notify_async_value(std::int64_t type,
            std::optional<std::int64_t> parameter_one,
            std::optional<std::int64_t> parameter_two, Value value);
    [[nodiscard]] bool run_init() { return run("init"); }
    void import_persistent_variables(
            std::span<const PersistentVariable> variables);
    [[nodiscard]] std::vector<PersistentVariable>
    export_persistent_variables() const;
    void set(std::string_view name, Value value);
    [[nodiscard]] std::optional<Value> get(std::string_view name) const;
    [[nodiscard]] const std::vector<Diagnostic> &diagnostics() const noexcept;
    void clear_diagnostics();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace libksp
