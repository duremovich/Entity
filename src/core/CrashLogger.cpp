/**
 * CrashLogger — SEH minidump + context.json capture.
 *
 * SEH filter invariants:
 *   - The heap may be corrupt on AV/access-violation. Everything in the filter
 *     uses pre-allocated static storage filled at install() time.
 *   - No new/malloc/std::string/std::filesystem inside the filter.
 *   - No locks — InterlockedExchange on s_inFilter to serialize re-entry.
 *   - TerminateProcess after writing artifacts to bypass WER dialogs.
 *
 * captureNonFatal (device-lost path) runs on the show thread with heap intact;
 * it may use normal stdlib calls.
 */

#include "entity/core/CrashLogger.hpp"
#include "entity/core/Settings.hpp"
#include <entity/BuildInfo.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <DbgHelp.h>
#include <dxgi1_4.h>
#include <winerror.h>
#pragma comment(lib, "dbghelp.lib")

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <typeinfo>
#include <vector>

namespace entity {

// ---------------------------------------------------------------------------
// Static storage for the SEH filter — no heap allocation allowed inside filter
// ---------------------------------------------------------------------------

static constexpr int kMaxPathW    = MAX_PATH + 1;
static constexpr int kMaxJsonBuf  = 8192;

// Wide-char paths pre-computed at install() time
static wchar_t s_dumpPathW   [kMaxPathW]  = {};
static wchar_t s_jsonPathW   [kMaxPathW]  = {};

// Context.json scratch buffer — fixed size, filled in install() template parts
// then overwritten in captureNonFatal/SEH filter.
static char    s_jsonBuf     [kMaxJsonBuf] = {};

// Pre-opened HANDLE for context.json (opened in install(), closed in teardown())
// For SEH filter use: the filter writes directly to this handle.
// For captureNonFatal: a fresh handle is opened to a new directory.
// (We can't reuse the pre-opened one because captureNonFatal creates a new dir.)
static HANDLE  s_jsonHandle  = INVALID_HANDLE_VALUE;
static HANDLE  s_dumpHandle  = INVALID_HANDLE_VALUE;

// Re-entry guard
static volatile LONG s_inFilter = 0;

// Fields that can be safely set from any thread before crash (writes happen
// only from editor thread, reads from the filter; acceptable race for forensics).
static char s_projectPathUtf8[MAX_PATH * 2] = {};
static char s_adapterDescUtf8[256]          = {};
static UINT s_vendorId   = 0;
static UINT s_deviceId   = 0;

// Root crash-logs dir (UTF-8 + wide), set once in install()
static char    s_crashLogsRootUtf8[MAX_PATH * 2] = {};
static wchar_t s_crashLogsRootW   [kMaxPathW]    = {};

// Timestamp string for the pre-allocated crash dir (set at install() time as
// a "this-session" fallback; captureNonFatal generates a fresh one at call time).
static char    s_sessionDirUtf8   [64]           = {};

// ---------------------------------------------------------------------------
// Helpers — all allocation-free (wchar_t / raw char only)
// ---------------------------------------------------------------------------

static void makeUtcIsoNow(char* buf, int bufLen) {
    SYSTEMTIME st;
    GetSystemTime(&st);
    _snprintf_s(buf, bufLen, _TRUNCATE,
        "%04d-%02d-%02dT%02d-%02d-%02dZ",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
}

// Append to a fixed char buffer without std::string.
static int appendStr(char* dst, int dstLen, int pos, const char* src) {
    while (pos < dstLen - 1 && *src) dst[pos++] = *src++;
    dst[pos] = '\0';
    return pos;
}

// JSON-escape src into dst, returning new position.
static int jsonEscape(char* dst, int dstLen, int pos, const char* src) {
    if (!src) { return appendStr(dst, dstLen, pos, "null"); }
    pos = appendStr(dst, dstLen, pos, "\"");
    for (; *src && pos < dstLen - 4; ++src) {
        unsigned char c = static_cast<unsigned char>(*src);
        if (c == '"')       { dst[pos++] = '\\'; dst[pos++] = '"'; }
        else if (c == '\\') { dst[pos++] = '\\'; dst[pos++] = '\\'; }
        else if (c == '\n') { dst[pos++] = '\\'; dst[pos++] = 'n'; }
        else if (c == '\r') { dst[pos++] = '\\'; dst[pos++] = 'r'; }
        else if (c < 0x20)  { /* skip control chars */ }
        else                { dst[pos++] = static_cast<char>(c); }
    }
    pos = appendStr(dst, dstLen, pos, "\"");
    return pos;
}

// Convert a UTF-16 WCHAR string to UTF-8 into a fixed-size buffer.
static void wcharToUtf8(const wchar_t* src, char* dst, int dstLen) {
    if (!src || dstLen <= 0) { if (dst && dstLen > 0) dst[0] = '\0'; return; }
    WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dstLen, nullptr, nullptr);
    dst[dstLen - 1] = '\0';
}

// Build a context.json payload into s_jsonBuf using only fixed-size buffers.
// exceptionCode and hresult are 0 if not applicable.
static int buildJsonPayload(const char* kind,
                             DWORD exceptionCode,
                             LONG  hresult,
                             const char* dredBreadcrumbs,
                             const char* extra,
                             const char* timestamp)
{
    // Use RtlGetVersion which works on Win10/11 without manifest tricks.
    char osVer[64] = "Windows";
    {
        using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        if (HMODULE ntdll = GetModuleHandleA("ntdll.dll")) {
            auto fn = reinterpret_cast<RtlGetVersionFn>(
                GetProcAddress(ntdll, "RtlGetVersion"));
            if (fn) {
                RTL_OSVERSIONINFOW rovi{sizeof(rovi)};
                if (fn(&rovi) == 0 /*STATUS_SUCCESS*/) {
                    _snprintf_s(osVer, sizeof(osVer), _TRUNCATE,
                        "Windows %lu.%lu.%lu",
                        rovi.dwMajorVersion, rovi.dwMinorVersion, rovi.dwBuildNumber);
                }
            }
        }
    }

    char excCodeHex[20] = {};
    _snprintf_s(excCodeHex, sizeof(excCodeHex), _TRUNCATE, "0x%08lX", (unsigned long)exceptionCode);
    char hrHex[20] = {};
    _snprintf_s(hrHex, sizeof(hrHex), _TRUNCATE, "0x%08lX", (unsigned long)(DWORD)hresult);
    char vendorHex[20] = {}, deviceHex[20] = {};
    _snprintf_s(vendorHex, sizeof(vendorHex), _TRUNCATE, "0x%04X", s_vendorId);
    _snprintf_s(deviceHex, sizeof(deviceHex), _TRUNCATE, "0x%04X", s_deviceId);

    int p = 0;
    p = appendStr(s_jsonBuf, kMaxJsonBuf, p, "{\n");
    p = appendStr(s_jsonBuf, kMaxJsonBuf, p, "  \"timestamp\": "); p = jsonEscape(s_jsonBuf, kMaxJsonBuf, p, timestamp);       p = appendStr(s_jsonBuf, kMaxJsonBuf, p, ",\n");
    p = appendStr(s_jsonBuf, kMaxJsonBuf, p, "  \"kind\": ");      p = jsonEscape(s_jsonBuf, kMaxJsonBuf, p, kind);             p = appendStr(s_jsonBuf, kMaxJsonBuf, p, ",\n");
    p = appendStr(s_jsonBuf, kMaxJsonBuf, p, "  \"exceptionCode\": "); p = jsonEscape(s_jsonBuf, kMaxJsonBuf, p, exceptionCode ? excCodeHex : nullptr); p = appendStr(s_jsonBuf, kMaxJsonBuf, p, ",\n");
    p = appendStr(s_jsonBuf, kMaxJsonBuf, p, "  \"hresult\": ");   p = jsonEscape(s_jsonBuf, kMaxJsonBuf, p, hresult ? hrHex : nullptr);               p = appendStr(s_jsonBuf, kMaxJsonBuf, p, ",\n");
    p = appendStr(s_jsonBuf, kMaxJsonBuf, p, "  \"osVersion\": "); p = jsonEscape(s_jsonBuf, kMaxJsonBuf, p, osVer);            p = appendStr(s_jsonBuf, kMaxJsonBuf, p, ",\n");
    p = appendStr(s_jsonBuf, kMaxJsonBuf, p, "  \"buildCommit\": "); p = jsonEscape(s_jsonBuf, kMaxJsonBuf, p, ENTITY_BUILD_COMMIT); p = appendStr(s_jsonBuf, kMaxJsonBuf, p, ",\n");
    p = appendStr(s_jsonBuf, kMaxJsonBuf, p, "  \"projectPath\": "); p = jsonEscape(s_jsonBuf, kMaxJsonBuf, p, s_projectPathUtf8); p = appendStr(s_jsonBuf, kMaxJsonBuf, p, ",\n");
    p = appendStr(s_jsonBuf, kMaxJsonBuf, p, "  \"adapterDescription\": "); p = jsonEscape(s_jsonBuf, kMaxJsonBuf, p, s_adapterDescUtf8); p = appendStr(s_jsonBuf, kMaxJsonBuf, p, ",\n");
    p = appendStr(s_jsonBuf, kMaxJsonBuf, p, "  \"vendorId\": ");  p = appendStr(s_jsonBuf, kMaxJsonBuf, p, vendorHex);         p = appendStr(s_jsonBuf, kMaxJsonBuf, p, ",\n");
    p = appendStr(s_jsonBuf, kMaxJsonBuf, p, "  \"deviceId\": ");  p = appendStr(s_jsonBuf, kMaxJsonBuf, p, deviceHex);         p = appendStr(s_jsonBuf, kMaxJsonBuf, p, ",\n");
    p = appendStr(s_jsonBuf, kMaxJsonBuf, p, "  \"dredBreadcrumbs\": "); p = jsonEscape(s_jsonBuf, kMaxJsonBuf, p, dredBreadcrumbs); p = appendStr(s_jsonBuf, kMaxJsonBuf, p, ",\n");
    p = appendStr(s_jsonBuf, kMaxJsonBuf, p, "  \"extra\": ");     p = jsonEscape(s_jsonBuf, kMaxJsonBuf, p, extra);            p = appendStr(s_jsonBuf, kMaxJsonBuf, p, "\n");
    p = appendStr(s_jsonBuf, kMaxJsonBuf, p, "}\n");
    return p;
}

// ---------------------------------------------------------------------------
// SEH filter — must be allocation-free
// ---------------------------------------------------------------------------

static LONG WINAPI sehFilter(LPEXCEPTION_POINTERS info) {
    // Serialize re-entrant calls (e.g. AV inside the filter itself).
    if (InterlockedExchange(&s_inFilter, 1L) != 0L) {
        TerminateProcess(GetCurrentProcess(), 1);
        return EXCEPTION_EXECUTE_HANDLER; // unreachable
    }

    // Write minidump -------------------------------------------------------
    if (s_dumpHandle != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId          = GetCurrentThreadId();
        mei.ExceptionPointers = info;
        mei.ClientPointers    = FALSE;

        constexpr MINIDUMP_TYPE kFlags = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithDataSegs |
            MiniDumpWithUnloadedModules |
            MiniDumpWithThreadInfo);

        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                          s_dumpHandle, kFlags, &mei, nullptr, nullptr);
        CloseHandle(s_dumpHandle);
        s_dumpHandle = INVALID_HANDLE_VALUE;
    }

    // Write context.json ---------------------------------------------------
    if (s_jsonHandle != INVALID_HANDLE_VALUE) {
        char ts[64] = {};
        makeUtcIsoNow(ts, sizeof(ts));

        DWORD excCode = info ? info->ExceptionRecord->ExceptionCode : 0;

        // For MSVC C++ throws (0xE06D7363) the thrown object pointer lives at
        // ExceptionInformation[1]. If it derives from std::exception the
        // vtable is at offset 0 and what() is reachable via static_cast.
        // Wrap the deref in __try so a non-std::exception throw (or a
        // corrupt vtable) falls back instead of taking down the filter.
        // No heap allocation — writes into a fixed stack buffer.
        char extraBuf[512] = {};
        const char* extraPtr = nullptr;
        if (info && excCode == 0xE06D7363u &&
            info->ExceptionRecord->NumberParameters >= 2) {
            const void* exObj = reinterpret_cast<const void*>(
                info->ExceptionRecord->ExceptionInformation[1]);
            __try {
                const std::exception* e =
                    static_cast<const std::exception*>(exObj);
                const char* what = e->what();
                if (what) {
                    _snprintf_s(extraBuf, sizeof(extraBuf), _TRUNCATE,
                                "C++ throw what(): %s", what);
                    extraPtr = extraBuf;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                _snprintf_s(extraBuf, sizeof(extraBuf), _TRUNCATE,
                            "C++ throw: object not derived from std::exception "
                            "(or vtable unreadable); see minidump");
                extraPtr = extraBuf;
            }
        }

        int payloadLen = buildJsonPayload("seh", excCode, 0, nullptr, extraPtr, ts);

        DWORD written = 0;
        WriteFile(s_jsonHandle, s_jsonBuf, static_cast<DWORD>(payloadLen), &written, nullptr);
        CloseHandle(s_jsonHandle);
        s_jsonHandle = INVALID_HANDLE_VALUE;
    }

    // Skip WER entirely.
    TerminateProcess(GetCurrentProcess(), 1);
    return EXCEPTION_EXECUTE_HANDLER; // unreachable
}

