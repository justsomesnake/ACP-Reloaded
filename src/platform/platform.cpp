#include "platform.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

namespace acp::platform {

std::optional<fs::path> DocumentsDir() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &raw)) || !raw) {
        if (raw) CoTaskMemFree(raw);
        return std::nullopt;
    }
    fs::path p(raw);
    CoTaskMemFree(raw);
    return p;
}

namespace {
std::optional<std::wstring> Env(const wchar_t* name) {
    wchar_t buf[2048];
    const DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return std::nullopt;
    return std::wstring(buf, n);
}
}  // namespace

std::vector<fs::path> DefaultIniCandidates() {
    if (auto e = Env(L"ACP_RELOADED_INI")) return {fs::path(*e)};
    std::vector<fs::path> out;
    if (auto docs = DocumentsDir()) {
        out.push_back(*docs / L"ACP.ini");
        out.push_back(*docs / L"Assassin's Creed Origins" / L"ACP.ini");
    }
    return out;
}

std::vector<fs::path> GameSaveDirs() {
    std::vector<fs::path> out;
    // Launcher install location from the registry, else the default path
    std::wstring launcher;
    wchar_t buf[MAX_PATH]{};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Ubisoft\\Launcher", L"InstallDir", RRF_RT_REG_SZ, nullptr, buf,
                     &size) == ERROR_SUCCESS)
        launcher = buf;
    if (launcher.empty()) launcher = L"C:\\Program Files (x86)\\Ubisoft\\Ubisoft Game Launcher\\";
    const fs::path root = fs::path(launcher) / L"savegames";
    std::error_code ec;
    for (const auto& account : fs::directory_iterator(root, ec)) {
        if (!account.is_directory(ec)) continue;
        for (const wchar_t* product : {L"4923", L"3539"}) {
            const fs::path dir = account.path() / product;
            if (fs::is_directory(dir, ec)) out.push_back(dir);
        }
    }
    return out;
}
std::optional<fs::path> UserDataDir() {
    if (auto e = Env(L"ACP_RELOADED_DATA_DIR")) return fs::path(*e);
    auto docs = DocumentsDir();
    if (!docs) return std::nullopt;
    return *docs / L"Assassin's Creed Origins" / L"ACP_Reloaded";
}

fs::path ThisModuleDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ThisModuleDir), &self);
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(self, buf.data(), static_cast<DWORD>(buf.size()));
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    return fs::path(buf).parent_path();
}

std::optional<FileStamp> StatFile(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return std::nullopt;
    if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return std::nullopt;
    FileStamp s;
    s.size = (static_cast<std::uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    s.writeTime = (static_cast<std::uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
    s.readOnly = (d.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
    return s;
}

std::optional<std::string> ReadFileBytes(const fs::path& p, std::uint32_t* lastError) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (lastError) *lastError = GetLastError();
        return std::nullopt;
    }
    std::string out;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (16LL << 20)) {
        out.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr)) {
            if (lastError) *lastError = GetLastError();
            CloseHandle(h);
            return std::nullopt;
        }
        out.resize(read);
    }
    CloseHandle(h);
    if (lastError) *lastError = 0;
    return out;
}

namespace {

bool IsRetryable(DWORD e) {
    return e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION || e == ERROR_ACCESS_DENIED ||
           e == ERROR_UNABLE_TO_REMOVE_REPLACED || e == ERROR_UNABLE_TO_MOVE_REPLACEMENT ||
           e == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2;
}

DWORD WriteWholeHandle(HANDLE h, std::string_view bytes) {
    std::size_t done = 0;
    while (done < bytes.size()) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 1u << 20));
        if (!WriteFile(h, bytes.data() + done, chunk, &written, nullptr)) return GetLastError();
        done += written;
    }
    if (!SetEndOfFile(h)) return GetLastError();
    if (!FlushFileBuffers(h)) return GetLastError();
    return ERROR_SUCCESS;
}

DWORD TryReplace(const fs::path& target, std::string_view bytes, bool allowCreate) {
    const bool exists = GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!exists && !allowCreate) return ERROR_FILE_NOT_FOUND;

    fs::path tmp = target;
    tmp += L".acpr.tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD err = WriteWholeHandle(h, bytes);
    CloseHandle(h);
    if (err != ERROR_SUCCESS) {
        DeleteFileW(tmp.c_str());
        return err;
    }
    BOOL ok;
    if (exists) {
        ok = ReplaceFileW(target.c_str(), tmp.c_str(), nullptr,
                          REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS, nullptr, nullptr);
    } else {
        ok = MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH);
    }
    if (!ok) {
        err = GetLastError();
        DeleteFileW(tmp.c_str());
        return err;
    }
    return ERROR_SUCCESS;
}

DWORD TryInPlace(const fs::path& target, std::string_view bytes) {
    HANDLE h = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    const DWORD err = WriteWholeHandle(h, bytes);
    CloseHandle(h);
    return err;
}

}  // namespace

WriteResult WriteFileAtomic(const fs::path& target, std::string_view bytes, WriteStrategy strategy, bool allowCreate) {
    static constexpr std::array<int, 6> kDelaysMs{15, 30, 60, 120, 250, 500};
    WriteResult r;
    for (std::size_t attempt = 0;; ++attempt) {
        ++r.attempts;
        const DWORD err = strategy == WriteStrategy::AtomicReplace ? TryReplace(target, bytes, allowCreate)
                                                                  : TryInPlace(target, bytes);
        if (err == ERROR_SUCCESS) {
            r.ok = true;
            r.lastError = 0;
            return r;
        }
        r.lastError = err;
        r.retryable = IsRetryable(err);
        if (!r.retryable || attempt >= kDelaysMs.size()) return r;
        std::this_thread::sleep_for(std::chrono::milliseconds(kDelaysMs[attempt]));
    }
}

std::uint64_t Fnv1a64(std::string_view data) noexcept {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : data) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}

std::wstring FormatWin32Error(std::uint32_t code) {
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring s = n && buf ? std::wstring(buf, n) : L"Error " + std::to_wstring(code);
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
    return s;
}

std::string Narrow(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

namespace log {
namespace {
std::mutex g_mutex;
std::string g_buffer;
fs::path g_file;
}  // namespace

void SetFile(const fs::path& path) {
    std::lock_guard lock(g_mutex);
    g_file = path;
}

void Write(std::string_view line) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%02u:%02u:%02u.%03u  ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    std::lock_guard lock(g_mutex);
    if (g_buffer.size() > (1u << 20)) return;  // guard against unbounded growth
    g_buffer += stamp;
    g_buffer += line;
    g_buffer += "\r\n";
}

void Flush() {
    std::string data;
    fs::path file;
    {
        std::lock_guard lock(g_mutex);
        if (g_buffer.empty() || g_file.empty()) return;
        data.swap(g_buffer);
        file = g_file;
    }
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    HANDLE h = CreateFileW(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(h);
}
}  // namespace log

}  // namespace acp::platform
