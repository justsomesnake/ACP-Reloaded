#pragma once

// Win32 helpers for paths, file I/O and logging. Every function here does file I/O
// and may only be called on the I/O or lifecycle thread.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace acp::platform {

namespace fs = std::filesystem;

// <Documents> via SHGetKnownFolderPath(FOLDERID_Documents).
std::optional<fs::path> DocumentsDir();

// Default candidates: <Documents>\ACP.ini, <Documents>\Assassin's Creed Origins\ACP.ini.
// The environment variable ACP_RELOADED_INI replaces the list (tests, unusual setups).
std::vector<fs::path> DefaultIniCandidates();

// <Documents>\Assassin's Creed Origins\ACP_Reloaded (or ACP_RELOADED_DATA_DIR). Not created here.
std::optional<fs::path> UserDataDir();

// Assassin's Creed Origins save folders in the Ubisoft Connect directory
// (<Launcher>\savegames\<account>\<product>, product 4923 = Steam edition, 3539 = Ubisoft Connect edition).
std::vector<fs::path> GameSaveDirs();

// Directory of the module containing this code (DLL or EXE).
fs::path ThisModuleDir();

struct FileStamp {
    std::uint64_t size = 0;
    std::uint64_t writeTime = 0;
    bool readOnly = false;
    friend bool operator==(const FileStamp&, const FileStamp&) = default;
};
std::optional<FileStamp> StatFile(const fs::path& p);

// Reads the whole file; shares read, write and delete access so nobody is blocked.
std::optional<std::string> ReadFileBytes(const fs::path& p, std::uint32_t* lastError = nullptr);

enum class WriteStrategy { AtomicReplace, InPlace };

struct WriteResult {
    bool ok = false;
    std::uint32_t lastError = 0;
    int attempts = 0;
    bool retryable = false;  // lock or similar; retry later
};

// Writes bytes atomically (ReplaceFile) or exclusively in place; retries on locks
// (15/30/60/120/250/500 ms). With ReplaceFile, creates the target only if allowCreate.
WriteResult WriteFileAtomic(const fs::path& target, std::string_view bytes, WriteStrategy strategy,
                            bool allowCreate = false);

std::uint64_t Fnv1a64(std::string_view data) noexcept;

std::wstring FormatWin32Error(std::uint32_t code);

// Simple logger: Write() only buffers in memory (callable from any thread, never from
// hook detours); Flush() writes to disk (I/O or lifecycle thread only).
namespace log {
void SetFile(const fs::path& path);
void Write(std::string_view line);
void Flush();
}  // namespace log

std::string Narrow(std::wstring_view w);
std::wstring Widen(std::string_view s);

}  // namespace acp::platform
