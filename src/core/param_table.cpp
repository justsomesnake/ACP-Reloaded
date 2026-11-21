#include "param_table.h"

#include <algorithm>

namespace acp {
namespace {

constexpr std::array<ParamDesc, kParamCount> kTable{{
#include "param_table_data.inc"
}};

// Table order must match the ParamId enumeration.
constexpr bool IdsInOrder() {
    for (std::size_t i = 0; i < kTable.size(); ++i) {
        if (static_cast<std::size_t>(kTable[i].id) != i) return false;
    }
    return true;
}
static_assert(IdsInOrder(), "param_table_data.inc and param_ids.inc are out of sync");

}  // namespace

const std::array<ParamDesc, kParamCount>& ParamTable() noexcept { return kTable; }

std::optional<ParamId> FindParam(std::string_view key) noexcept {
    const auto it = std::find_if(kTable.begin(), kTable.end(), [key](const ParamDesc& d) { return d.key == key; });
    if (it == kTable.end()) return std::nullopt;
    return it->id;
}

bool InUiRange(const ParamDesc& desc, const ParamValue& value) noexcept {
    if (value.type != desc.type) return false;
    switch (desc.type) {
        case ParamType::Bool: return true;
        case ParamType::Int: return value.i >= desc.uiMin.i && value.i <= desc.uiMax.i;
        case ParamType::Float: return value.f >= desc.uiMin.f && value.f <= desc.uiMax.f;
    }
    return false;
}

}  // namespace acp
