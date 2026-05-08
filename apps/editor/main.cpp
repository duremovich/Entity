#include "entity/core/Engine.hpp"
#include "entity/core/Types.hpp"
#include "entity/core/EnginePluginContext.hpp"
#include "entity/command/CommandDispatcher.hpp"
#include "entity/plugin/Plugin.hpp"
#include <filesystem>
#include <iostream>
#include <cstdlib>
#include <string>
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
 *   --script <path>   Run a JSON script file after initialization
 *   --help            Show help message
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
#endif

    std::cout << "========================================" << std::endl;
    std::cout << "Entity Media Server - Editor" << std::endl;
    std::cout << "Version 0.1.0" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    // Parse command-line arguments
    std::string scriptPath;
    std::string projectPath;   // Positional <project.entity> for OS file-association open
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

    // Run script if specified
    if (!scriptPath.empty()) {
        std::cout << "Loading script: " << scriptPath << std::endl;
        if (!engine.runScript(scriptPath)) {
            std::cerr << "Failed to load script: " << scriptPath << std::endl;
            // Continue running anyway - don't exit on script failure
        }
    } else if (!projectPath.empty()) {
        // OS file-association / "open with" flow. Skip the launcher and
        // load the project directly. On failure (file missing, corrupt,
        // wrong version) fall back to the launcher so the user gets a
        // recoverable surface instead of a silently-empty editor.
        std::cout << "Opening project: " << projectPath << std::endl;
        if (!engine.loadProject(std::filesystem::path(projectPath))) {
            std::cerr << "Failed to open project: " << projectPath
                      << " — falling back to launcher." << std::endl;
            engine.showLauncher();
        }
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

    if (scriptFailed) {
        std::cerr << "Application exited with script failures." << std::endl;
        return EXIT_FAILURE;
    }

    std::cout << "Application exited normally." << std::endl;
    return EXIT_SUCCESS;
}
