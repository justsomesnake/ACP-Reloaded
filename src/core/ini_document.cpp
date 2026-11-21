#include "ini_document.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>

namespace acp {
namespace {

constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

bool IsSpace(char c) noexcept { return c == ' ' || c == '\t'; }
bool IsKeyChar(char c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

template <typename T>
bool ParseWhole(std::string_view s, T& out) noexcept {
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);
    if (s.empty()) return false;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && ptr == s.data() + s.size();
}

}  // namespace

bool ParseReport::Has(ParseIssue::Kind k) const noexcept {
    return std::any_of(issues.begin(), issues.end(), [k](const ParseIssue& i) { return i.kind == k; });
}

std::optional<ParamValue> IniDocument::ParseValue(ParamType type, std::string_view token) {
    switch (type) {
        case ParamType::Bool:
            if (EqualsIgnoreCase(token, "true") || token == "1") return ParamValue::Bool(true);
            if (EqualsIgnoreCase(token, "false") || token == "0") return ParamValue::Bool(false);
            return std::nullopt;
        case ParamType::Int: {
            std::int32_t i = 0;
            if (ParseWhole(token, i)) return ParamValue::Int(i);
            // Lenient: integral float text such as "100.000000".
            double d = 0;
            if (ParseWhole(token, d) && std::isfinite(d) && d == std::floor(d) &&
                d >= std::numeric_limits<std::int32_t>::min() && d <= std::numeric_limits<std::int32_t>::max()) {
                return ParamValue::Int(static_cast<std::int32_t>(d));
            }
            return std::nullopt;
        }
        case ParamType::Float: {
            float f = 0;
            if (ParseWhole(token, f) && std::isfinite(f)) return ParamValue::Float(f);
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::string IniDocument::FormatValue(const ParamValue& value) {
    switch (value.type) {
        case ParamType::Bool: return value.b ? "true" : "false";
        case ParamType::Int: {
            std::array<char, 16> buf{};
            const auto res = std::to_chars(buf.data(), buf.data() + buf.size(), value.i);
            return std::string(buf.data(), res.ptr);
        }
        case ParamType::Float: {
            std::array<char, 64> buf{};
            const float f = (value.f == 0.0f) ? 0.0f : value.f;  // no "-0"
            const auto res = std::to_chars(buf.data(), buf.data() + buf.size(), f, std::chars_format::fixed, 6);
            std::string s(buf.data(), res.ptr);
            // Compact: strip trailing zeros and the point ("6.000000" -> "6", "0.500000" -> "0.5").
            if (s.find('.') != std::string::npos) {
                while (!s.empty() && s.back() == '0') s.pop_back();
                if (!s.empty() && s.back() == '.') s.pop_back();
            }
            return s;
        }
    }
    return {};
}

IniDocument IniDocument::Parse(std::string_view bytes, ParseReport* report) {
    IniDocument doc;
    if (bytes.substr(0, kUtf8Bom.size()) == kUtf8Bom) {
        doc.bom_ = std::string(kUtf8Bom);
        bytes.remove_prefix(kUtf8Bom.size());
    }

    // Split into lines, keeping the exact line endings.
    std::size_t start = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const char c = bytes[i];
        if (c != '\r' && c != '\n') continue;
        Line line;
        line.text = std::string(bytes.substr(start, i - start));
        if (c == '\r' && i + 1 < bytes.size() && bytes[i + 1] == '\n') {
            line.eol = "\r\n";
            ++i;
        } else {
            line.eol = std::string(1, c);
        }
        doc.lines_.push_back(std::move(line));
        start = i + 1;
    }
    if (start < bytes.size()) {
        Line line;
        line.text = std::string(bytes.substr(start));
        doc.lines_.push_back(std::move(line));
    }

    std::array<bool, kParamCount> seen{};
    auto add = [report](ParseIssue::Kind kind, std::size_t lineNo, std::string_view key) {
        if (report) report->issues.push_back({kind, lineNo, std::string(key)});
    };

    for (std::size_t n = 0; n < doc.lines_.size(); ++n) {
        Line& line = doc.lines_[n];
        const std::string_view t = line.text;
        const std::size_t lineNo = n + 1;

        std::size_t p = 0;
        while (p < t.size() && IsSpace(t[p])) ++p;
        if (p == t.size()) continue;  // blank line
        if (t[p] == '[') {
            line.isSection = true;
            add(ParseIssue::Kind::Section, lineNo, t.substr(p));
            continue;
        }

        const std::size_t keyBegin = p;
        while (p < t.size() && IsKeyChar(t[p])) ++p;
        const std::size_t keyEnd = p;
        while (p < t.size() && IsSpace(t[p])) ++p;
        if (keyEnd == keyBegin || p == t.size() || t[p] != '=') {
            add(ParseIssue::Kind::Malformed, lineNo, t);
            continue;
        }
        const std::size_t eqPos = p++;
        while (p < t.size() && IsSpace(t[p])) ++p;
        std::size_t valueEnd = t.size();
        while (valueEnd > p && IsSpace(t[valueEnd - 1])) --valueEnd;

        line.isKeyValue = true;
        line.valueBegin = p;
        line.valueEnd = valueEnd;
        if (valueEnd == p) {
            line.emptyValue = true;
            line.valueBegin = eqPos + 1;
            line.valueEnd = t.size();
        }

        const std::string_view key = t.substr(keyBegin, keyEnd - keyBegin);
        const auto id = FindParam(key);
        if (!id) {
            add(ParseIssue::Kind::UnknownKey, lineNo, key);
            continue;
        }
        line.param = id;
        const auto idx = static_cast<std::size_t>(*id);
        if (seen[idx]) add(ParseIssue::Kind::Duplicate, lineNo, key);
        seen[idx] = true;
        if (line.emptyValue || !ParseValue(Desc(*id).type, t.substr(line.valueBegin, line.valueEnd - line.valueBegin))) {
            add(ParseIssue::Kind::InvalidValue, lineNo, key);
        }
    }

    for (const ParamDesc& d : ParamTable()) {
        if (!seen[static_cast<std::size_t>(d.id)]) add(ParseIssue::Kind::MissingKey, 0, d.key);
    }
    return doc;
}

std::string IniDocument::Serialize() const {
    std::string out = bom_;
    for (const Line& l : lines_) {
        out += l.text;
        out += l.eol;
    }
    return out;
}

std::optional<std::string_view> IniDocument::GetRaw(ParamId id) const {
    for (const Line& l : lines_) {
        if (l.param == id) {
            if (l.emptyValue) return std::string_view{};
            return std::string_view(l.text).substr(l.valueBegin, l.valueEnd - l.valueBegin);
        }
    }
    return std::nullopt;
}

bool IniDocument::Contains(ParamId id) const {
    return std::any_of(lines_.begin(), lines_.end(), [id](const Line& l) { return l.param == id; });
}

std::optional<ParamValue> IniDocument::Get(ParamId id) const {
    const auto raw = GetRaw(id);
    if (!raw) return std::nullopt;
    return ParseValue(Desc(id).type, *raw);
}

std::string IniDocument::DefaultEol() const {
    for (const Line& l : lines_) {
        if (!l.eol.empty()) return l.eol;
    }
    return "\r\n";
}

void IniDocument::Set(ParamId id, const ParamValue& value) {
    const ParamDesc& desc = Desc(id);
    if (value.type != desc.type) return;  // programming error; file stays unchanged
    const std::string token = FormatValue(value);

    bool found = false;
    for (Line& l : lines_) {
        if (l.param != id) continue;
        found = true;
        if (l.emptyValue) {
            l.text.replace(l.valueBegin, l.valueEnd - l.valueBegin, " " + token);
            l.valueBegin += 1;
            l.valueEnd = l.valueBegin + token.size();
            l.emptyValue = false;
        } else {
            l.text.replace(l.valueBegin, l.valueEnd - l.valueBegin, token);
            l.valueEnd = l.valueBegin + token.size();
        }
    }
    if (found) return;

    const std::string eol = DefaultEol();
    Line line;
    line.text = std::string(desc.key) + " = ";
    line.valueBegin = line.text.size();
    line.text += token;
    line.valueEnd = line.text.size();
    line.eol = eol;
    line.param = id;
    line.isKeyValue = true;

    const auto section = std::find_if(lines_.begin(), lines_.end(), [](const Line& l) { return l.isSection; });
    if (section == lines_.end() && !lines_.empty() && lines_.back().eol.empty()) lines_.back().eol = eol;
    lines_.insert(section, std::move(line));
}

}  // namespace acp
