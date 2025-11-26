#pragma once

#include <nlohmann/json_fwd.hpp>
#include <memory>
#include <string>

namespace entity {

// Forward declarations
class Engine;

/**
 * Command - Base interface for all scriptable commands.
 *
 * Commands encapsulate user actions that can be:
 * - Triggered by UI (keyboard, menus)
 * - Loaded from scripts (JSON files)
 * - Recorded for macro playback
 * - Queued for deterministic replay
 *
 * Design:
 * - Commands are immutable after construction
 * - execute() is called exactly once on main thread
 * - Commands store their own parameters
 * - All user actions flow through this interface (scriptable by default)
 */
class Command {
public:
    virtual ~Command() = default;

    /**
     * Execute the command.
     * Called on main thread with access to Engine and all subsystems.
     * @param engine Reference to the main engine
     * @return true if execution succeeded, false otherwise
     */
    virtual bool execute(Engine& engine) = 0;

    /**
     * Get command type name (for serialization and logging).
     * @return Static string literal, e.g., "ImportVideo"
     */
    virtual const char* getTypeName() const = 0;

    /**
     * Serialize command to JSON.
     * Used for macro recording and script generation.
     */
    virtual nlohmann::json toJson() const = 0;

    /**
     * Get human-readable description for logging.
     */
    virtual std::string getDescription() const {
        return getTypeName();
    }
};

// Convenience alias
using CommandPtr = std::unique_ptr<Command>;

} // namespace entity
