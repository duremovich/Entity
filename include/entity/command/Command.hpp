#pragma once

#include <nlohmann/json_fwd.hpp>
#include <memory>
#include <string>

namespace entity {

// Forward declarations
class Engine;

// Thread affinity for command execution.
//   Editor — registry mutations, project I/O, UI state, timeline transport,
//            and ALL assert/capture commands. Editor-affinity commands honour
//            WaitFrames and WaitSeconds ordering: the editor processQueue
//            checks the deadline before popping the next command, so
//            WaitSeconds(2.0) → AssertFoo is guaranteed to run after 2s.
//            Any assert that must execute after a preceding wait MUST be
//            Editor-affinity — Either bypasses the wait-state gate on the
//            show thread and can fire with 0 accumulated samples.
//   Either — commands that are safe on either thread AND have no ordering
//            dependency with respect to WaitFrames/assert commands. The show
//            thread drains Either commands without checking the wait deadline.
//            Use only for commands that are truly order-independent (ExitCommand,
//            pure-setter transport ops consumed by the show thread's own path).
//   Show   — (unused: reserved for future show-thread-only commands)
enum class Affinity { Editor, Show, Either };

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
 * - execute() is called on the thread matching getAffinity()
 * - Commands store their own parameters
 * - All user actions flow through this interface (scriptable by default)
 */
class Command {
public:
    virtual ~Command() = default;

    /**
     * Execute the command.
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

    /**
     * Thread affinity. processQueue filters by this so Show-affinity
     * commands execute on the show thread and Editor-affinity commands
     * execute on the editor thread.
     */
    virtual Affinity getAffinity() const { return Affinity::Editor; }
};

// Convenience alias
using CommandPtr = std::unique_ptr<Command>;

} // namespace entity
