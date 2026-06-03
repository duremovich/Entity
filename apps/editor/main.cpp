#include "entity/core/Engine.hpp"
#include "entity/core/Types.hpp"
#include "entity/core/EnginePluginContext.hpp"
#include "entity/core/CrashLogger.hpp"
#include "entity/command/CommandDispatcher.hpp"
#include "entity/plugin/Plugin.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellscalingapi.h>
#pragma comment(lib, "shcore.lib")
#endif

/**
 * Entity Media Server - Editor Application
 *
 * Main entry point for the editor application.
 *
 * Command-line arguments:
 *   --script <path>                Run a JSON script file after initialization
 *   --device-lost-recovery <path>  Recovery launch: load autosave path with
 *                                  ERROR_SHARING_VIOLATION retry logic
 *   --headless                     Run with a hidden window (for tests / CI)
 *   --help                         Show help message
 */

void printHelp() {
    std::cout << "Usage: EntityMediaEditor [options] [<project.entity>]" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --script <path>   Run a JSON script file after initialization" << std::endl;
    std::cout << "  --headless        Run with a hidden window (for integration tests / CI)" << std::endl;
    std::cout << "  --help            Show this help message" << std::endl;
    std::cout << std::endl;
    std::cout << "A trailing positional <project.entity> path opens that project" << std::endl;
    std::cout << "directly, bypassing the launcher. This is the path the OS hands" << std::endl;
    std::cout << "the editor when a .entity file is opened via Explorer / Finder" << std::endl;
    std::cout << "double-click after a file association is set." << std::endl;
    std::cout << std::endl;
    std::cout << "Example:" << std::endl;
    std::cout << "  EntityMediaEditor --script scripts/test.json" << std::endl;
    std::cout << "  EntityMediaEditor --headless --script scripts/integration/seek.json" << std::endl;
    std::cout << "  EntityMediaEditor \"C:\\Shows\\IIWY\\IIWY.entity\"" << std::endl;
}

// Loop guard: scan the last 30 seconds of crash-log entries for device-lost
// events referencing this project path. If > 2 found, the system is in a
// device-lost storm; skip auto-load and fall through to the launcher.
static bool shouldSkipAutoLoad(const std::filesystem::path& projectPath) {
#ifdef _WIN32
    namespace fs = std::filesystem;
    fs::path logsRoot = entity::crashLogsRoot();
    if (!fs::exists(logsRoot)) return false;

    auto now = std::chrono::system_clock::now();
    int count = 0;

    std::error_code ec;
    for (auto& entry : fs::directory_iterator(logsRoot, ec)) {
        if (!entry.is_directory()) continue;
        fs::path jsonPath = entry.path() / "context.json";
        if (!fs::exists(jsonPath)) continue;

        // Check file mtime — skip dirs older than 30s.
        auto mtime = fs::last_write_time(jsonPath, ec);
        if (ec) continue;
        // Convert file_time_type to system_clock::time_point via duration cast.
        auto mtimeSys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            mtime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        auto age = std::chrono::duration_cast<std::chrono::seconds>(now - mtimeSys);
        if (age.count() > 30) continue;

        // Read and parse enough of context.json to check kind + projectPath.
        std::ifstream f(jsonPath);
        if (!f) continue;
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());

        if (content.find("\"device-lost\"") == std::string::npos) continue;

        // Match project path: the JSON field stores the path as a UTF-8 string.
        std::string projStr = projectPath.string();
        // Strip the ".autosave.entity" suffix to match the base project path
        // stored in context.json (setProjectPath is called with the real path,
        // not the autosave path).
        constexpr std::string_view kAutosaveSuffix = ".autosave.entity";
        if (projStr.size() > kAutosaveSuffix.size() &&
            projStr.substr(projStr.size() - kAutosaveSuffix.size()) == kAutosaveSuffix) {
            // Replace suffix with ".entity" to reconstruct the original path.
            projStr = projStr.substr(0, projStr.size() - kAutosaveSuffix.size()) + ".entity";
        }
        if (content.find(projStr) != std::string::npos) {
            ++count;
        }
    }

    if (count > 2) {
        std::cerr << "[main] Device-lost loop guard: " << count
                  << " device-lost entries for this project in the last 30s — "
                  << "skipping auto-load and falling through to launcher." << std::endl;
        return true;
    }