// ---------------------------------------------------------------------------
// std::terminate handler (heap assumed intact — normal stdlib allowed)
// ---------------------------------------------------------------------------

static void terminateHandler() {
    // MSVC's C++ exception flow runs std::terminate before our SEH filter
    // catches the resulting abort, so std::current_exception() is still live
    // here. Rethrow into a typed catch to pull the exception type + what().
    // Without this, the crash dir gets a generic "std::terminate called"
    // entry and the actual cause stays buried in the minidump.
    std::string detail = "std::terminate called";
    if (auto eptr = std::current_exception()) {
        try {
            std::rethrow_exception(eptr);
        } catch (const std::exception& e) {
            detail = std::string("std::terminate: ")
                   + typeid(e).name() + ": " + e.what();
        } catch (...) {
            detail = "std::terminate: unknown non-std::exception throw";
        }
    }
    CrashLogger::captureNonFatal("terminate", 0, "", detail);
    TerminateProcess(GetCurrentProcess(), 1);
}

// ---------------------------------------------------------------------------
// Rotation helper — two passes (issue #69 triage, 2026-07-04):
//
//  1. Delete "litter" dirs: every contained file 0-byte, and the dir old
//     enough that no live process is still writing it. These are the
//     install()-time pre-created session dirs of processes that died without
//     teardown() — e.g. ctest SIGKILL after a device-lost teardown hang —
//     not real crash records. The age guard protects a concurrent process's
//     just-created capture dir; a live process's own session dir is
//     additionally protected by its open handles (delete fails, skipped).
//
//  2. Cap the remaining dirs at kMaxDirs by mtime, oldest deleted first.
//     kMaxDirs raised 10 → 30: an integration-suite run spawns ~700 editor
//     processes (install() rotates in each), and a wedge window writes a
//     dozen real crash dirs in minutes — cap 10 rotated away the storm's
//     trigger records before anyone could read them.
// ---------------------------------------------------------------------------

