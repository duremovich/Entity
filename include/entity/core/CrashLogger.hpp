#pragma once

#include <filesystem>
#include <string_view>

#ifdef _WIN32
#include <dxgi1_4.h>
#endif

namespace entity {

/**
 * CrashLogger — minimal forensic capture for SEH faults and device-lost events.
 *
 * Output: %APPDATA%/Entity/crash-logs/<UTC-iso>/dump.dmp + context.json
 * Rotation: last 10 directories kept on install(); oldest deleted by mtime.
 *
 * SEH filter rules (must hold — heap may be corrupt):
 *   - No allocations inside the filter. All paths pre-allocated at install() time.
 *   - No locks. InterlockedExchange used to serialize re-entrant calls.
 *   - Calls TerminateProcess directly to skip WER dialogs.
 *
 * captureNonFatal is safe to call from any thread (controlled-exit path, heap intact).
 */
class CrashLogger {
public:
    static void install();
    static void teardown();

    static void setProjectPath(const std::filesystem::path& p);

#ifdef _WIN32
    static void setAdapterDesc(const DXGI_ADAPTER_DESC1& desc);
#endif

    // Called from D3D12Renderer::handleDeviceLost — controlled exit, not an SEH unwind.
    // hresult is the raw HRESULT. dredBreadcrumbs and extra are optional diagnostic strings.
    static void captureNonFatal(const char* kind,
                                long hresult,
                                std::string_view dredBreadcrumbs,
                                std::string_view extra);
};

// Returns %APPDATA%/Entity/crash-logs — same root that install() writes to.
// Used by Phase 3's loop-guard to enumerate recent crash entries.
std::filesystem::path crashLogsRoot();

} // namespace entity
