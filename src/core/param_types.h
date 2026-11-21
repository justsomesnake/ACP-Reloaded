#pragma once

#include <cstdint>

namespace acp {

enum class ParamType : std::uint8_t { Bool, Int, Float };

// The five parameter pages of the UI. "Presets" is not a parameter category.
enum class Category : std::uint8_t { HeroGeneral, HeroFighting, NpcGeneral, NpcFighting, World };

enum class Unit : std::uint8_t { None, Percent, Seconds, Dps, Level, Count, Enum, Unknown };

// How reliable the UI range is (docs/param_map.csv, column Conf).
enum class Confidence : std::uint8_t {
    NotApplicable,  // Bool, no range
    Game,           // type and range from the game's parameter table (verified)
    ExeLabel,       // unit backed by a label in the EXE, range assumed
    Community,      // community claim, unconfirmed
    Assumed,        // derived from the semantics
    Unknown,        // placeholder
};

// Small tagged value; the type must match the ParamDesc.
struct ParamValue {
    ParamType type = ParamType::Int;
    union {
        bool b;
        std::int32_t i;
        float f;
    };

    constexpr ParamValue() noexcept : i(0) {}
    static constexpr ParamValue Bool(bool v) noexcept { ParamValue p; p.type = ParamType::Bool; p.b = v; return p; }
    static constexpr ParamValue Int(std::int32_t v) noexcept { ParamValue p; p.type = ParamType::Int; p.i = v; return p; }
    static constexpr ParamValue Float(float v) noexcept { ParamValue p; p.type = ParamType::Float; p.f = v; return p; }

    friend constexpr bool operator==(const ParamValue& a, const ParamValue& b) noexcept {
        if (a.type != b.type) return false;
        switch (a.type) {
            case ParamType::Bool: return a.b == b.b;
            case ParamType::Int: return a.i == b.i;
            case ParamType::Float: return a.f == b.f;
        }
        return false;
    }
};

enum class ParamId : std::uint16_t {
#include "param_ids.inc"
    Count_
};

inline constexpr std::size_t kParamCount = static_cast<std::size_t>(ParamId::Count_);

// The item is disabled while the Bool parameter `param` has the value `whenValue`.
struct DisabledWhen {
    ParamId param = ParamId::Count_;  // Count_ = no condition
    bool whenValue = false;
    constexpr bool Active() const noexcept { return param != ParamId::Count_; }
};

}  // namespace acp
