#pragma once

#include <nlohmann/json_fwd.hpp>
#include <memory>
#include <string>

namespace entity {

// Forward declarations
class Engine;

// Thread affinity for command execution.
//   Editor — registry mutations, project I/O, UI state, and timeline
//            transport (Play/Pause/Seek/SectionGo). Running transport on the
//            editor thread keeps script-driven asserts ordered: Pause then
//            AssertPlaybackState("Paused") both execute on the editor thread
//            in sequence, so the assert always sees the post-Pause state.
//            Timeline transport writes are all atomic, so running them from
//            the editor thread is safe even though the show thread reads them.
//   Either — commands that are safe on either thread and have no ordering
//            dependency with respect to WaitFrames/assert commands. Currently
//            only ExitCommand.
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
