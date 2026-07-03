#pragma once

#include "Command.hpp"
#include "UndoableCommand.hpp"
#include <atomic>
#include <queue>
#include <deque>
#include <mutex>
#include <functional>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <nlohmann/json.hpp>

namespace entity {

/**
 * CommandDispatcher - Central hub for command execution.
 *
 * Responsibilities:
 * - Maintain thread-safe command queue
 * - Execute commands on main thread
 * - Provide factory registration for JSON deserialization
 * - Handle WaitFrames by pausing command processing
 * - Optional: Record executed commands for macro export
 *
 * Thread Safety (post-ADR-0014):
 * - enqueue() is thread-safe (can be called from any thread)
 * - processQueue() runs on the editor thread (Affinity::Editor) AND the
 *   show thread (Affinity::Show). Only the queue itself is locked; the
 *   undo/redo stacks, recording buffer, and script-results members are
 *   editor-thread-only. Therefore every UndoableCommand MUST declare
 *   Affinity::Editor, and undo()/redo()/clearHistory() are editor-only.
 */
class CommandDispatcher {
public:
    // Factory function type for creating commands from JSON
    using CommandFactory = std::function<CommandPtr(const nlohmann::json&)>;

    CommandDispatcher();
    ~CommandDispatcher();

    /**
     * Enqueue a command for execution.
     * Thread-safe - can be called from script threads, UI callbacks, etc.
     */
    void enqueue(CommandPtr command);

    /**
     * Enqueue a command by type name and JSON parameters.
     * @param typeName Command type (e.g., "ImportVideo")
     * @param params JSON object with command parameters
     * @return true if command was created and enqueued
     */
    bool enqueue(const std::string& typeName, const nlohmann::json& params = {});

    /**
     * Process all queued commands matching `affinity` (until WaitFrames or empty).
     * Editor thread passes Affinity::Editor; show thread passes Affinity::Show.
     * Either-affinity commands are consumed by whichever thread drains first.
     * @param engine Reference to engine for command execution
     * @param affinity Thread context of the caller
     * @return Number of commands executed
     */
    size_t processQueue(Engine& engine, Affinity affinity = Affinity::Editor);

    /**
     * Register a command factory for JSON deserialization.
     * Call once during initialization for each command type.
     */
    void registerFactory(const std::string& typeName, CommandFactory factory);

    /**
     * Register all built-in command factories.
     * Called automatically during construction.
     */
    void registerBuiltinFactories();

    /**
     * Create a command from JSON.
     * @return Command pointer, or nullptr if type unknown
     */
    CommandPtr createFromJson(const nlohmann::json& json);

    /**
     * Execute a script file (JSON array of commands).
     * @param filepath Path to script file
     * @return true if file was loaded and commands enqueued
     */
    bool loadScript(const std::string& filepath);

    /**
     * Enable/disable command recording for macro export.
     */
    void setRecording(bool enabled) { m_recording = enabled; }
    bool isRecording() const { return m_recording; }

    /**
     * Get recorded commands as JSON array.
     */
    nlohmann::json getRecordedCommands() const;

    /**
     * Clear recorded commands.
     */
    void clearRecording();

    /**
     * Get number of pending commands in queue.
     */
    size_t getPendingCount() const;

    /**
     * Check if currently waiting (WaitFrames or WaitUntil active).
     */
    bool isWaiting() const { return m_waitFramesRemaining > 0 || m_waitUntilActive; }

    /**
     * Get remaining wait frames.
     */
    uint32_t getWaitFramesRemaining() const { return m_waitFramesRemaining; }

    /**
     * Set wait frames (called by WaitFramesCommand).
     */
    void setWaitFrames(uint32_t frames) { m_waitFramesRemaining = frames; }

    /**
     * Pause script execution until the given time point passes.
     * processQueue() returns immediately each tick while the deadline hasn't
     * elapsed — the editor thread stays unblocked and the show thread runs
     * freely. Called by SleepMsCommand instead of a blocking sleep.
     */
    void setWaitUntil(std::chrono::steady_clock::time_point deadline) {
        m_waitUntil = deadline;
        m_waitUntilActive = true;
    }

    /**
     * Check if a script is currently running.
     */
    bool isScriptRunning() const { return m_scriptRunning; }

    /**
     * Get script execution results.
     */
    const nlohmann::json& getScriptResults() const { return m_scriptResults; }

    /**
     * True if any command has failed, thrown, or failed to parse since
     * results were last reset. Used by the integration test harness to
     * propagate failure to the process exit code.
     */
    bool hasErrors() const {
        auto it = m_scriptResults.find("errors");
        return it != m_scriptResults.end() && it->is_array() && !it->empty();
    }

    /**
     * True if a script is loaded, the queue has drained, and no
     * `WaitFrames` is in progress. The Engine main loop calls this after
     * the bus resolves any outstanding capture replies and only then
     * calls `finishScript()` -- so the written `script_result.json`
     * reflects the final state of all asynchronous work.
     */
    bool scriptReadyToFinish() const;

    /**
     * Mark script as complete (called when queue empties after script load).
     */
    void finishScript();

    /**
     * Add a screenshot path to results.
     */
    void addScreenshotToResults(const std::string& path);

    /**
     * Add an error to results.
     */
    void addErrorToResults(const std::string& error);

    /**
     * Undo the most recently executed undoable command.
     * Non-undoable commands (e.g. Seek, Save) are skipped by the stack —
     * they never land on it in the first place, so they don't block undo.
     * @return true if a command was undone.
     */
    bool undo(Engine& engine);

    /**
     * Redo the most recently undone command.
     * @return true if a command was redone.
     */
    bool redo(Engine& engine);

    size_t getUndoDepth() const { return m_undoStack.size(); }
    size_t getRedoDepth() const { return m_redoStack.size(); }

    /**
     * Clear undo + redo stacks. Called on project load so that you can't
     * undo past the project boundary into a state that doesn't match the
     * currently loaded entities.
     */
    void clearHistory();

private:
    std::queue<CommandPtr> m_commandQueue;
    mutable std::mutex m_queueMutex;

    std::unordered_map<std::string, CommandFactory> m_factories;

    // WaitFrames support
    uint32_t m_waitFramesRemaining{0};

    // WaitUntil support (wall-clock pause; doesn't block the editor thread)
    bool m_waitUntilActive{false};
    std::chrono::steady_clock::time_point m_waitUntil{};

    // Recording support
    bool m_recording{false};
    std::vector<nlohmann::json> m_recordedCommands;

    // Script execution tracking. m_scriptCommandsExecuted is atomic because
    // both processQueue() callers (editor + show thread) increment it.
    bool m_scriptRunning{false};
    size_t m_scriptCommandsTotal{0};
    std::atomic<size_t> m_scriptCommandsExecuted{0};
    nlohmann::json m_scriptResults;

    // Undo / redo stacks. Bounded to MAX_UNDO_DEPTH; overflow drops the
    // oldest entry. A new non-undo execute() clears the redo stack (standard
    // editor behaviour — branching timelines aren't worth the complexity).
    static constexpr size_t MAX_UNDO_DEPTH = 200;
    std::deque<std::unique_ptr<UndoableCommand>> m_undoStack;
    std::deque<std::unique_ptr<UndoableCommand>> m_redoStack;

    /**
     * Write script results to fixed path.
     */
    void writeScriptResults();
};

} // namespace entity
