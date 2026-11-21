#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "param_table.h"

namespace acp {

struct ParseIssue {
    enum class Kind {
        UnknownKey,    // Key = Value, but not in the parameter table
        Malformed,     // neither Key = Value nor blank line nor section
        Section,       // line "[...]"
        Duplicate,     // key appears more than once
        InvalidValue,  // value does not match the type
        MissingKey,    // table key missing from the file (line = 0)
    };
    Kind kind;
    std::size_t line;  // 1-based, 0 = not tied to a line
    std::string key;
};

struct ParseReport {
    std::vector<ParseIssue> issues;
    bool Has(ParseIssue::Kind k) const noexcept;
};

// Line-preserving model of ACP.ini (flat, no sections).
// Invariant: Serialize(Parse(bytes)) == bytes. Set() changes only the value token.
class IniDocument {
public:
    static IniDocument Parse(std::string_view bytes, ParseReport* report = nullptr);

    std::string Serialize() const;

    // Value of the first line with this key; nullopt if the key is missing or the value is invalid.
    std::optional<ParamValue> Get(ParamId id) const;
    // Raw value token (as it appears in the file).
    std::optional<std::string_view> GetRaw(ParamId id) const;
    bool Contains(ParamId id) const;

    // Sets the value in every line with this key. If the key is missing, "Key = Value" is
    // inserted before the first section line, or at the end. The type must match.
    void Set(ParamId id, const ParamValue& value);

    // Formatting: true/false, decimal ints, compact floats ("6", "0.5"; at most 6 decimals).
    static std::string FormatValue(const ParamValue& value);
    // Lenient parsing of a value token for the given type.
    static std::optional<ParamValue> ParseValue(ParamType type, std::string_view token);

private:
    struct Line {
        std::string text;  // without line ending
        std::string eol;   // "\r\n", "\n", "\r" or "" (last line)
        std::optional<ParamId> param;
        std::size_t valueBegin = 0;  // value token in text (Key = Value lines only)
        std::size_t valueEnd = 0;
        bool isKeyValue = false;
        bool isSection = false;
        bool emptyValue = false;  // "Key =" without a value
    };

    std::string DefaultEol() const;

    std::string bom_;  // UTF-8 BOM, if present (written back unchanged)
    std::vector<Line> lines_;
};

}  // namespace acp