#endif
    return false;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    // Declare per-monitor DPI awareness BEFORE GLFW creates a window. Without
    // this, Windows silently virtualizes the app at display-scale != 100%
    // (e.g. 150% scaling), which causes a multiplicative drift between mouse
    // input and rendering — visible as buttons whose hit-test rects move
    // away from their visual position the further down the window you go.
    // Per-monitor V2 lets the OS keep ImGui's mouse coords and render coords
    // in lock-step at any scale and across multi-monitor setups.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Install SEH + terminate crash handlers before anything else so even
    // failures during Engine::initialize produce a forensic dump.
    entity::CrashLogger::install();
#endif

    std::cout << "========================================" << std::endl;
    std::cout << "Entity Media Server - Editor" << std::endl;
    std::cout << "Version 0.1.0" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    // Parse command-line arguments
    std::string scriptPath;
    std::string projectPath;         // Positional <project.entity> for OS file-association open
    std::string recoveryAutosavePath; // --device-lost-recovery <path>
    bool headless = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            printHelp();
            return EXIT_SUCCESS;
        }
        else if (arg == "--script" || arg == "-s") {
            if (i + 1 < argc) {
                scriptPath = argv[++i];
            } else {
                std::cerr << "Error: --script requires a path argument" << std::endl;
                return EXIT_FAILURE;
            }
        }
        else if (arg == "--headless") {
            headless = true;
        }
        else if (arg == "--device-lost-recovery") {
            if (i + 1 < argc) {
                recoveryAutosavePath = argv[++i];
            } else {
                std::cerr << "Error: --device-lost-recovery requires a path argument" << std::endl;
                return EXIT_FAILURE;
            }
        }
        else if (!arg.empty() && arg[0] != '-') {
            // Positional path. Windows hands this to the editor when a .entity
            // file is opened via Explorer / file association. Reject a second
            // positional so a typo'd flag doesn't silently get treated as a
            // path.
            if (!projectPath.empty()) {
                std::cerr << "Error: more than one project path supplied ("
                          << projectPath << ", " << arg << ")" << std::endl;
                return EXIT_FAILURE;
            }
            projectPath = arg;
        }
        else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            std::cerr << "Use --help for usage information" << std::endl;
            return EXIT_FAILURE;
        }
    }

    if (!scriptPath.empty() && !projectPath.empty()) {
        std::cerr << "Error: cannot combine --script with a positional project "
                     "path. Scripts run their own OpenProject command if they "
                     "need a project loaded." << std::endl;
        return EXIT_FAILURE;
    }

    // Create engine instance
    entity::Engine engine;

    // Mark recovery relaunch before initialize so the engine suppresses
    // m_relaunchRequested on any subsequent device-loss (single-relaunch latch).
    if (!recoveryAutosavePath.empty()) {
        engine.setRecoveryRelaunch();
    }

    // Initialize engine
    entity::Result result = engine.initialize(
        1920,  // Width
        1080,  // Height
        "Entity Media Server - Editor",
        headless
    );

    if (result != entity::Result::Success) {
        std::cerr << "Failed to initialize engine: "
                  << entity::ResultToString(result) << std::endl;
        return EXIT_FAILURE;
    }

    // Register all static-linked plugins. The dispatcher is generated by
    // CMake from `plugins/StaticRegistry.cpp.in` and walks every plugin
    // that called `entity_register_plugin()` in its CMakeLists.txt.
    entity::core::EnginePluginContext pluginCtx(&engine, "engine");
    entity::plugin::registerStaticPlugins(&pluginCtx);
    // Wire the context into Engine so postSignalEmit() is reachable from
    // the show thread (Phase 6) and script command handlers (Phase 2+).
    engine.setPluginContext(&pluginCtx);

    // Run script if specified
    if (!scriptPath.empty()) {
        std::cout << "Loading script: " << scriptPath << std::endl;
        if (!engine.runScript(scriptPath)) {
            std::cerr << "Failed to load script: " << scriptPath << std::endl;
            // Continue running anyway - don't exit on script failure
        }
    } else if (!recoveryAutosavePath.empty()) {
        // Device-lost recovery launch. Load the autosave with retry-on-sharing-
        // violation — the parent process may still be writing the file.
        namespace fs = std::filesystem;
        fs::path autosavePath = recoveryAutosavePath;
        bool fileReady = false;
#ifdef _WIN32
        for (int attempt = 0; attempt < 10; ++attempt) {
            HANDLE h = CreateFileW(autosavePath.wstring().c_str(),
                                   GENERIC_READ, FILE_SHARE_READ, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
                fileReady = true;
                break;
            }
            DWORD err = GetLastError();
            if (err != ERROR_SHARING_VIOLATION && err != ERROR_FILE_NOT_FOUND) break;
            std::cerr << "[main] --device-lost-recovery: file not ready (err=" << err
                      << ") on attempt " << (attempt + 1) << ", retrying..." << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
#else
        fileReady = std::filesystem::exists(autosavePath);
#endif
        if (!fileReady) {
            std::cerr << "[main] --device-lost-recovery failed to open "
                      << autosavePath << " — falling through to launcher." << std::endl;
            engine.showLauncher();
        } else {
            std::cerr << "[main] --device-lost-recovery: loading " << autosavePath << std::endl;
            engine.requestProjectLoad(autosavePath);
        }
    } else if (!projectPath.empty()) {
        // OS file-association / "open with" flow. Skip the launcher and
        // stage the load so it runs on the first render frame, with a
        // "Loading..." overlay painted in between (see
        // Engine::render()'s file-association handshake). The previous
        // synchronous loadProject() before run() left the user staring
        // at a blank window during deserialize. Engine handles the
        // failure-to-launcher fallback internally.
        std::cout << "Opening project: " << projectPath << std::endl;
        engine.requestProjectLoad(std::filesystem::path(projectPath));
    } else {
        // ADR-0009 — interactive launches enter the Project Launcher
        // first; the editor only opens once a project root exists.
        // Script-driven runs (--script) skip the launcher because the
        // script is expected to OpenProject explicitly. Headless +
        // script combinations on CI continue to work unchanged.
        engine.showLauncher();
    }

    // Run main loop
    engine.run();

    // In script-driven runs (integration tests), propagate command failures
    // to the process exit code so CI can detect them.
    bool scriptFailed = false;
    if (!scriptPath.empty()) {
        auto* dispatcher = engine.getCommandDispatcher();
        if (dispatcher && dispatcher->hasErrors()) {
            std::cerr << "Script reported command failures: "
                      << dispatcher->getScriptResults().dump() << std::endl;
            scriptFailed = true;
        }
    }

    // Shutdown (automatic via destructor, but explicit is clearer)
    engine.shutdown();

    // Remove pre-opened crash handles now that the engine has exited cleanly.
    entity::CrashLogger::teardown();

    // Device-lost auto-relaunch (Step 5). Only fires if:
    //   - The engine flagged m_relaunchRequested (autosave succeeded)
    //   - This is NOT already a recovery process (single-relaunch latch via m_isRecoveryRelaunch)
    //   - The loop guard finds < 3 recent device-lost entries for this project
#ifdef _WIN32
    if (engine.shouldRelaunch() && !scriptFailed) {
        std::filesystem::path autosavePath = engine.relaunchProjectPath();
        if (!shouldSkipAutoLoad(autosavePath)) {
            // Get path to our own executable.
            wchar_t exePathBuf[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exePathBuf, MAX_PATH);
            std::wstring exePath(exePathBuf);

            // Build command line: "<exe>" --device-lost-recovery "<autosave>"
            // cmd.data() (not c_str()) — OS writes a null terminator into the buffer.
            std::wstring cmd = L"\"" + exePath + L"\" --device-lost-recovery \""
                               + autosavePath.wstring() + L"\"";

            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr, cmd.data(),
                               nullptr, nullptr, FALSE, 0,
                               nullptr, nullptr, &si, &pi)) {
                std::cerr << "[main] Device-lost recovery process spawned (PID "
                          << pi.dwProcessId << "): " << autosavePath << std::endl;
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            } else {
                std::cerr << "[main] CreateProcessW failed for device-lost recovery "
                          << "(error=" << GetLastError() << ")" << std::endl;
            }
        }
    }
#endif

    if (scriptFailed) {
        std::cerr << "Application exited with script failures." << std::endl;
        return EXIT_FAILURE;
    }

    std::cout << "Application exited normally." << std::endl;
    return EXIT_SUCCESS;
}
