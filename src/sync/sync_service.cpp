#include "sync_service.h"

#include <charconv>

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>

#include "ini_document.h"

namespace acp {

namespace fs = std::filesystem;
using platform::log::Write;

struct SyncService::IoState {
    fs::path path;
    bool multiple = false;
    std::optional<platform::FileStamp> stamp;
    std::uint64_t hash = 0;
    IniDocument doc;
    bool loaded = false;
    std::array<std::optional<ParamValue>, kParamCount> pending{};
    bool backedUp = false;
    SyncStatus status = SyncStatus::Starting;
    std::string message;
    std::uint64_t externalChanges = 0;
    std::uint64_t writes = 0;
    std::uint64_t lastWriteTick = 0;
    std::uint64_t nextRetryTick = 0;
    std::uint64_t nextResolveTick = 0;
    std::vector<PresetInfo> presets;
    std::string settingsText;
    bool dirty = true;

    bool HasPending() const {
        return std::any_of(pending.begin(), pending.end(), [](const auto& p) { return p.has_value(); });
    }
};

namespace {

std::uint64_t Now() { return GetTickCount64(); }

std::string SanitizePresetName(std::string name) {
    std::string out;
    for (char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 32 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos) continue;
        out += c;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    if (out.size() > 64) out.resize(64);
    return out;
}

std::string TimeStamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                  st.wSecond);
    return buf;
}

// Complete file in the game's format, built from values (for presets without a template).
std::string SerializeAll(const std::array<ParamValue, kParamCount>& values) {
    std::string out;
    for (const ParamDesc& d : ParamTable()) {
        out += d.key;
        out += " = ";
        const ParamValue& v = values[static_cast<std::size_t>(d.id)];
        if (d.id == ParamId::DamageInSandstorm && v.type == ParamType::Float) {
            // The game writes its only float key with six decimals ("0.000000").
            std::array<char, 32> buf{};
            const auto res = std::to_chars(buf.data(), buf.data() + buf.size(), v.f, std::chars_format::fixed, 6);
            out.append(buf.data(), res.ptr);
        } else {
            out += IniDocument::FormatValue(v);
        }
        out += "\r\n";
    }
    return out;
}

}  // namespace

SyncService::~SyncService() { Stop(false); }

void SyncService::Start(SyncConfig config) {
    if (thread_.joinable()) return;
    config_ = std::move(config);
    if (config_.candidates.empty()) config_.candidates = platform::DefaultIniCandidates();
    stop_ = false;
    thread_ = std::thread([this] { ThreadMain(); });
}

void SyncService::Stop(bool flush) {
    if (!thread_.joinable()) return;
    flushOnStop_ = flush;
    {
        std::lock_guard lock(cmdMutex_);
        stop_ = true;
    }
    cmdCv_.notify_all();
    thread_.join();
}

void SyncService::Enqueue(Command c) {
    {
        std::lock_guard lock(cmdMutex_);
        commands_.push_back(std::move(c));
    }
    cmdCv_.notify_one();
}

void SyncService::SubmitEdit(ParamId id, ParamValue value) {
    if (value.type != Desc(id).type) return;
    Enqueue(EditCmd{id, value});
}
void SyncService::ResetToDefaults() { Enqueue(ResetCmd{}); }
void SyncService::SavePreset(std::string name) { Enqueue(SavePresetCmd{std::move(name)}); }
void SyncService::ApplyPreset(std::string name) { Enqueue(ApplyPresetCmd{std::move(name)}); }
void SyncService::DeletePreset(std::string name) { Enqueue(DeletePresetCmd{std::move(name)}); }
void SyncService::Reload() { Enqueue(ReloadCmd{}); }
void SyncService::SaveSettings(std::string text) { Enqueue(SaveSettingsCmd{std::move(text)}); }
void SyncService::CreateMissingIni() { Enqueue(CreateIniCmd{}); }
void SyncService::BackupGameSaves() { Enqueue(BackupSavesCmd{}); }
void SyncService::OpenSaveBackupFolder() { Enqueue(OpenBackupsCmd{}); }

bool SyncService::TryConsumeSnapshot(Snapshot& out) {
    if (latestVersion_.load(std::memory_order_acquire) == consumedVersion_) return false;
    std::lock_guard lock(snapMutex_);
    out = latest_;
    consumedVersion_ = latest_.version;
    return true;
}

