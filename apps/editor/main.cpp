#include "entity/core/Engine.hpp"
#include "entity/core/Types.hpp"
#include <iostream>
#include <cstdlib>

/**
 * Entity Media Server - Editor Application
 *
 * Main entry point for the editor application.
 */

int main(int argc, char** argv) {
    std::cout << "========================================" << std::endl;
    std::cout << "Entity Media Server - Editor" << std::endl;
    std::cout << "Version 0.1.0" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    // Parse command-line arguments (future feature)
    // TODO: Add argument parsing for project files, etc.

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

    // Run main loop
    engine.run();

    // Shutdown (automatic via destructor, but explicit is clearer)
    engine.shutdown();

    std::cout << "Application exited normally." << std::endl;
    return EXIT_SUCCESS;
}
