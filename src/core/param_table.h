#pragma once

#include <array>
#include <optional>
#include <string_view>

#include "param_types.h"

namespace acp {

struct ParamDesc {
    ParamId id;
    std::string_view key;    // exactly as in ACP.ini
    std::string_view label;  // display name (original English label)
    std::string_view group;  // group heading within the category
    ParamType type;
    Category category;
    Unit unit;
    Confidence confidence;
    ParamValue def;    // factory default (= reference file)
    ParamValue uiMin;  // slider range; reliability given by confidence
    ParamValue uiMax;
    std::array<DisabledWhen, 2> disabledWhen;  // OR-combined
    bool readOnly;      // status value / no effect: not settable, hidden in the UI
    bool reloadsWorld;  // change makes the game reload the world (send only batched via "Apply")
    bool local;         // applied only by ACP Reloaded (own image filter), never sent to the game
    std::string_view hint;  // tooltip text (empty = none)
};

// All 109 parameters, in the order of the file the game writes.
const std::array<ParamDesc, kParamCount>& ParamTable() noexcept;

inline const ParamDesc& Desc(ParamId id) noexcept { return ParamTable()[static_cast<std::size_t>(id)]; }

// Exact, case-sensitive match, as in the game's format.
std::optional<ParamId> FindParam(std::string_view key) noexcept;

// true if the value lies within the (assumed) UI range. Always true for Bool.
bool InUiRange(const ParamDesc& desc, const ParamValue& value) noexcept;

}  // namespace acp