void SyncService::ThreadMain() {
    IoState state;
    io_ = &state;
    Write("SyncService started");
    ResolvePath();
    if (!io_->path.empty()) LoadFromDisk(false);
    RefreshPresets();
    if (!config_.userDataDir.empty()) {
        if (auto t = platform::ReadFileBytes(config_.userDataDir / L"settings.ini")) io_->settingsText = *t;
    }
    Publish();

    for (;;) {
        std::vector<Command> cmds;
        {
            std::unique_lock lock(cmdMutex_);
            cmdCv_.wait_for(lock, std::chrono::milliseconds(500), [this] { return stop_.load() || !commands_.empty(); });
            if (!commands_.empty() && !stop_) {
                // Brief debounce: rapid follow-up changes result in a single write.
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(config_.debounceMs));
                lock.lock();
            }
            cmds.swap(commands_);
        }
        if (!cmds.empty()) ProcessCommands(cmds);

        const std::uint64_t now = Now();
        if (io_->path.empty()) {
            if (now >= io_->nextResolveTick) {
                io_->nextResolveTick = now + 2000;
                if (ResolvePath()) LoadFromDisk(false);
            }
        } else {
            const auto stamp = platform::StatFile(io_->path);
            if (!stamp) {
                Write("ACP.ini disappeared");
                io_->path.clear();
                io_->loaded = false;
                io_->status = SyncStatus::NotFound;
                io_->dirty = true;
            } else if (!io_->stamp || !(*stamp == *io_->stamp)) {
                LoadFromDisk(true);
            }
        }

        if (io_->HasPending() && now >= io_->nextRetryTick) TryWrite();

        const bool stopping = stop_.load();
        if (stopping) {
            if (flushOnStop_ && io_->HasPending()) {
                io_->nextRetryTick = 0;
                TryWrite();
            }
        }
        if (io_->dirty) Publish();
        platform::log::Flush();
        if (stopping) break;
    }
    Write("SyncService stopped");
    platform::log::Flush();
    io_ = nullptr;
}

bool SyncService::ResolvePath() {
    std::vector<fs::path> found;
    for (const auto& c : config_.candidates) {
        if (platform::StatFile(c)) found.push_back(c);
    }
    if (found.empty()) {
        if (io_->status != SyncStatus::NotFound) {
            Write("ACP.ini not found");
            io_->status = SyncStatus::NotFound;
            io_->message = "ACP.ini not found - start the game once or create it from the panel";
            io_->dirty = true;
        }
        return false;
    }
    io_->path = found.front();
    io_->multiple = found.size() > 1;
    Write("ACP.ini: " + platform::Narrow(io_->path.wstring()) + (io_->multiple ? " (other candidates exist)" : ""));
    io_->dirty = true;
    return true;
}

void SyncService::LoadFromDisk(bool external) {
    std::uint32_t err = 0;
    auto bytes = platform::ReadFileBytes(io_->path, &err);
    if (!bytes) {
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            io_->path.clear();
            io_->loaded = false;
            io_->status = SyncStatus::NotFound;
        } else {
            io_->status = SyncStatus::Error;
            io_->message = "Cannot read ACP.ini: " + platform::Narrow(platform::FormatWin32Error(err));
            io_->nextRetryTick = Now() + 2000;
        }
        io_->dirty = true;
        return;
    }
    io_->stamp = platform::StatFile(io_->path);
    const std::uint64_t h = platform::Fnv1a64(*bytes);
    if (io_->loaded && h == io_->hash) return;  // only the timestamp changed
    ParseReport report;
    io_->doc = IniDocument::Parse(*bytes, &report);
    if (external && io_->loaded) {
        ++io_->externalChanges;
        io_->message = "ACP.ini was changed externally - reloaded";
        Write("External change detected, reloaded");
    } else {
        io_->message = "ACP.ini loaded";
    }
    for (const auto& issue : report.issues) {
        if (issue.kind != ParseIssue::Kind::MissingKey)
            Write("INI notice line " + std::to_string(issue.line) + ": " + issue.key);
    }
    io_->hash = h;
    io_->loaded = true;
    if (!io_->HasPending()) io_->status = io_->stamp && io_->stamp->readOnly ? SyncStatus::ReadOnly : SyncStatus::Ok;
    io_->dirty = true;
}

