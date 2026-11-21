// acp_tests - unit tests for acp_core (no test framework, run via CTest).
#include <clocale>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

#include "ini_document.h"
#include "test_main.h"
#include "param_table.h"

using namespace acp;

namespace {

std::string ReadFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

const std::string& Reference() {
    static const std::string bytes = ReadFile(ACP_REFERENCE_INI);
    return bytes;
}

std::vector<std::string> SplitCrlf(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0, pos;
    while ((pos = s.find("\r\n", start)) != std::string::npos) {
        out.push_back(s.substr(start, pos - start));
        start = pos + 2;
    }
    return out;
}

std::size_t CountDiffBytes(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return static_cast<std::size_t>(-1);
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.size(); ++i) n += (a[i] != b[i]);
    return n;
}

// ---------------------------------------------------------------------------

TEST(ReferenceFileIsPresent) {
    CHECK(Reference().size() == 3518);
}

TEST(RoundtripReferenceIsByteIdentical) {
    ParseReport rep;
    const auto doc = IniDocument::Parse(Reference(), &rep);
    CHECK(doc.Serialize() == Reference());
    CHECK(rep.issues.empty());
}

TEST(TableMatchesReferenceKeyTypeDefaultAndOrder) {
    const auto lines = SplitCrlf(Reference());
    CHECK(lines.size() == kParamCount);
    static_assert(kParamCount == 109);
    const auto doc = IniDocument::Parse(Reference());
    for (std::size_t i = 0; i < lines.size() && i < kParamCount; ++i) {
        const ParamDesc& d = ParamTable()[i];
        const auto eq = lines[i].find(" = ");
        CHECK(eq != std::string::npos);
        const std::string key = lines[i].substr(0, eq);
        const std::string tok = lines[i].substr(eq + 3);
        CHECK(d.key == key);
        const bool isBoolToken = tok == "true" || tok == "false";
        CHECK((d.type == ParamType::Bool) == isBoolToken);
        const auto v = doc.Get(d.id);
        CHECK(v.has_value());
        if (!v) continue;
        if (d.type == ParamType::Float) {
            // The game writes decimals truncated ("%d"); status values are not compared.
            if (!d.readOnly) CHECK(std::trunc(d.def.f) == v->f);
        } else {
            CHECK(*v == d.def);
            CHECK(IniDocument::FormatValue(d.def) == tok);  // default exactly in the game's format
        }
    }
}

TEST(TypeCounts) {
    int b = 0, i = 0, f = 0;
    for (const auto& d : ParamTable()) {
        b += d.type == ParamType::Bool;
        i += d.type == ParamType::Int;
        f += d.type == ParamType::Float;
    }
    CHECK(b == 20);
    CHECK(i == 77);
    CHECK(f == 12);
    int ro = 0;
    for (const auto& d : ParamTable()) ro += d.readOnly;
    CHECK(ro == 3);  // DayNightTime, DamageInSandstorm (status values), PostProcessCustomFilterIndex (no effect)
    int reload = 0, local = 0;
    for (const auto& d : ParamTable()) {
        reload += d.reloadsWorld;
        local += d.local;
    }
    CHECK(reload == 11);
    CHECK(local == 6);
    CHECK(Desc(ParamId::PlayerHealthFactor).reloadsWorld && Desc(ParamId::PostProcessCustomTint).local);
    CHECK(Desc(ParamId::DayNightTime).readOnly && Desc(ParamId::DamageInSandstorm).readOnly);
}

TEST(CategoryCountsAndRules) {
    int counts[5] = {};
    int rules = 0;
    for (const auto& d : ParamTable()) {
        ++counts[static_cast<int>(d.category)];
        bool hasRule = false;
        for (const auto& w : d.disabledWhen) {
            if (!w.Active()) continue;
            hasRule = true;
            CHECK(Desc(w.param).type == ParamType::Bool);
            CHECK(w.param != d.id);
        }
        rules += hasRule;
        CHECK(InUiRange(d, d.def));
        if (d.type != ParamType::Bool) CHECK(d.uiMin.type == d.type && d.uiMax.type == d.type);
    }
    CHECK(counts[static_cast<int>(Category::HeroGeneral)] == 19);
    CHECK(counts[static_cast<int>(Category::HeroFighting)] == 18);
    CHECK(counts[static_cast<int>(Category::NpcGeneral)] == 14);
    CHECK(counts[static_cast<int>(Category::NpcFighting)] == 18);
    CHECK(counts[static_cast<int>(Category::World)] == 40);
    CHECK(rules == 24);  // custom filter values depend on their own switch (a setting), not on a parameter
}

TEST(TableMatchesCsvKeys) {
    const auto csv = ReadFile(ACP_PARAM_CSV);
    std::istringstream in(csv);
    std::string line;
    std::getline(in, line);  // header line
    std::size_t n = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const std::string key = line.substr(0, line.find(';'));
        CHECK(n < kParamCount && ParamTable()[n].key == key);
        ++n;
    }
    CHECK(n == kParamCount);
}

