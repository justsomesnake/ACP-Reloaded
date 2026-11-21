#pragma once

// SyncService: owns the IniDocument and the I/O thread. The only place that does file I/O.
// Render/UI thread: Submit*() and TryConsumeSnapshot() - both non-blocking (short mutex).

#include <array>
#include <atomic>
#include <bitset>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "param_table.h"
#include "platform.h"

namespace acp {

enum class SyncStatus : std::uint8_t {
    Starting,
    NotFound,  // no ACP.ini at the candidate paths
    Ok,        // loaded, everything written
    Pending,   // changes waiting to be written
    Locked,    // file locked, retrying
    ReadOnly,  // write-protected
    Error,
};

struct PresetInfo {
    std::string name;
    bool readOnly = false;  // bundled presets next to the DLL
};

struct Snapshot {
    std::uint64_t version = 0;
    SyncStatus status = SyncStatus::Starting;
    std::filesystem::path path;
    bool multipleCandidates = false;
    std::array<ParamValue, kParamCount> values{};  // file value, or default if missing/invalid
    std::bitset<kParamCount> present;               // key exists in the file
    std::bitset<kParamCount> invalid;               // value unreadable
    std::uint64_t externalChanges = 0;              // count of external changes
    std::uint64_t writes = 0;                       // count of our own writes
    std::uint64_t lastWriteTick = 0;                // GetTickCount64 of the last write
    std::vector<PresetInfo> presets;
    std::string message;  // last message (English, for the banner)
    std::string settingsText;  // contents of <userData>\settings.ini (key=value per line)
};

struct SyncConfig {
    std::vector<std::filesystem::path> candidates;       // empty = platform::DefaultIniCandidates()
    std::filesystem::path userDataDir;                    // presets, backups
    std::filesystem::path bundledPresetsDir;              // optional, read-only
    platform::WriteStrategy strategy = platform::WriteStrategy::AtomicReplace;
    std::function<void()> onWritten;                      // on the I/O thread after every write
    // On the I/O thread for every new state (including pending changes); first call = initial state.
    std::function<void(const std::array<ParamValue, kParamCount>&)> onValues;
    unsigned debounceMs = 60;                              // batches rapid changes
};

class SyncService {
public:
    SyncService() = default;
    ~SyncService();
    SyncService(const SyncService&) = delete;
    SyncService& operator=(const SyncService&) = delete;

    void Start(SyncConfig config);  // lifecycle thread
    void Stop(bool flush);          // lifecycle thread; writes pending changes if flush

    // UI thread, non-blocking:
    void SubmitEdit(ParamId id, ParamValue value);
    void ResetToDefaults();
    void SavePreset(std::string name);
    void ApplyPreset(std::string name);
    void DeletePreset(std::string name);
    void Reload();
    void SaveSettings(std::string text);  // writes <userData>\settings.ini
    // Only on explicit request (panel button): creates a missing ACP.ini with the original values at the first
    // candidate path. Does nothing if a file already exists.
    void CreateMissingIni();
    // Copies the save games (Ubisoft Connect folder) with a timestamp to <userData>\SaveBackups (at most 10).
    void BackupGameSaves();
    // Opens <userData>\SaveBackups in Explorer.
    void OpenSaveBackupFolder();

    // Returns true and copies if a new state is available since the last call.
    bool TryConsumeSnapshot(Snapshot& out);

private:
    struct EditCmd { ParamId id; ParamValue value; };
    struct ResetCmd {};
    struct SavePresetCmd { std::string name; };
    struct ApplyPresetCmd { std::string name; };
    struct DeletePresetCmd { std::string name; };
    struct ReloadCmd {};
    struct SaveSettingsCmd { std::string text; };
    struct CreateIniCmd {};
    struct BackupSavesCmd {};
    struct OpenBackupsCmd {};
    using Command = std::variant<EditCmd, ResetCmd, SavePresetCmd, ApplyPresetCmd, DeletePresetCmd, ReloadCmd, SaveSettingsCmd,
                                 CreateIniCmd, BackupSavesCmd, OpenBackupsCmd>;

    void Enqueue(Command c);
    void ThreadMain();

    // I/O thread only:
    bool ResolvePath();
    void LoadFromDisk(bool external);
    void ProcessCommands(std::vector<Command>& cmds);
    void TryWrite();
    void BackupOnce();
    void RefreshPresets();
    void Publish();

    SyncConfig config_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> flushOnStop_{true};

    std::mutex cmdMutex_;
    std::condition_variable cmdCv_;
    std::vector<Command> commands_;

    std::mutex snapMutex_;
    Snapshot latest_;
    std::atomic<std::uint64_t> latestVersion_{0};
    std::uint64_t consumedVersion_ = 0;  // UI thread only

    // I/O thread state
    struct IoState;
    IoState* io_ = nullptr;
};

}  // namespace acp