void SyncService::ProcessCommands(std::vector<Command>& cmds) {
    for (auto& cmd : cmds) {
        if (auto* e = std::get_if<EditCmd>(&cmd)) {
            io_->pending[static_cast<std::size_t>(e->id)] = e->value;
            io_->nextRetryTick = 0;
        } else if (std::holds_alternative<ResetCmd>(cmd)) {
            for (const ParamDesc& d : ParamTable())
                if (!d.readOnly) io_->pending[static_cast<std::size_t>(d.id)] = d.def;
            io_->nextRetryTick = 0;
            io_->message = "Original values restored";
            Write("Reset to original values");
        } else if (auto* a = std::get_if<ApplyPresetCmd>(&cmd)) {
            std::optional<std::string> bytes;
            for (const fs::path& dir : {config_.userDataDir / L"Presets", config_.bundledPresetsDir}) {
                if (dir.empty()) continue;
                bytes = platform::ReadFileBytes(dir / (platform::Widen(a->name) + L".ini"));
                if (bytes) break;
            }
            if (!bytes) {
                io_->message = "Preset '" + a->name + "' not found";
            } else {
                const auto preset = IniDocument::Parse(*bytes);
                int n = 0;
                for (const ParamDesc& d : ParamTable()) {
                    if (auto v = preset.Get(d.id)) {
                        io_->pending[static_cast<std::size_t>(d.id)] = *v;
                        ++n;
                    }
                }
                io_->nextRetryTick = 0;
                io_->message = "Preset '" + a->name + "' applied (" + std::to_string(n) + " values)";
                Write("Preset applied: " + a->name);
            }
        } else if (auto* s = std::get_if<SavePresetCmd>(&cmd)) {
            const std::string name = SanitizePresetName(s->name);
            if (name.empty() || config_.userDataDir.empty()) {
                io_->message = "Invalid preset name";
            } else {
                std::array<ParamValue, kParamCount> values{};
                for (const ParamDesc& d : ParamTable()) {
                    const auto i = static_cast<std::size_t>(d.id);
                    if (io_->pending[i]) values[i] = *io_->pending[i];
                    else if (auto v = io_->loaded ? io_->doc.Get(d.id) : std::nullopt) values[i] = *v;
                    else values[i] = d.def;
                }
                std::error_code ec;
                const fs::path dir = config_.userDataDir / L"Presets";
                fs::create_directories(dir, ec);
                const auto r = platform::WriteFileAtomic(dir / (platform::Widen(name) + L".ini"), SerializeAll(values),
                                                         platform::WriteStrategy::AtomicReplace, true);
                io_->message = r.ok ? "Preset '" + name + "' saved"
                                    : "Saving preset failed: " + platform::Narrow(platform::FormatWin32Error(r.lastError));
                Write(io_->message);
                RefreshPresets();
            }
        } else if (auto* del = std::get_if<DeletePresetCmd>(&cmd)) {
            const fs::path p = config_.userDataDir / L"Presets" / (platform::Widen(SanitizePresetName(del->name)) + L".ini");
            io_->message = DeleteFileW(p.c_str()) ? "Preset '" + del->name + "' deleted" : "Preset could not be deleted";
            Write(io_->message);
            RefreshPresets();
        } else if (auto* st = std::get_if<SaveSettingsCmd>(&cmd)) {
            if (!config_.userDataDir.empty() && st->text != io_->settingsText) {
                std::error_code ec;
                fs::create_directories(config_.userDataDir, ec);
                const auto r = platform::WriteFileAtomic(config_.userDataDir / L"settings.ini", st->text,
                                                         platform::WriteStrategy::AtomicReplace, true);
                if (r.ok) io_->settingsText = st->text;
                else Write("Settings could not be saved");
            }
        } else if (std::holds_alternative<CreateIniCmd>(cmd)) {
            // Only if no candidate path holds a file; format exactly as the game writes it (original values).
            bool exists = false;
            for (const auto& c : config_.candidates) exists |= platform::StatFile(c).has_value();
            if (exists || config_.candidates.empty()) {
                io_->message = "ACP.ini already exists";
            } else {
                std::array<ParamValue, kParamCount> defaults{};
                for (const ParamDesc& d : ParamTable()) defaults[static_cast<std::size_t>(d.id)] = d.def;
                const fs::path target = config_.candidates.front();
                std::error_code ec;
                fs::create_directories(target.parent_path(), ec);
                const auto r = platform::WriteFileAtomic(target, SerializeAll(defaults), platform::WriteStrategy::AtomicReplace, true);
                io_->message = r.ok ? "ACP.ini created with the original values"
                                    : "Creating ACP.ini failed: " + platform::Narrow(platform::FormatWin32Error(r.lastError));
                Write(r.ok ? "ACP.ini created on request: " + platform::Narrow(target.wstring()) : io_->message);
                if (r.ok && ResolvePath()) LoadFromDisk(false);
            }
        } else if (std::holds_alternative<BackupSavesCmd>(cmd)) {
            const auto dirs = platform::GameSaveDirs();
            if (dirs.empty() || config_.userDataDir.empty()) {
                io_->message = "No game saves found";
            } else {
                const fs::path root = config_.userDataDir / L"SaveBackups";
                const fs::path dest = root / platform::Widen(TimeStamp());
                std::error_code ec;
                int files = 0, n = 0;
                for (const fs::path& d : dirs) {
                    // Target: <timestamp>\<product>[_<n>] (account ID not in the name)
                    const fs::path to = dest / (d.filename().wstring() + (n ? L"_" + std::to_wstring(n) : L""));
                    ++n;
                    fs::create_directories(to, ec);
                    for (const auto& f : fs::directory_iterator(d, ec)) {
                        if (!f.is_regular_file(ec)) continue;
                        if (CopyFileW(f.path().c_str(), (to / f.path().filename()).c_str(), FALSE)) ++files;
                    }
                }
                // Keep at most 10 backups (delete the oldest first; names are sortable timestamps)
                std::vector<fs::path> all;
                for (const auto& entry : fs::directory_iterator(root, ec))
                    if (entry.is_directory(ec)) all.push_back(entry.path());
                std::sort(all.begin(), all.end());
                for (std::size_t i = 0; i + 10 < all.size(); ++i) fs::remove_all(all[i], ec);
                io_->message = files > 0 ? "Game saves backed up (" + std::to_string(files) + " files)" : "Backing up game saves failed";
                Write(io_->message + ": " + platform::Narrow(dest.wstring()));
            }
        } else if (std::holds_alternative<OpenBackupsCmd>(cmd)) {
            if (!config_.userDataDir.empty()) {
                const fs::path root = config_.userDataDir / L"SaveBackups";
                std::error_code ec;
                fs::create_directories(root, ec);
                ShellExecuteW(nullptr, L"open", root.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        } else if (std::holds_alternative<ReloadCmd>(cmd)) {
            io_->stamp.reset();
            io_->loaded = false;
            if (io_->path.empty()) ResolvePath();
            if (!io_->path.empty()) LoadFromDisk(false);
            RefreshPresets();
        }
        io_->dirty = true;
    }
    if (io_->HasPending() && io_->status == SyncStatus::Ok) io_->status = SyncStatus::Pending;
}

void SyncService::BackupOnce() {
    if (io_->backedUp || config_.userDataDir.empty()) return;
    auto bytes = platform::ReadFileBytes(io_->path);
    if (!bytes) return;
    std::error_code ec;
    const fs::path dir = config_.userDataDir / L"Backup";
    fs::create_directories(dir, ec);
    const auto r = platform::WriteFileAtomic(dir / platform::Widen("ACP.ini." + TimeStamp() + ".bak"), *bytes,
                                             platform::WriteStrategy::AtomicReplace, true);
    if (!r.ok) {
        Write("Backup failed: " + platform::Narrow(platform::FormatWin32Error(r.lastError)));
        return;  // still allow writing; the atomic replace protects the file
    }
    io_->backedUp = true;
    std::vector<fs::path> backups;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() == L".bak") backups.push_back(e.path());
    }
    std::sort(backups.begin(), backups.end());
    while (backups.size() > 10) {
        fs::remove(backups.front(), ec);
        backups.erase(backups.begin());
    }
}

