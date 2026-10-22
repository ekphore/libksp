// SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "internal.h"

#include <array>
#include <unordered_map>

namespace libksp::detail {

bool is_builtin_variable(std::string_view name) {
    return lookup_builtin_variable_id(name).has_value();
}

std::optional<int> lookup_builtin_variable_id(std::string_view name) {
    static const std::unordered_map<std::string_view, int> variables = {
        { "%CC", 0 },
        { "%CC_TOUCHED", 1 },
        { "$CC_NUM", 5 },
        { "$EVENT_NOTE", 2 },
        { "$EVENT_ID", 6 },
        { "%KEY_DOWN", 12 },
        { "$NOTE_HELD", 11 },
        { "%KEY_DOWN_OCT", 13 },
        { "%EVENT_PAR", 7 },
        { "%GROUPS_AFFECTED", 8 },
        { "%NOTE_DURATION", 14 },
        { "$ENGINE_UPTIME", 32 },
        { "$EVENT_VELOCITY", 3 },
        { "$EVENT_VELOCITY_2", 4 },
        { "$DISTANCE_BAR_START", 15 },
        { "$SIGNATURE_NUM", 17 },
        { "$SIGNATURE_DENOM", 18 },
        { "$DURATION_BAR", 16 },
        { "$DURATION_QUARTER", 19 },
        { "$DURATION_EIGHTH", 20 },
        { "$DURATION_SIXTEENTH", 21 },
        { "$DURATION_QUARTER_TRIPLET", 22 },
        { "$DURATION_EIGHTH_TRIPLET", 23 },
        { "$DURATION_SIXTEENTH_TRIPLET", 24 },
        { "$NUM_GROUPS", 46 },
        { "$NUM_ZONES", 47 },
        { "$NUM_OUTPUT_CHANNELS", 48 },
        { "$KSP_TIMER", 53 },
        { "$CURRENT_SCRIPT_SLOT", 54 },
        { "%POLY_AT", 55 },
        { "$POLY_AT_NUM", 56 },
        { "$NI_SONG_POSITION", 25 },
        { "$NI_BAR_START_POSITION", 26 },
        { "$NI_SIGNATURE_TYPE", 27 },
        { "$NI_CALLBACK_ID", 28 },
        { "$NI_CALLBACK_TYPE", 29 },
        { "$NI_TRANSPORT_ACTIVE", 30 },
        { "$NI_TRANSPORT_RUNNING", 31 },
        { "$NI_DATE_YEAR", 33 },
        { "$NI_DATE_MONTH", 34 },
        { "$NI_DATE_DAY", 35 },
        { "$NI_TIME_HOUR", 36 },
        { "$NI_TIME_MINUTE", 37 },
        { "$NI_TIME_SECOND", 38 },
        { "$MIDI_CHANNEL", 39 },
        { "$MIDI_COMMAND", 40 },
        { "$MIDI_BYTE_1", 41 },
        { "$MIDI_BYTE_2", 42 },
        { "%SLOTS_AFFECTED", 9 },
        { "%GROUPS_SELECTED", 10 },
        { "$REF_GROUP_IDX", 49 },
        { "$REF_SLOT_IDX", 50 },
        { "$RPN_ADDRESS", 43 },
        { "$RPN_VALUE", 44 },
        { "%SLOT_TYPE", 45 },
        { "$PLAYED_VOICES_TOTAL", 51 },
        { "$PLAYED_VOICES_INST", 52 },
        { "$NI_ASYNC_ID", 57 },
        { "$NI_ASYNC_EXIT_STATUS", 63 },
        { "$NI_ASYNC_VALUE", 61 },
        { "@NI_ASYNC_STR_VALUE", 62 },
        { "$NI_ASYNC_TYPE", 58 },
        { "$NI_ASYNC_PAR_1", 59 },
        { "$NI_ASYNC_PAR_2", 60 },
        { "$NI_CONTROL_PAR_IDX", 64 },
        { "$NI_MOUSE_EVENT_TYPE", 65 },
        // this is our semantic-table ID, not the native name-map ID
        { "$NI_SIGNAL_TYPE", 66 },
        { "$NI_MOUSE_OVER_CONTROL", 67 },
        { "!NI_DND_ITEMS_AUDIO", 68 },
        { "!NI_DND_ITEMS_MIDI", 69 },
        { "!NI_DND_ITEMS_ARRAY", 70 },
        { "%NI_USER_ZONE_IDS", 71 },
        { "$NI_UI_ID", 72 },
        { "$NI_SLOT_IDX", 76 },
        { "$NI_LEAP_DEFAULT_START_KEY", 77 },
        { "$NC_VALUE", 75 },
        { "$NC_NOTE", 74 },
        { "$NC_NUM", 73 },
    };
    const auto variable = variables.find(name);
    if (variable == variables.end())
        return std::nullopt;
    return variable->second;
}

bool is_runtime_managed_variable(std::string_view name) {
    return name == "$NI_CALLBACK_ID" || name == "$NI_CALLBACK_TYPE";
}

std::optional<std::int64_t> lookup_builtin_integer_constant(
        std::string_view name) {
    static const std::unordered_map<std::string_view, std::int64_t>
            constants = {
#include "integer_constants.inc"
            };
    const auto constant = constants.find(name);
    if (constant == constants.end())
        return std::nullopt;
    return constant->second;
}

std::optional<double> lookup_builtin_real_constant(std::string_view name) {
    if (name == "~NI_MATH_PI")
        return 3.14159265358979323846;
    if (name == "~NI_MATH_E")
        return 2.71828182845904523536;
    if (name == "~NI_DETECT_PITCH_INVALID" ||
            name == "~NI_DETECT_LOUDNESS_INVALID" ||
            name == "~NI_DETECT_PEAK_INVALID" ||
            name == "~NI_DETECT_RMS_INVALID")
        return 1'000'000.0;
    if (name == "~NI_DETECT_TEMPO_INVALID")
        return -1.0;
    return std::nullopt;
}

bool is_valid_engine_parameter_index(std::int64_t index) {
    static constexpr std::array<std::uint64_t, 16> valid_indices = {
        0xf26000801c0191c0ULL, 0x79f279f3987f9fccULL, 0xc9e79fd39f8f1cfeULL,
        0x01cffe0000001021ULL, 0xffffffe000380000ULL, 0xffffffffffffffffULL,
        0xffffffffffffffffULL, 0x01ffffffffffffffULL, 0x0000fffffffc0000ULL,
        0xffffffffff000000ULL, 0xf004017fc00ffc03ULL, 0x00000000000f83cfULL,
        0x0000000000000000ULL, 0xfc7f000000000000ULL, 0x000003000000000fULL,
        0x00001f0000000000ULL
    };
    if (index < 0 || index > 0x3ff)
        return false;
    const auto unsigned_index = static_cast<std::uint64_t>(index);
    return (valid_indices[unsigned_index / 64] &
                   (std::uint64_t{ 1 } << (unsigned_index % 64))) != 0;
}

} // namespace libksp::detail
