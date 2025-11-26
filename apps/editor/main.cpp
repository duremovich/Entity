#include "entity/core/Engine.hpp"
#include "entity/core/Types.hpp"
#include <iostream>
#include <cstdlib>
#include <string>
#include <vector>

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
    std::cout << "Usage: EntityMediaEditor [options]" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  --script <path>   Run a JSON script file after initialization" << std::endl;
    std::cout << "  --help            Show this help message" << std::endl;
    std::cout << std::endl;
    std::cout << "Example:" << std::endl;
    std::cout << "  EntityMediaEditor --script scripts/test.json" << std::endl;
}

int main(int argc, char** argv) {
    std::cout << "========================================" << std::endl;
    std::cout << "Entity Media Server - Editor" << std::endl;
    std::cout << "Version 0.1.0" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    // Parse command-line arguments
    std::string scriptPath;

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
        else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            std::cerr << "Use --help for usage information" << std::endl;
            return EXIT_FAILURE;
        }
    }

    // Create engine instance
    entity::Engine engine;

    // Initialize engine
    entity::Result result = engine.initialize(
        1920,  // Width
        1080,  // Height
        "Entity Media Server - Editor"
    );

    if (result != entity::Result::Success) {
        std::cerr << "Failed to initialize engine: "
                  << entity::ResultToString(result) << std::endl;
        return EXIT_FAILURE;
    }

    // Run script if specified
    if (!scriptPath.empty()) {
        std::cout << "Loading script: " << scriptPath << std::endl;
        if (!engine.runScript(scriptPath)) {
            std::cerr << "Failed to load script: " << scriptPath << std::endl;
            // Continue running anyway - don't exit on script failure
        }
    }

    // Run main loop
    engine.run();

    // Shutdown (automatic via destructor, but explicit is clearer)
    engine.shutdown();

    std::cout << "Application exited normally." << std::endl;
    return EXIT_SUCCESS;
}