TEST(FindParamByKey) {
    CHECK(FindParam("PlayerMaxLevel") == ParamId::PlayerMaxLevel);
    CHECK(!FindParam("playermaxlevel").has_value());
    CHECK(!FindParam("DoesNotExist").has_value());
}

TEST(SetChangesOnlyValueToken) {
    auto doc = IniDocument::Parse(Reference());
    doc.Set(ParamId::PlayerMovementSpeedMultiplier, ParamValue::Int(300));
    const auto changed = doc.Serialize();
    CHECK(CountDiffBytes(changed, Reference()) == 1);  // "100" -> "300"
    CHECK(changed.find("\r\nPlayerMovementSpeedMultiplier = 300\r\n") != std::string::npos);
    CHECK(doc.Get(ParamId::PlayerMovementSpeedMultiplier) == ParamValue::Int(300));

    doc.Set(ParamId::PlayerMovementSpeedMultiplier, ParamValue::Int(100));
    CHECK(doc.Serialize() == Reference());
}

TEST(SetLengthChangingValuesAndBool) {
    auto doc = IniDocument::Parse(Reference());
    doc.Set(ParamId::PlayerHealthFactor, ParamValue::Int(1000));
    doc.Set(ParamId::WorldMapUnfogged, ParamValue::Bool(true));
    doc.Set(ParamId::RebelsLevelScale, ParamValue::Int(-5));
    doc.Set(ParamId::DamageInSandstorm, ParamValue::Float(2.5f));
    const auto s = doc.Serialize();
    CHECK(s.find("\r\nPlayerHealthFactor = 1000\r\n") != std::string::npos);
    CHECK(s.find("\r\nWorldMapUnfogged = true\r\n") != std::string::npos);
    CHECK(s.find("\r\nRebelsLevelScale = -5\r\n") != std::string::npos);
    CHECK(s.find("\r\nDamageInSandstorm = 2.5\r\n") != std::string::npos);
    // +1 (100->1000), -1 (false->true), +1 (0->-5), -5 (0.000000->2.5)
    CHECK(s.size() == Reference().size() - 4);
    // Readable again and stable
    const auto again = IniDocument::Parse(s);
    CHECK(again.Serialize() == s);
    CHECK(again.Get(ParamId::RebelsLevelScale) == ParamValue::Int(-5));
}

TEST(WrongTypeIsIgnored) {
    auto doc = IniDocument::Parse(Reference());
    doc.Set(ParamId::PlayerMaxLevel, ParamValue::Bool(true));
    CHECK(doc.Serialize() == Reference());
}

TEST(UnknownAndMalformedLinesArePreserved) {
    const std::string in =
        "PlayerMaxLevel = 40\r\n"
        "SomeFutureKey = 7\r\n"
        "\r\n"
        "; Kommentar\r\n"
        "garbage line\r\n"
        "  PlayerHealthFactor\t=   150  \r\n"
        "[AnimusControlPanel Settings]\r\n"
        "ActivePreset = 2\r\n";
    ParseReport rep;
    auto doc = IniDocument::Parse(in, &rep);
    CHECK(doc.Serialize() == in);
    CHECK(rep.Has(ParseIssue::Kind::UnknownKey));
    CHECK(rep.Has(ParseIssue::Kind::Malformed));
    CHECK(rep.Has(ParseIssue::Kind::Section));
    CHECK(rep.Has(ParseIssue::Kind::MissingKey));
    CHECK(doc.Get(ParamId::PlayerHealthFactor) == ParamValue::Int(150));

    doc.Set(ParamId::PlayerHealthFactor, ParamValue::Int(200));
    const auto s = doc.Serialize();
    CHECK(s.find("  PlayerHealthFactor\t=   200  \r\n") != std::string::npos);  // only the token changed

    // A missing key is inserted before the first section.
    doc.Set(ParamId::WorldMapUnfogged, ParamValue::Bool(true));
    const auto s2 = doc.Serialize();
    const auto ins = s2.find("WorldMapUnfogged = true\r\n");
    CHECK(ins != std::string::npos && ins < s2.find("[AnimusControlPanel Settings]"));
}

TEST(LfFileReadAndWrittenWithLf) {
    std::string lf = Reference();
    std::string out;
    for (char c : lf)
        if (c != '\r') out += c;
    auto doc = IniDocument::Parse(out);
    CHECK(doc.Serialize() == out);
    CHECK(doc.Get(ParamId::PlayerMaxLevel) == ParamValue::Int(40));
    doc.Set(ParamId::PlayerMaxLevel, ParamValue::Int(55));
    CHECK(doc.Serialize().find("\nPlayerMaxLevel = 55\n") != std::string::npos);
    CHECK(doc.Serialize().find('\r') == std::string::npos);
}

