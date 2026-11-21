// Tests for SyncService (real files in a temp folder).
#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "ini_document.h"
#include "platform.h"
#include "sync_service.h"
#include "test_main.h"

using namespace acp;
namespace fs = std::filesystem;

namespace {

std::string Slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void Spit(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

struct TempDir {
    fs::path dir;
    TempDir() {
        wchar_t tmp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tmp);
        dir = fs::path(tmp) / (L"acp_tests_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                               std::to_wstring(GetTickCount64()));
        fs::create_directories(dir / L"docs");
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    fs::path Ini() const { return dir / L"docs" / L"ACP.ini"; }
    fs::path Data() const { return dir / L"data"; }
};

template <typename Pred>
bool WaitSnap(SyncService& s, Snapshot& snap, Pred pred, int timeoutMs = 3000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        s.TryConsumeSnapshot(snap);
        if (pred(snap)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    s.TryConsumeSnapshot(snap);
    return pred(snap);
}

SyncConfig Config(const TempDir& t) {
    SyncConfig c;
    c.candidates = {t.Ini()};
    c.userDataDir = t.Data();
    return c;
}

const std::string& Ref() {
    static const std::string s = Slurp(ACP_REFERENCE_INI);
    return s;
}

}  // namespace

TEST(SyncLoadsAndWritesOnlyToken) {
    TempDir t;
    Spit(t.Ini(), Ref());
    SyncService s;
    s.Start(Config(t));
    Snapshot snap;
    CHECK(WaitSnap(s, snap, [](const Snapshot& x) { return x.status == SyncStatus::Ok; }));
    CHECK(snap.present.all());
    CHECK(snap.values[static_cast<std::size_t>(ParamId::PlayerMaxLevel)] == ParamValue::Int(40));

    s.SubmitEdit(ParamId::PlayerMovementSpeedMultiplier, ParamValue::Int(300));
    CHECK(WaitSnap(s, snap, [](const Snapshot& x) { return x.writes == 1 && x.status == SyncStatus::Ok; }));
    const std::string after = Slurp(t.Ini());
    CHECK(after.size() == Ref().size());
    CHECK(after.find("\r\nPlayerMovementSpeedMultiplier = 300\r\n") != std::string::npos);
    // backup created
    int backups = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(t.Data() / L"Backup", ec)) backups += e.path().extension() == L".bak";
    CHECK(backups == 1);
    s.Stop(true);
}

TEST(SyncNoWriteWithoutChange) {
    TempDir t;
    Spit(t.Ini(), Ref());
    const auto before = platform::StatFile(t.Ini());
    SyncService s;
    s.Start(Config(t));
    Snapshot snap;
    CHECK(WaitSnap(s, snap, [](const Snapshot& x) { return x.status == SyncStatus::Ok; }));
    s.SubmitEdit(ParamId::PlayerMaxLevel, ParamValue::Int(40));  // same value
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    s.Stop(true);
    s.TryConsumeSnapshot(snap);
    CHECK(snap.writes == 0);
    CHECK(platform::StatFile(t.Ini()) == before);
    CHECK(!fs::exists(t.Data() / L"Backup"));
}

TEST(SyncCoalescesRapidEdits) {
    TempDir t;
    Spit(t.Ini(), Ref());
    SyncService s;
    s.Start(Config(t));
    Snapshot snap;
    CHECK(WaitSnap(s, snap, [](const Snapshot& x) { return x.status == SyncStatus::Ok; }));
    for (int i = 0; i < 100; ++i) s.SubmitEdit(ParamId::PlayerHealthFactor, ParamValue::Int(101 + i));
    CHECK(WaitSnap(s, snap, [](const Snapshot& x) {
        return x.status == SyncStatus::Ok && x.values[static_cast<std::size_t>(ParamId::PlayerHealthFactor)] == ParamValue::Int(200);
    }));
    s.Stop(true);
    CHECK(snap.writes >= 1 && snap.writes <= 2);
    CHECK(Slurp(t.Ini()).find("\r\nPlayerHealthFactor = 200\r\n") != std::string::npos);
}

TEST(SyncDetectsExternalChangeAndKeepsIt) {
    TempDir t;
    Spit(t.Ini(), Ref());
    SyncService s;
    s.Start(Config(t));
    Snapshot snap;
    CHECK(WaitSnap(s, snap, [](const Snapshot& x) { return x.status == SyncStatus::Ok; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto doc = IniDocument::Parse(Ref());
    doc.Set(ParamId::NPCHealthFactor, ParamValue::Int(555));
    Spit(t.Ini(), doc.Serialize());
    const auto start = std::chrono::steady_clock::now();
    CHECK(WaitSnap(s, snap, [](const Snapshot& x) {
        return x.externalChanges >= 1 && x.values[static_cast<std::size_t>(ParamId::NPCHealthFactor)] == ParamValue::Int(555);
    }));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1500));
    // Our change to another key keeps the external one
    s.SubmitEdit(ParamId::PlayerHealthFactor, ParamValue::Int(150));
    CHECK(WaitSnap(s, snap, [](const Snapshot& x) { return x.writes == 1; }));
    const auto txt = Slurp(t.Ini());
    CHECK(txt.find("NPCHealthFactor = 555\r\n") != std::string::npos);
    CHECK(txt.find("PlayerHealthFactor = 150\r\n") != std::string::npos);
    s.Stop(true);
}

TEST(SyncRetriesWhenLocked) {
    TempDir t;
    Spit(t.Ini(), Ref());
    SyncService s;
    s.Start(Config(t));
    Snapshot snap;
    CHECK(WaitSnap(s, snap, [](const Snapshot& x) { return x.status == SyncStatus::Ok; }));
    HANDLE lock = CreateFileW(t.Ini().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(lock != INVALID_HANDLE_VALUE);
    s.SubmitEdit(ParamId::PlayerMaxLevel, ParamValue::Int(45));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CloseHandle(lock);
    CHECK(WaitSnap(s, snap, [](const Snapshot& x) { return x.writes == 1 && x.status == SyncStatus::Ok; }, 4000));
    CHECK(Slurp(t.Ini()).find("\r\nPlayerMaxLevel = 45\r\n") != std::string::npos);
    s.Stop(true);
}

TEST(SyncNeverCreatesMissingIni) {
    TempDir t;
    SyncService s;
    s.Start(Config(t));
    Snapshot snap;
    CHECK(WaitSnap(s, snap, [](const Snapshot& x) { return x.status == SyncStatus::NotFound; }));
    s.SubmitEdit(ParamId::PlayerMaxLevel, ParamValue::Int(45));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    s.Stop(true);
    CHECK(!fs::exists(t.Ini()));
}

TEST(SyncCreatesMissingIniOnRequest) {
    TempDir t;
    SyncService s;
    s.Start(Config(t));
    Snapshot snap;
    CHECK(WaitSnap(s, snap, [](const Snapshot& x) { return x.status == SyncStatus::NotFound; }));
    s.CreateMissingIni();
    CHECK(WaitSnap(s, snap, [](const Snapshot& x) { return x.status == SyncStatus::Ok; }));
    CHECK(snap.present.all());
    // Same format as the game-written reference (keys, order, spelling). Only intended difference:
    // the two sandstorm intervals get the real default 0.5 instead of the 0 from the game's file.
    std::string expected = Ref();
    for (const char* k : {"PlayerDamageInSandstormInterval", "NPCDamageInSandstormInterval"}) {
        const std::string from = std::string("\r\n") + k + " = 0\r\n", to = std::string("\r\n") + k + " = 0.5\r\n";
        const auto pos = expected.find(from);
        CHECK(pos != std::string::npos);
        if (pos != std::string::npos) expected.replace(pos, from.size(), to);
    }
    const std::string created = Slurp(t.Ini());
    CHECK(created == expected);
    s.CreateMissingIni();  // second call: file exists, overwrite nothing
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(Slurp(t.Ini()) == created);
    s.Stop(true);
}

TEST(SyncBacksUpGameSaves) {
    // Only reads the real save games (copies into a temp folder). Without saves: a clean message.
    TempDir t;
    Spit(t.Ini(), Ref());
    SyncService s;
    s.Start(Config(t));
    Snapshot snap;
    CHECK(WaitSnap(s, snap, [](const Snapshot& x) { return x.status == SyncStatus::Ok; }));
    s.BackupGameSaves();
    CHECK(WaitSnap(s, snap, [](const Snapshot& x) {
        return x.message.rfind("Game saves backed up", 0) == 0 || x.message == "No game saves found";
    }, 10000));
    if (!platform::GameSaveDirs().empty()) {
        int files = 0;
        std::error_code ec;
        for (const auto& e : fs::recursive_directory_iterator(t.Data() / L"SaveBackups", ec)) files += e.is_regular_file(ec);
        CHECK(files > 0);
    }
    s.Stop(true);
}

TEST(SyncPresetsAndReset) {
    TempDir t;
    Spit(t.Ini(), Ref());
    SyncService s;
    s.Start(Config(t));
    Snapshot snap;
    CHECK(WaitSnap(s, snap, [](const Snapshot& x) { return x.status == SyncStatus::Ok; }));

    s.SubmitEdit(ParamId::PlayerHealthFactor, ParamValue::Int(300));
    s.SavePreset("My: Preset?");  // invalid characters are removed
    CHECK(WaitSnap(s, snap, [](const Snapshot& x) { return x.presets.size() == 1; }));
    CHECK(!snap.presets.empty() && snap.presets[0].name == "My Preset");
    CHECK(fs::exists(t.Data() / L"Presets" / L"My Preset.ini"));

    s.ResetToDefaults();
    CHECK(WaitSnap(s, snap, [](const Snapshot& x) {
        return x.status == SyncStatus::Ok && x.values[static_cast<std::size_t>(ParamId::PlayerHealthFactor)] == ParamValue::Int(100);
    }));
    {
        // After the reset all settable values are at the game defaults; status values stay untouched.
        const auto doc = IniDocument::Parse(Slurp(t.Ini()));
        for (const ParamDesc& d : ParamTable()) {
            if (d.readOnly) CHECK(doc.GetRaw(d.id) == IniDocument::Parse(Ref()).GetRaw(d.id));
            else CHECK(doc.Get(d.id) == d.def);
        }
    }

    s.ApplyPreset("My Preset");
    CHECK(WaitSnap(s, snap, [](const Snapshot& x) {
        return x.status == SyncStatus::Ok && x.values[static_cast<std::size_t>(ParamId::PlayerHealthFactor)] == ParamValue::Int(300);
    }));
    s.DeletePreset("My Preset");
    CHECK(WaitSnap(s, snap, [](const Snapshot& x) { return x.presets.empty(); }));
    s.Stop(true);
}

TEST(AtomicWriteRoundtrip) {
    TempDir t;
    const fs::path p = t.dir / L"x.ini";
    CHECK(!platform::WriteFileAtomic(p, "a", platform::WriteStrategy::AtomicReplace).ok);  // creates nothing
    CHECK(platform::WriteFileAtomic(p, "abc", platform::WriteStrategy::AtomicReplace, true).ok);
    CHECK(Slurp(p) == "abc");
    CHECK(platform::WriteFileAtomic(p, "de", platform::WriteStrategy::AtomicReplace).ok);
    CHECK(Slurp(p) == "de");
    CHECK(platform::WriteFileAtomic(p, "fgh", platform::WriteStrategy::InPlace).ok);
    CHECK(Slurp(p) == "fgh");
    CHECK(!fs::exists(t.dir / L"x.ini.acpr.tmp"));
}