void SyncService::TryWrite() {
    const std::uint64_t now = Now();
    if (io_->path.empty()) {
        io_->status = SyncStatus::NotFound;
        io_->dirty = true;
        return;
    }
    std::uint32_t err = 0;
    auto fresh = platform::ReadFileBytes(io_->path, &err);
    if (!fresh) {
        io_->status = SyncStatus::Locked;
        io_->message = "Cannot read ACP.ini: " + platform::Narrow(platform::FormatWin32Error(err));
        io_->nextRetryTick = now + 2000;
        io_->dirty = true;
        return;
    }
    // Always start from the fresh file contents so external changes to other keys are kept.
    const std::uint64_t freshHash = platform::Fnv1a64(*fresh);
    if (io_->loaded && freshHash != io_->hash) {
        ++io_->externalChanges;
        Write("File changed externally before writing; applying changes to the new contents");
    }
    IniDocument doc = IniDocument::Parse(*fresh);
    for (const ParamDesc& d : ParamTable()) {
        if (const auto& v = io_->pending[static_cast<std::size_t>(d.id)]) doc.Set(d.id, *v);
    }
    const std::string bytes = doc.Serialize();
    if (bytes == *fresh) {
        io_->pending = {};
        io_->doc = std::move(doc);
        io_->hash = freshHash;
        io_->loaded = true;
        io_->status = SyncStatus::Ok;
        io_->dirty = true;
        return;
    }
    const auto stamp = platform::StatFile(io_->path);
    if (stamp && stamp->readOnly) {
        io_->status = SyncStatus::ReadOnly;
        io_->message = "ACP.ini is read-only - changes not saved";
        io_->nextRetryTick = now + 2000;
        io_->dirty = true;
        return;
    }
    BackupOnce();
    const auto r = platform::WriteFileAtomic(io_->path, bytes, config_.strategy);
    if (!r.ok) {
        io_->status = r.retryable ? SyncStatus::Locked : SyncStatus::Error;
        io_->message = (r.retryable ? "ACP.ini is locked, retrying: " : "Saving failed: ") +
                       platform::Narrow(platform::FormatWin32Error(r.lastError));
        io_->nextRetryTick = now + 2000;
        io_->dirty = true;
        Write(io_->message + " (Versuche: " + std::to_string(r.attempts) + ")");
        return;
    }
    // Read back and verify.
    auto check = platform::ReadFileBytes(io_->path);
    const std::uint64_t h = platform::Fnv1a64(bytes);
    if (!check || platform::Fnv1a64(*check) != h) Write("Warning: read-back after writing does not match");
    io_->doc = std::move(doc);
    io_->hash = h;
    io_->stamp = platform::StatFile(io_->path);
    io_->loaded = true;
    io_->pending = {};
    ++io_->writes;
    io_->lastWriteTick = now;
    io_->status = SyncStatus::Ok;
    io_->message = "Saved";
    io_->dirty = true;
    Write("ACP.ini saved (" + std::to_string(r.attempts) + " attempt(s))");
    if (config_.onWritten) config_.onWritten();
}