static void rotateCrashLogs(const std::filesystem::path& root, int kMaxDirs = 30) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(root, ec)) return;

    constexpr auto kLitterMinAge = std::chrono::minutes(5);
    const auto now = fs::file_time_type::clock::now();

    std::vector<std::pair<fs::file_time_type, fs::path>> dirs;
    for (auto& e : fs::directory_iterator(root, ec)) {
        if (!e.is_directory(ec)) continue;

        bool allEmpty = true;
        for (auto& f : fs::directory_iterator(e.path(), ec)) {
            if (!f.is_regular_file(ec) || f.file_size(ec) > 0) {
                allEmpty = false;
                break;
            }
        }
        const auto mtime = e.last_write_time(ec);
        if (ec) continue;

        if (allEmpty && (now - mtime) > kLitterMinAge) {
            fs::remove_all(e.path(), ec); // open handles → fails, skipped
            continue;
        }
        dirs.push_back({mtime, e.path()});
    }

    if (static_cast<int>(dirs.size()) <= kMaxDirs) return;
    std::sort(dirs.begin(), dirs.end()); // oldest first
    int toDelete = static_cast<int>(dirs.size()) - kMaxDirs;
    for (int i = 0; i < toDelete; ++i) {
        fs::remove_all(dirs[i].second, ec);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void CrashLogger::install() {
    // Compute crash-logs root and store as both UTF-8 and wide.
    std::filesystem::path root = crashLogsRoot();
    {
        std::string rootStr = root.string();
        strncpy_s(s_crashLogsRootUtf8, sizeof(s_crashLogsRootUtf8), rootStr.c_str(), _TRUNCATE);
    }
    {
        std::wstring rootW = root.wstring();
        wcsncpy_s(s_crashLogsRootW, kMaxPathW, rootW.c_str(), _TRUNCATE);
    }

    // Rotate old logs.
    try { rotateCrashLogs(root); } catch (...) {}

    // Pre-compute a session crash dir: root/<UTC-iso>-startup
    // If a crash fires before captureNonFatal's per-call dir is created, the
    // SEH filter writes here.
    char tsBuf[64] = {};
    makeUtcIsoNow(tsBuf, sizeof(tsBuf));
    strncpy_s(s_sessionDirUtf8, sizeof(s_sessionDirUtf8), tsBuf, _TRUNCATE);

    char dirUtf8[MAX_PATH * 2] = {};
    _snprintf_s(dirUtf8, sizeof(dirUtf8), _TRUNCATE, "%s\\%s", s_crashLogsRootUtf8, tsBuf);

    // Convert dir to wide and pre-open handles.
    wchar_t dirW[kMaxPathW] = {};
    MultiByteToWideChar(CP_UTF8, 0, dirUtf8, -1, dirW, kMaxPathW);

    // Pre-create the dir so the filter's CreateFile succeeds.
    std::filesystem::create_directories(dirUtf8);

    // Dump path
    _snwprintf_s(s_dumpPathW, kMaxPathW, _TRUNCATE, L"%s\\dump.dmp", dirW);
    // JSON path
    _snwprintf_s(s_jsonPathW, kMaxPathW, _TRUNCATE, L"%s\\context.json", dirW);

    // Pre-open dump file
    s_dumpHandle = CreateFileW(s_dumpPathW,
        GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);

    // Pre-open context.json
    s_jsonHandle = CreateFileW(s_jsonPathW,
        GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);

    // Suppress WER dialog even if TerminateProcess path doesn't run.
    SetErrorMode(SEM_NOGPFAULTERRORBOX);

    SetUnhandledExceptionFilter(sehFilter);
    std::set_terminate(terminateHandler);
}

void CrashLogger::teardown() {
    SetUnhandledExceptionFilter(nullptr);
    std::set_terminate(nullptr);

    if (s_dumpHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(s_dumpHandle);
        s_dumpHandle = INVALID_HANDLE_VALUE;
        // Clean up the pre-created empty dump file since no crash occurred.
        DeleteFileW(s_dumpPathW);
    }
    if (s_jsonHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(s_jsonHandle);
        s_jsonHandle = INVALID_HANDLE_VALUE;
        DeleteFileW(s_jsonPathW);
    }

    // Remove the pre-created empty session dir if it's empty.
    wchar_t dirW[kMaxPathW] = {};
    _snwprintf_s(dirW, kMaxPathW, _TRUNCATE, L"%s\\%hs", s_crashLogsRootW, s_sessionDirUtf8);
    RemoveDirectoryW(dirW); // no-op if non-empty (captureNonFatal wrote to it)
}

void CrashLogger::setProjectPath(const std::filesystem::path& p) {
    std::string s = p.string();
    strncpy_s(s_projectPathUtf8, sizeof(s_projectPathUtf8), s.c_str(), _TRUNCATE);
}

void CrashLogger::setAdapterDesc(const DXGI_ADAPTER_DESC1& desc) {
    wcharToUtf8(desc.Description, s_adapterDescUtf8, sizeof(s_adapterDescUtf8));
    s_vendorId = desc.VendorId;
    s_deviceId = desc.DeviceId;
}

void CrashLogger::captureNonFatal(const char* kind,
                                   long hresult,
                                   std::string_view dredBreadcrumbs,
                                   std::string_view extra)
{
    // Normal stdlib path — heap assumed intact.
    try {
        std::filesystem::path root = crashLogsRoot();

        char tsBuf[64] = {};
        makeUtcIsoNow(tsBuf, sizeof(tsBuf));

        // Capture dirs are "<ts>-<kind>-<pid>", NOT the bare "<ts>" the
        // session dir uses. A process that crashes within the same second it
        // started would otherwise collide with its own pre-created session
        // dir, whose context.json/dump.dmp are pre-opened with no sharing —
        // both CreateFileW calls below fail silently and the crash record is
        // lost (observed 2026-07-04T15-10-59Z: WARP DEVICE_REMOVED 0.98s
        // after process start left only 0-byte files; likely also #71's
        // "0-byte single entry" from 2026-06-12). The PID disambiguates two
        // processes crashing in the same second during a storm.
        char dirName[128] = {};
        _snprintf_s(dirName, sizeof(dirName), _TRUNCATE, "%s-%s-%lu",
                    tsBuf, kind, GetCurrentProcessId());
        std::filesystem::path dir = root / dirName;
        std::filesystem::create_directories(dir);

        // Write context.json via std::ofstream (heap is fine here).
        std::string dredStr(dredBreadcrumbs);
        std::string extraStr(extra);

        // Reuse buildJsonPayload into s_jsonBuf (single-threaded from show thread).
        int payloadLen = buildJsonPayload(kind,
                                          0,
                                          static_cast<LONG>(hresult),
                                          dredStr.empty() ? nullptr : dredStr.c_str(),
                                          extraStr.empty() ? nullptr : extraStr.c_str(),
                                          tsBuf);

        std::filesystem::path jsonPath = dir / "context.json";
        HANDLE h = CreateFileW(jsonPath.wstring().c_str(),
            GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(h, s_jsonBuf, static_cast<DWORD>(payloadLen), &written, nullptr);
            CloseHandle(h);
        }

        // Write a minidump for device-lost events (DRED breadcrumbs are in it).
        if (std::string_view(kind) == "device-lost") {
            std::filesystem::path dumpPath = dir / "dump.dmp";
            HANDLE hDump = CreateFileW(dumpPath.wstring().c_str(),
                GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hDump != INVALID_HANDLE_VALUE) {
                constexpr MINIDUMP_TYPE kFlags = static_cast<MINIDUMP_TYPE>(
                    MiniDumpWithDataSegs |
                    MiniDumpWithUnloadedModules |
                    MiniDumpWithThreadInfo);
                MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                  hDump, kFlags, nullptr, nullptr, nullptr);
                CloseHandle(hDump);
            }
        }

        // Rotate after writing so the new dir is counted in the limit.
        rotateCrashLogs(root);
    } catch (...) {
        // Swallow — we're already in a failure path.
    }
}

std::filesystem::path crashLogsRoot() {
    // Mirror the appDataRoot() logic from Settings.cpp.
#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA")) {
        return std::filesystem::path(appdata) / "Entity" / "crash-logs";
    }
    if (const char* up = std::getenv("USERPROFILE")) {
        return std::filesystem::path(up) / "AppData" / "Roaming" / "Entity" / "crash-logs";
    }
    return std::filesystem::path(".") / ".entity" / "crash-logs";
#else
    return std::filesystem::path(".") / ".entity" / "crash-logs";
#endif
}

} // namespace entity

#else // !_WIN32

// Stub for non-Windows builds.
namespace entity {
void CrashLogger::install() {}
void CrashLogger::teardown() {}
void CrashLogger::setProjectPath(const std::filesystem::path&) {}
void CrashLogger::captureNonFatal(const char*, long, std::string_view, std::string_view) {}
std::filesystem::path crashLogsRoot() { return {}; }
} // namespace entity

#endif // _WIN32