TEST(MissingKeyAppendedWithCrlfEvenWithoutTrailingNewline) {
    auto doc = IniDocument::Parse("PlayerMaxLevel = 40\r\nPlayerHealthFactor = 100");
    CHECK(!doc.Contains(ParamId::NPCHealthFactor));
    CHECK(!doc.Get(ParamId::NPCHealthFactor).has_value());
    doc.Set(ParamId::NPCHealthFactor, ParamValue::Int(250));
    CHECK(doc.Serialize() == "PlayerMaxLevel = 40\r\nPlayerHealthFactor = 100\r\nNPCHealthFactor = 250\r\n");
}

TEST(EmptyDocument) {
    auto doc = IniDocument::Parse("");
    CHECK(doc.Serialize().empty());
    doc.Set(ParamId::PlayerMaxLevel, ParamValue::Int(45));
    CHECK(doc.Serialize() == "PlayerMaxLevel = 45\r\n");
}

TEST(BomPreserved) {
    const std::string in = "\xEF\xBB\xBFPlayerMaxLevel = 40\r\n";
    ParseReport rep;
    const auto doc = IniDocument::Parse(in, &rep);
    CHECK(doc.Serialize() == in);
    CHECK(doc.Get(ParamId::PlayerMaxLevel) == ParamValue::Int(40));
    CHECK(!rep.Has(ParseIssue::Kind::Malformed));
}

TEST(TolerantValueParsing) {
    CHECK(IniDocument::ParseValue(ParamType::Bool, "True") == ParamValue::Bool(true));
    CHECK(IniDocument::ParseValue(ParamType::Bool, "0") == ParamValue::Bool(false));
    CHECK(!IniDocument::ParseValue(ParamType::Bool, "yes").has_value());
    CHECK(IniDocument::ParseValue(ParamType::Int, "-12") == ParamValue::Int(-12));
    CHECK(IniDocument::ParseValue(ParamType::Int, "+7") == ParamValue::Int(7));
    CHECK(IniDocument::ParseValue(ParamType::Int, "100.000000") == ParamValue::Int(100));
    CHECK(!IniDocument::ParseValue(ParamType::Int, "1.5").has_value());
    CHECK(!IniDocument::ParseValue(ParamType::Int, "abc").has_value());
    CHECK(!IniDocument::ParseValue(ParamType::Int, "").has_value());
    CHECK(IniDocument::ParseValue(ParamType::Float, "3") == ParamValue::Float(3.0f));
    CHECK(!IniDocument::ParseValue(ParamType::Float, "3,5").has_value());
}

TEST(InvalidValueAndDuplicateReported) {
    ParseReport rep;
    const auto doc = IniDocument::Parse("PlayerMaxLevel = abc\r\nPlayerMaxLevel = 50\r\nPlayerHealthFactor =\r\n", &rep);
    CHECK(rep.Has(ParseIssue::Kind::InvalidValue));
    CHECK(rep.Has(ParseIssue::Kind::Duplicate));
    CHECK(!doc.Get(ParamId::PlayerMaxLevel).has_value());  // first line wins
    auto copy = doc;
    copy.Set(ParamId::PlayerMaxLevel, ParamValue::Int(45));
    copy.Set(ParamId::PlayerHealthFactor, ParamValue::Int(120));
    CHECK(copy.Serialize() == "PlayerMaxLevel = 45\r\nPlayerMaxLevel = 45\r\nPlayerHealthFactor = 120\r\n");
}

TEST(FloatFormatIsLocaleIndependent) {
    const char* prev = std::setlocale(LC_ALL, nullptr);
    const std::string saved = prev ? prev : "C";
    std::setlocale(LC_ALL, "de-DE");
    try {
        std::locale::global(std::locale("de-DE"));
    } catch (...) {
    }
    CHECK(IniDocument::FormatValue(ParamValue::Float(0.0f)) == "0");
    CHECK(IniDocument::FormatValue(ParamValue::Float(-0.0f)) == "0");
    CHECK(IniDocument::FormatValue(ParamValue::Float(0.5f)) == "0.5");
    CHECK(IniDocument::FormatValue(ParamValue::Float(6.0f)) == "6");
    CHECK(IniDocument::FormatValue(ParamValue::Float(1234.5f)) == "1234.5");
    CHECK(IniDocument::ParseValue(ParamType::Float, "1.250000") == ParamValue::Float(1.25f));
    std::locale::global(std::locale::classic());
    std::setlocale(LC_ALL, saved.c_str());
}

}  // namespace

int main() {
    for (const auto& [name, fn] : acptest::Registry()) {
        const int before = acptest::g_failures;
        fn();
        std::printf("%s %s\n", acptest::g_failures == before ? "[ OK ]" : "[FAIL]", name);
    }
    std::printf("\n%zu tests, %d failed checks\n", acptest::Registry().size(), acptest::g_failures);
    return acptest::g_failures == 0 ? 0 : 1;
}