void SyncService::RefreshPresets() {
    std::vector<PresetInfo> list;
    std::error_code ec;
    auto scan = [&](const fs::path& dir, bool readOnly) {
        if (dir.empty()) return;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (e.path().extension() != L".ini") continue;
            const std::string name = platform::Narrow(e.path().stem().wstring());
            if (std::none_of(list.begin(), list.end(), [&](const PresetInfo& p) { return p.name == name; }))
                list.push_back({name, readOnly});
        }
    };
    if (!config_.userDataDir.empty()) scan(config_.userDataDir / L"Presets", false);
    scan(config_.bundledPresetsDir, true);
    std::sort(list.begin(), list.end(), [](const PresetInfo& a, const PresetInfo& b) { return a.name < b.name; });
    io_->presets = std::move(list);
    io_->dirty = true;
}

void SyncService::Publish() {
    Snapshot s;
    s.status = io_->status;
    s.path = io_->path;
    s.multipleCandidates = io_->multiple;
    for (const ParamDesc& d : ParamTable()) {
        const auto i = static_cast<std::size_t>(d.id);
        const bool contains = io_->loaded && io_->doc.Contains(d.id);
        const auto fileValue = contains ? io_->doc.Get(d.id) : std::nullopt;
        s.present[i] = contains;
        s.invalid[i] = contains && !fileValue;
        s.values[i] = io_->pending[i] ? *io_->pending[i] : fileValue ? *fileValue : d.def;
    }
    s.externalChanges = io_->externalChanges;
    s.writes = io_->writes;
    s.lastWriteTick = io_->lastWriteTick;
    s.presets = io_->presets;
    s.message = io_->message;
    s.settingsText = io_->settingsText;
    if (config_.onValues && s.status != SyncStatus::Starting) config_.onValues(s.values);
    {
        std::lock_guard lock(snapMutex_);
        s.version = latest_.version + 1;
        latest_ = std::move(s);
        latestVersion_.store(latest_.version, std::memory_order_release);
    }
    io_->dirty = false;
}

}  // namespace acp
