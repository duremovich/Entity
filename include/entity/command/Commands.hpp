#pragma once

#include "Command.hpp"
#include "UndoableCommand.hpp"
#include "entity/core/Types.hpp"
#include "entity/components/Clip.hpp"   // PlaybackMode
#include "entity/components/AnimatedProperties.hpp"  // AnimatableProperty, InterpolationType
#include "entity/components/EffectParam.hpp"  // ParamValue
#include "entity/timeline/Timeline.hpp"  // Ripple{Insert,Delete}Result
#include "entity/timeline/CueTag.hpp"
#include <entt/entt.hpp>
#include <nlohmann/json.hpp>
#include <string>
#include <optional>
#include <array>
#include <vector>

namespace entity {

// ============================================================================
// Transport Commands
// ============================================================================

class PlayCommand : public Command {
public:
    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "Play"; }
    nlohmann::json toJson() const override { return {{"type", "Play"}}; }
    Affinity getAffinity() const override { return Affinity::Editor; }
    static CommandPtr fromJson(const nlohmann::json& j);
};

class PauseCommand : public Command {
public:
    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "Pause"; }
    nlohmann::json toJson() const override { return {{"type", "Pause"}}; }
    Affinity getAffinity() const override { return Affinity::Editor; }
    static CommandPtr fromJson(const nlohmann::json& j);
};

class TogglePlayPauseCommand : public Command {
public:
    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "TogglePlayPause"; }
    nlohmann::json toJson() const override { return {{"type", "TogglePlayPause"}}; }
    Affinity getAffinity() const override { return Affinity::Editor; }
    static CommandPtr fromJson(const nlohmann::json& j);
};

class SeekCommand : public Command {
public:
    explicit SeekCommand(Timecode time) : m_time(time) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "Seek"; }
    nlohmann::json toJson() const override {
        return {{"type", "Seek"}, {"time", m_time}};
    }
    std::string getDescription() const override;
    Affinity getAffinity() const override { return Affinity::Editor; }
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    Timecode m_time;
};

class SeekToFrameCommand : public Command {
public:
    explicit SeekToFrameCommand(FrameNumber frame) : m_frame(frame) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "SeekToFrame"; }
    nlohmann::json toJson() const override {
        return {{"type", "SeekToFrame"}, {"frame", m_frame}};
    }
    std::string getDescription() const override;
    Affinity getAffinity() const override { return Affinity::Editor; }
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    FrameNumber m_frame;
};

// ============================================================================
// Navigation Commands
// ============================================================================

class SeekToStartCommand : public Command {
public:
    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "SeekToStart"; }
    nlohmann::json toJson() const override { return {{"type", "SeekToStart"}}; }
    Affinity getAffinity() const override { return Affinity::Editor; }
    static CommandPtr fromJson(const nlohmann::json& j);
};

class SeekToEndCommand : public Command {
public:
    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "SeekToEnd"; }
    nlohmann::json toJson() const override { return {{"type", "SeekToEnd"}}; }
    Affinity getAffinity() const override { return Affinity::Editor; }
    static CommandPtr fromJson(const nlohmann::json& j);
};

class StepForwardCommand : public Command {
public:
    explicit StepForwardCommand(int32_t frames = 1) : m_frames(frames) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "StepForward"; }
    nlohmann::json toJson() const override {
        return {{"type", "StepForward"}, {"frames", m_frames}};
    }
    Affinity getAffinity() const override { return Affinity::Editor; }
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    int32_t m_frames;
};

class StepBackwardCommand : public Command {
public:
    explicit StepBackwardCommand(int32_t frames = 1) : m_frames(frames) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "StepBackward"; }
    nlohmann::json toJson() const override {
        return {{"type", "StepBackward"}, {"frames", m_frames}};
    }
    Affinity getAffinity() const override { return Affinity::Editor; }
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    int32_t m_frames;
};

// ============================================================================
// Clip Commands
// ============================================================================

class SelectClipCommand : public Command {
public:
    // Select by track and clip index (for scripts), optionally expand to show property tracks
    SelectClipCommand(int trackIndex, int clipIndex, bool expand = false)
        : m_trackIndex(trackIndex), m_clipIndex(clipIndex), m_expand(expand) {}

    // Select by entity ID (for internal use)
    explicit SelectClipCommand(uint32_t entityId, bool expand = false)
        : m_entityId(entityId), m_expand(expand) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "SelectClip"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    std::optional<int> m_trackIndex;
    std::optional<int> m_clipIndex;
    std::optional<uint32_t> m_entityId;
    bool m_expand{false};
};

class DeselectAllCommand : public Command {
public:
    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "DeselectAll"; }
    nlohmann::json toJson() const override { return {{"type", "DeselectAll"}}; }

    static CommandPtr fromJson(const nlohmann::json& j);
};

class SplitClipCommand : public Command {
public:
    // Split selected clip at playhead (default)
    SplitClipCommand() = default;

    // Split specific entity at specific frame
    SplitClipCommand(uint32_t entityId, FrameNumber frame)
        : m_entityId(entityId), m_frame(frame) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "SplitClip"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    std::optional<uint32_t> m_entityId;
    std::optional<FrameNumber> m_frame;
};

class DuplicateClipCommand : public Command {
public:
    // Duplicate selected clip (default)
    DuplicateClipCommand() = default;

    // Duplicate specific entity
    explicit DuplicateClipCommand(uint32_t entityId) : m_entityId(entityId) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "DuplicateClip"; }
    nlohmann::json toJson() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    std::optional<uint32_t> m_entityId;
};

class DeleteClipCommand : public UndoableCommand {
public:
    // Delete selected clip (default)
    DeleteClipCommand() = default;

    // Delete specific entity
    explicit DeleteClipCommand(uint32_t entityId) : m_entityId(entityId) {}

    bool execute(Engine& engine) override;
    bool undo(Engine& engine) override;
    bool redo(Engine& engine) override;
    const char* getTypeName() const override { return "DeleteClip"; }
    nlohmann::json toJson() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    std::optional<uint32_t> m_entityId;
    // Pre-delete snapshot, captured on first execute(). Empty until then;
    // undo() reads it to recreate the clip with a fresh entity ID + GPU
    // resources. redo() retargets m_entityId to the restored entity so the
    // re-execute deletes the right clip.
    bool                        m_captured{false};
    Timeline::DeletedClipSnapshot m_snapshot;
};

// ============================================================================
// Media Commands
// ============================================================================

class ImportVideoCommand : public Command {
public:
    explicit ImportVideoCommand(std::string filepath, int trackIndex = 0, Timecode position = 0)
        : m_filepath(std::move(filepath)), m_trackIndex(trackIndex), m_position(position) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "ImportVideo"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    std::string m_filepath;
    int m_trackIndex;
    Timecode m_position;
};

// ============================================================================
// Project Commands
// ============================================================================

class SaveProjectCommand : public Command {
public:
    explicit SaveProjectCommand(std::string filepath = "") : m_filepath(std::move(filepath)) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "SaveProject"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    std::string m_filepath;
};

class LoadProjectCommand : public Command {
public:
    explicit LoadProjectCommand(std::string filepath) : m_filepath(std::move(filepath)) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "LoadProject"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    std::string m_filepath;
};

// ============================================================================
// Timeline Structure Commands
// ============================================================================

class AddTrackCommand : public Command {
public:
    AddTrackCommand() = default;

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "AddTrack"; }
    nlohmann::json toJson() const override { return {{"type", "AddTrack"}}; }
    std::string getDescription() const override { return "Add track"; }
    Affinity getAffinity() const override { return Affinity::Editor; }
    static CommandPtr fromJson(const nlohmann::json&) {
        return std::make_unique<AddTrackCommand>();
    }
};

// ============================================================================
// Script Control Commands
// ============================================================================

class WaitFramesCommand : public Command {
public:
    explicit WaitFramesCommand(uint32_t count) : m_count(count) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "WaitFrames"; }
    nlohmann::json toJson() const override {
        return {{"type", "WaitFrames"}, {"count", m_count}};
    }
    std::string getDescription() const override;
    Affinity getAffinity() const override { return Affinity::Editor; }
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    uint32_t m_count;
};

class WaitSecondsCommand : public Command {
public:
    explicit WaitSecondsCommand(double count) : m_count(count) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "WaitSeconds"; }
    nlohmann::json toJson() const override {
        return {{"type", "WaitSeconds"}, {"count", m_count}};
    }
    std::string getDescription() const override;
    Affinity getAffinity() const override { return Affinity::Editor; }
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    double m_count;
};

class CaptureScreenshotCommand : public Command {
public:
    enum class Region {
        ComposeTarget,  // Video output only
        FullWindow      // Entire window with UI
    };

    explicit CaptureScreenshotCommand(std::string filepath, Region region = Region::ComposeTarget)
        : m_filepath(std::move(filepath)), m_region(region) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "CaptureScreenshot"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    std::string m_filepath;
    Region m_region;
};

// Capture a pixel-hash of a compose target. Used by integration tests for
// deterministic output comparison. Writes "<hex> <WxH>\n" to `hashFilepath`.
// If `goldenFilepath` is non-empty, fails on mismatch with the golden file.
class CaptureHashCommand : public Command {
public:
    CaptureHashCommand(std::string hashFilepath,
                       std::string goldenFilepath = "",
                       uint32_t composeSlot = 0)
        : m_hashFilepath(std::move(hashFilepath))
        , m_goldenFilepath(std::move(goldenFilepath))
        , m_composeSlot(composeSlot) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "CaptureHash"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    std::string m_hashFilepath;
    std::string m_goldenFilepath;
    uint32_t m_composeSlot;
};

// ============================================================================
// Application Commands
// ============================================================================

class ExitCommand : public Command {
public:
    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "Exit"; }
    nlohmann::json toJson() const override { return {{"type", "Exit"}}; }
    // Editor (not Either): otherwise the show thread eats Exit out of order
    // while skipping Editor-affinity commands (AddScreen, ImportVideo, ...)
    // queued ahead of it by a script, terminating the run early.
    Affinity getAffinity() const override { return Affinity::Editor; }
    static CommandPtr fromJson(const nlohmann::json& j);
};

// ============================================================================
// Property Commands (for debugging and scripting)
// ============================================================================

class SetClipBlendModeCommand : public UndoableCommand {
public:
    SetClipBlendModeCommand(int trackIndex, int clipIndex, BlendMode mode)
        : m_trackIndex(trackIndex), m_clipIndex(clipIndex), m_blendMode(mode) {}

    // UI sets this to the pre-edit value so undo restores the right state.
    // Scripts can skip it and execute() will auto-capture from live state.
    void setPreviousMode(BlendMode prev) { m_previousMode = prev; }

    bool execute(Engine& engine) override;
    bool undo(Engine& engine) override;
    const char* getTypeName() const override { return "SetClipBlendMode"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    int m_trackIndex;
    int m_clipIndex;
    BlendMode m_blendMode;
    std::optional<BlendMode> m_previousMode;
};

class SetClipOpacityCommand : public UndoableCommand {
public:
    SetClipOpacityCommand(int trackIndex, int clipIndex, float opacity)
        : m_trackIndex(trackIndex), m_clipIndex(clipIndex), m_opacity(opacity) {}

    void setPreviousOpacity(float prev) { m_previousOpacity = prev; }

    bool execute(Engine& engine) override;
    bool undo(Engine& engine) override;
    const char* getTypeName() const override { return "SetClipOpacity"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    int m_trackIndex;
    int m_clipIndex;
    float m_opacity;
    std::optional<float> m_previousOpacity;
};

class LogClipStateCommand : public Command {
public:
    // Log all clips state
    LogClipStateCommand() = default;

    // Log specific clip
    LogClipStateCommand(int trackIndex, int clipIndex)
        : m_trackIndex(trackIndex), m_clipIndex(clipIndex) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "LogClipState"; }
    nlohmann::json toJson() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    std::optional<int> m_trackIndex;
    std::optional<int> m_clipIndex;
};

// ============================================================================
// UI Commands
// ============================================================================

class SelectTabCommand : public Command {
public:
    SelectTabCommand(const std::string& tabName) : m_tabName(tabName) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "SelectTab"; }
    nlohmann::json toJson() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    std::string m_tabName;  // "Stage" or "Mapping"
};

class SetClipRotationCommand : public UndoableCommand {
public:
    SetClipRotationCommand(int trackIndex, int clipIndex, float rotX, float rotY, float rotZ)
        : m_trackIndex(trackIndex), m_clipIndex(clipIndex),
          m_rotX(rotX), m_rotY(rotY), m_rotZ(rotZ) {}

    void setPreviousRotation(float prevX, float prevY, float prevZ) {
        m_previousRotation = std::array<float, 3>{prevX, prevY, prevZ};
    }

    bool execute(Engine& engine) override;
    bool undo(Engine& engine) override;
    const char* getTypeName() const override { return "SetClipRotation"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    int m_trackIndex;
    int m_clipIndex;
    float m_rotX, m_rotY, m_rotZ;
    std::optional<std::array<float, 3>> m_previousRotation;
};

// ============================================================================
// Keyframe Animation Commands
// ============================================================================

/**
 * Add a keyframe to a clip's animated property.
 *
 * JSON format:
 * {
 *     "type": "AddKeyframe",
 *     "trackIndex": 0,
 *     "clipIndex": 0,
 *     "property": "PositionX",  // PositionX, PositionY, Rotation, ScaleX, ScaleY, Opacity
 *     "frame": 30,              // Frame relative to clip start
 *     "value": 100.0,
 *     "interpolation": "Linear" // Linear, Step, EaseIn, EaseOut, EaseInOut (optional, default: Linear)
 * }
 */
class AddKeyframeCommand : public Command {
public:
    AddKeyframeCommand(int trackIndex, int clipIndex, const std::string& property,
                       FrameNumber frame, float value, const std::string& interpolation = "Linear")
        : m_trackIndex(trackIndex), m_clipIndex(clipIndex), m_property(property),
          m_frame(frame), m_value(value), m_interpolation(interpolation) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "AddKeyframe"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    int m_trackIndex;
    int m_clipIndex;
    std::string m_property;
    FrameNumber m_frame;
    float m_value;
    std::string m_interpolation;
};

/**
 * Add-or-update a single keyframe on an animated-property track. Used by
 * the PropertyWindow sliders when they detect the property is already
 * keyframed — dragging the slider rewrites the keyframe at the current
 * frame (After Effects-style), and this command lets that action be
 * undone. Pre-edit state is either "keyframe at this frame had value V"
 * or "no keyframe existed at this frame" (undo removes the new one).
 *
 * Unlike AddKeyframeCommand, this one is undoable and round-trips both
 * existing-keyframe overwrite and new-keyframe insert. Not currently
 * exposed to JSON scripts — emitted only from UI paths.
 */
class UpsertKeyframeCommand : public UndoableCommand {
public:
    UpsertKeyframeCommand(int trackIndex, int clipIndex,
                          AnimatableProperty property, FrameNumber frame,
                          float newValue,
                          InterpolationType interp = InterpolationType::Linear)
        : m_trackIndex(trackIndex), m_clipIndex(clipIndex),
          m_property(property), m_frame(frame),
          m_newValue(newValue), m_interp(interp) {}

    // Pre-edit state. setPreviousValue(nullopt) means no keyframe existed
    // at m_frame before the drag — undo then removes the new one.
    void setPreviousValue(std::optional<float> prev) {
        m_previousValue = prev;
        m_hasPreviousState = true;
    }

    bool execute(Engine& engine) override;
    bool undo(Engine& engine) override;
    const char* getTypeName() const override { return "UpsertKeyframe"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    int m_trackIndex;
    int m_clipIndex;
    AnimatableProperty m_property;
    FrameNumber m_frame;
    float m_newValue;
    InterpolationType m_interp;
    bool m_hasPreviousState{false};
    std::optional<float> m_previousValue;  // nullopt = keyframe didn't exist
};

/**
 * Clear all keyframes from a clip.
 *
 * JSON format:
 * {
 *     "type": "ClearKeyframes",
 *     "trackIndex": 0,
 *     "clipIndex": 0
 * }
 */
class ClearKeyframesCommand : public Command {
public:
    ClearKeyframesCommand(int trackIndex, int clipIndex)
        : m_trackIndex(trackIndex), m_clipIndex(clipIndex) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "ClearKeyframes"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    int m_trackIndex;
    int m_clipIndex;
};

// ============================================================================
// Screen Commands
// ============================================================================

/**
 * Add a new screen to the project.
 *
 * JSON format:
 * {
 *     "type": "AddScreen",
 *     "name": "New Screen",
 *     "width": 1920,
 *     "height": 1080
 * }
 */
class AddScreenCommand : public Command {
public:
    AddScreenCommand(const std::string& name, uint32_t width = 1920, uint32_t height = 1080)
        : m_name(name), m_width(width), m_height(height) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "AddScreen"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    std::string m_name;
    uint32_t m_width;
    uint32_t m_height;
};

/**
 * Set a clip's target screen.
 *
 * JSON format:
 * {
 *     "type": "SetClipTargetScreen",
 *     "trackIndex": 0,
 *     "clipIndex": 0,
 *     "screenName": "Main Screen"  // Use "All Screens" for null target
 * }
 */
class SetClipTargetScreenCommand : public UndoableCommand {
public:
    SetClipTargetScreenCommand(int trackIndex, int clipIndex, const std::string& screenName)
        : m_trackIndex(trackIndex), m_clipIndex(clipIndex), m_screenName(screenName) {}

    // Screens are identified by name, not entity — matches JSON schema and
    // survives project reloads where entt::entity values aren't stable.
    // Pass "All Screens" (or empty) for the null-target case.
    void setPreviousScreenName(std::string prev) { m_previousScreenName = std::move(prev); }

    bool execute(Engine& engine) override;
    bool undo(Engine& engine) override;
    const char* getTypeName() const override { return "SetClipTargetScreen"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    int m_trackIndex;
    int m_clipIndex;
    std::string m_screenName;
    std::optional<std::string> m_previousScreenName;
};

/**
 * Assert that a Screen with the given name exists in the registry.
 * Fails the script (non-zero exit) if absent. Used by integration tests
 * to verify project-load preserved screens by name.
 *
 * JSON format:
 * {
 *     "type": "AssertScreenExists",
 *     "name": "Main Screen"
 * }
 */
class AssertScreenExistsCommand : public Command {
public:
    explicit AssertScreenExistsCommand(const std::string& name) : m_name(name) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "AssertScreenExists"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    std::string m_name;
};

/**
 * Assert that the registry contains exactly N Screen entities.
 *
 * JSON format:
 * {
 *     "type": "AssertScreenCount",
 *     "count": 2
 * }
 */
class AssertScreenCountCommand : public Command {
public:
    explicit AssertScreenCountCommand(size_t count) : m_count(count) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "AssertScreenCount"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    size_t m_count;
};

/**
 * Assert that the engine-global FrameCache currently holds an exact entry
 * for (clipEntity at trackIndex/clipIndex, sourceFrame). This is the gate
 * for the click-to-recently-viewed-frame fast path — Phase C.10's promise
 * is that re-seeking to a recently-decoded source frame is a cache hit, not
 * a re-decode.
 *
 * Usage in tests: warm the cache by seeking to N, seek elsewhere, seek back
 * to N, then AssertFrameCached the source frame N maps to. Failure here means
 * either the cache evicted under-budget (regression in eviction policy), or
 * the producer/consumer wiring is broken (frames aren't landing in the cache).
 *
 * JSON:
 * {
 *     "type": "AssertFrameCached",
 *     "trackIndex": 0,
 *     "clipIndex": 0,
 *     "sourceFrame": 8
 * }
 */
class AssertFrameCachedCommand : public Command {
public:
    AssertFrameCachedCommand(int trackIndex, int clipIndex, FrameNumber sourceFrame)
        : m_trackIndex(trackIndex), m_clipIndex(clipIndex), m_sourceFrame(sourceFrame) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "AssertFrameCached"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    int         m_trackIndex;
    int         m_clipIndex;
    FrameNumber m_sourceFrame;
};

/**
 * Live-set the engine-global FrameCache budget. Used by the budget-stress
 * test to shrink the cache below a clip's working set, forcing the LRU
 * eviction path to do real work.
 *
 * JSON:
 * {
 *     "type": "SetFrameCacheBudget",
 *     "bytes": 81920
 * }
 */
class SetFrameCacheBudgetCommand : public Command {
public:
    explicit SetFrameCacheBudgetCommand(uint64_t bytes) : m_bytes(bytes) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "SetFrameCacheBudget"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    uint64_t m_bytes;
};

/**
 * Assert the cache invariant `bytesUsed <= maxBytes && entryCount > 0`.
 *
 * The `entryCount > 0` check guards against the trivial-pass: a cache with
 * zero entries trivially satisfies the budget. We want to prove the cache
 * is *alive* (decoder is filling it) AND staying within budget.
 *
 * JSON:
 * {
 *     "type": "AssertFrameCacheBudgetOK"
 * }
 */
class AssertFrameCacheBudgetOKCommand : public Command {
public:
    AssertFrameCacheBudgetOKCommand() = default;

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "AssertFrameCacheBudgetOK"; }
    nlohmann::json toJson() const override { return {{"type", "AssertFrameCacheBudgetOK"}}; }
    std::string getDescription() const override { return "Assert FrameCache stays within budget"; }

    static CommandPtr fromJson(const nlohmann::json&) {
        return std::make_unique<AssertFrameCacheBudgetOKCommand>();
    }
};

// ============================================================================
// Playback-mode / framerate / duration overrides (for integration tests)
// ============================================================================

/**
 * Override a clip's PlaybackMode.
 *
 * JSON format:
 * {
 *     "type": "SetClipPlaybackMode",
 *     "trackIndex": 0,
 *     "clipIndex": 0,
 *     "mode": "PingPong"   // Freeze | Loop | PingPong
 * }
 */
class SetClipPlaybackModeCommand : public UndoableCommand {
public:
    SetClipPlaybackModeCommand(int trackIndex, int clipIndex, PlaybackMode mode)
        : m_trackIndex(trackIndex), m_clipIndex(clipIndex), m_mode(mode) {}

    void setPreviousMode(PlaybackMode prev) { m_previousMode = prev; }

    bool execute(Engine& engine) override;
    bool undo(Engine& engine) override;
    const char* getTypeName() const override { return "SetClipPlaybackMode"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    int m_trackIndex;
    int m_clipIndex;
    PlaybackMode m_mode;
    std::optional<PlaybackMode> m_previousMode;
};

/**
 * Override a clip's source framerate. Also recomputes `clip.duration` from
 * `totalMediaFrames * (timelineFrameRate / clip.framerate)` so the clip's
 * natural length on the timeline matches the new rate — mirroring the behavior
 * of ProjectSerializer::load and Engine import paths. Use to drive mixed-fps
 * tests that exercise the sourceFrame = localFrame * (srcFps / tlFps) mapping
 * in DecodeSystem / PlaybackTimeAuthority.
 *
 * JSON format:
 * {
 *     "type": "SetClipFramerate",
 *     "trackIndex": 0,
 *     "clipIndex": 0,
 *     "framerate": 24.0
 * }
 */
class SetClipFramerateCommand : public UndoableCommand {
public:
    SetClipFramerateCommand(int trackIndex, int clipIndex, double framerate)
        : m_trackIndex(trackIndex), m_clipIndex(clipIndex), m_framerate(framerate) {}

    // Framerate changes recompute clip duration — both need to restore.
    void setPreviousState(double prevFramerate, FrameNumber prevDuration) {
        m_previousFramerate = prevFramerate;
        m_previousDuration = prevDuration;
    }

    bool execute(Engine& engine) override;
    bool undo(Engine& engine) override;
    const char* getTypeName() const override { return "SetClipFramerate"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    int m_trackIndex;
    int m_clipIndex;
    double m_framerate;
    std::optional<double> m_previousFramerate;
    std::optional<FrameNumber> m_previousDuration;
};

/**
 * Override a clip's duration in timeline frames. Mirrors what UI trim does.
 * Required for ping-pong tests because the Freeze/Loop/PingPong branch in
 * DecodeSystem only triggers when sourceLocalFrame >= sourceLength, which
 * requires duration > natural length.
 *
 * JSON format:
 * {
 *     "type": "SetClipDuration",
 *     "trackIndex": 0,
 *     "clipIndex": 0,
 *     "duration": 64
 * }
 */
// ============================================================================
// Section break-point commands (Phase B refactor of Phase C #5).
// Sections live on the timeline as point markers; old `{start, end}` regional
// `AddSection` is kept as a deprecated alias that emits two break points for
// backwards compatibility with v8 project files / legacy scripts.
// ============================================================================

/**
 * Add a section break at the given timeline frame. Undoable.
 *
 * JSON: {"type":"AddSectionBreak","breakFrame":<usec>,
 *        "color":4286074559,"fadeSeconds":0.0}
 */
class AddSectionBreakCommand : public UndoableCommand {
public:
    AddSectionBreakCommand(Timecode breakFrame,
                           uint32_t color = 0xFF6090C8, double fadeSeconds = 0.0)
        : m_breakFrame(breakFrame),
          m_color(color), m_fadeSeconds(fadeSeconds) {}

    bool execute(Engine& engine) override;
    bool undo(Engine& engine) override;
    bool redo(Engine& engine) override;
    const char* getTypeName() const override { return "AddSectionBreak"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    Timecode    m_breakFrame;
    uint32_t    m_color;
    double      m_fadeSeconds;
    bool        m_inserted{false};
};

/**
 * Remove a section break at the given timeline frame. Captures pre-state
 * for undo. JSON: {"type":"RemoveSectionBreak","breakFrame":<usec>}
 */
class RemoveSectionBreakCommand : public UndoableCommand {
public:
    explicit RemoveSectionBreakCommand(Timecode breakFrame) : m_breakFrame(breakFrame) {}

    bool execute(Engine& engine) override;
    bool undo(Engine& engine) override;
    bool redo(Engine& engine) override;
    const char* getTypeName() const override { return "RemoveSectionBreak"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    Timecode m_breakFrame;
    bool     m_captured{false};
    Timeline::Section m_previousState;
};

/**
 * Edit an existing section break. UI sets `setPreviousState` to the pre-edit
 * snapshot so undo restores the original even when `breakFrame` changed.
 *
 * JSON: {"type":"EditSectionBreak","oldBreakFrame":...,"newBreakFrame":...,
 *        "newColor":...,"newFadeSeconds":0.0}
 */
class EditSectionBreakCommand : public UndoableCommand {
public:
    EditSectionBreakCommand(Timecode oldBreakFrame, Timecode newBreakFrame,
                            uint32_t newColor, double newFadeSeconds)
        : m_oldBreakFrame(oldBreakFrame), m_newBreakFrame(newBreakFrame),
          m_newColor(newColor), m_newFadeSeconds(newFadeSeconds) {}

    void setPreviousState(Timeline::Section prev) {
        m_previousState = std::move(prev);
        m_hasPreviousState = true;
    }

    bool execute(Engine& engine) override;
    bool undo(Engine& engine) override;
    bool redo(Engine& engine) override;
    const char* getTypeName() const override { return "EditSectionBreak"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    Timecode    m_oldBreakFrame;
    Timecode    m_newBreakFrame;
    uint32_t    m_newColor;
    double      m_newFadeSeconds;
    bool        m_hasPreviousState{false};
    Timeline::Section m_previousState;
};

/**
 * Spacebar GO when the timeline is at a section break. Not undoable —
 * playback transport changes stay outside the undo stack (matches the
 * Play/Pause/Seek pattern).
 *
 * JSON: {"type":"SectionGo"}
 */
class SectionGoCommand : public Command {
public:
    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "SectionGo"; }
    nlohmann::json toJson() const override { return {{"type", "SectionGo"}}; }
    std::string getDescription() const override { return "Section GO"; }
    Affinity getAffinity() const override { return Affinity::Editor; }
    static CommandPtr fromJson(const nlohmann::json&) {
        return std::make_unique<SectionGoCommand>();
    }
};

/**
 * Set a clip's section behavior policy (Normal | Locked). Phase B serializes
 * the value but treats every clip as Locked at runtime; Phase C activates
 * the Normal continuation path.
 *
 * JSON: {"type":"SetClipSectionBehavior","trackIndex":0,"clipIndex":0,
 *        "behavior":"Locked"}
 */
class SetClipSectionBehaviorCommand : public UndoableCommand {
public:
    SetClipSectionBehaviorCommand(int trackIndex, int clipIndex, SectionBehavior behavior)
        : m_trackIndex(trackIndex), m_clipIndex(clipIndex), m_behavior(behavior) {}

    void setPreviousBehavior(SectionBehavior prev) { m_previousBehavior = prev; }

    bool execute(Engine& engine) override;
    bool undo(Engine& engine) override;
    const char* getTypeName() const override { return "SetClipSectionBehavior"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    int m_trackIndex;
    int m_clipIndex;
    SectionBehavior m_behavior;
    std::optional<SectionBehavior> m_previousBehavior;
};

/**
 * Deprecated: legacy region-style section command. Phase B keeps it as an
 * alias that emits TWO break points (start + end) so v8 scripts and project
 * files keep working without rewriting. Prefer AddSectionBreak for new code.
 *
 * JSON: {"type":"AddSection","name":"Verse 1","start":1000000,"end":5000000,"color":...}
 */
class AddSectionCommand : public Command {
public:
    AddSectionCommand(std::string name, Timecode start, Timecode end, uint32_t color = 0xFF6090C8)
        : m_name(std::move(name)), m_start(start), m_end(end), m_color(color) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "AddSection"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    std::string m_name;
    Timecode m_start;
    Timecode m_end;
    uint32_t m_color;
};

class AssertSectionCountCommand : public Command {
public:
    explicit AssertSectionCountCommand(size_t count) : m_count(count) {}
    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "AssertSectionCount"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    static CommandPtr fromJson(const nlohmann::json& j);
private:
    size_t m_count;
};

/**
 * Assert the timeline's playhead currently sits at the given frame number.
 * Used by section-pause integration tests to verify the SectionScheduler
 * snapped the playhead at the expected break.
 *
 * JSON: {"type":"AssertPlayheadAtFrame","frame":60}
 */
class AssertPlayheadAtFrameCommand : public Command {
public:
    AssertPlayheadAtFrameCommand(FrameNumber frame, FrameNumber tolerance = 0)
        : m_frame(frame), m_tolerance(tolerance) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "AssertPlayheadAtFrame"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    FrameNumber m_frame;
    FrameNumber m_tolerance;
};

/**
 * Assert the timeline is currently in the requested playback state.
 * State strings: "Stopped" | "Playing" | "Paused".
 *
 * JSON: {"type":"AssertPlaybackState","state":"Paused"}
 */
class AssertPlaybackStateCommand : public Command {
public:
    explicit AssertPlaybackStateCommand(PlaybackState state) : m_state(state) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "AssertPlaybackState"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    PlaybackState m_state;
};

/**
 * Assert the clip at (trackIndex, clipIndex) currently maps to `expected`
 * source-media frame. Used by Phase C continuation tests to verify that
 * Loop / PingPong clips keep cycling while the playhead is parked at a
 * section break, and that Locked clips freeze. Calls
 * PlaybackTimeAuthority::mapToMediaFrame with the entity handle so the
 * ClipPlaybackPhase override fires when set.
 *
 * The `mode` controls how the result is compared:
 *   - "equal"    (default): pass when |actual - expected| <= tolerance.
 *   - "notEqual"          : pass when |actual - expected| >  tolerance
 *                           (used to verify continuation drift away from
 *                           a known seed value).
 *
 * JSON: {"type":"AssertClipMediaFrame","trackIndex":0,"clipIndex":0,
 *        "expected":12,"tolerance":1,"mode":"equal"}
 */
class AssertClipMediaFrameCommand : public Command {
public:
    enum class Mode { Equal, NotEqual };

    AssertClipMediaFrameCommand(int trackIndex, int clipIndex,
                                FrameNumber expected,
                                FrameNumber tolerance = 0,
                                Mode mode = Mode::Equal)
        : m_trackIndex(trackIndex)
        , m_clipIndex(clipIndex)
        , m_expected(expected)
        , m_tolerance(tolerance)
        , m_mode(mode) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "AssertClipMediaFrame"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    int         m_trackIndex;
    int         m_clipIndex;
    FrameNumber m_expected;
    FrameNumber m_tolerance;
    Mode        m_mode;
};

/**
 * Assert the clip at (trackIndex, clipIndex) currently has a section-fade
 * envelope multiplier matching `expected` within `tolerance`. Reads
 * `PlaybackTimeAuthority::computeSectionFadeMultiplier(clip)` against the
 * timeline's current frame, so the value reflects the same envelope the
 * Director stamps on the bus payload each tick.
 *
 * JSON: {"type":"AssertClipFadeMultiplier","trackIndex":0,"clipIndex":0,
 *        "expected":0.5,"tolerance":0.05}
 */
class AssertClipFadeMultiplierCommand : public Command {
public:
    AssertClipFadeMultiplierCommand(int trackIndex, int clipIndex,
                                    float expected,
                                    float tolerance = 0.01f)
        : m_trackIndex(trackIndex)
        , m_clipIndex(clipIndex)
        , m_expected(expected)
        , m_tolerance(tolerance) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "AssertClipFadeMultiplier"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    int   m_trackIndex;
    int   m_clipIndex;
    float m_expected;
    float m_tolerance;
};

// ============================================================================
// Cue Tag Commands (Phase A — numbered timeline markers)
// ============================================================================

/**
 * Fire a cue: seek the timeline to the cue's timestamp and start playback,
 * regardless of current play state. Logs a warning and fails when no cue
 * with the given number exists. Not undoable — playback transport changes
 * stay outside the undo stack (matches Seek/Play).
 *
 * JSON: {"type":"FireCue","number":1.5}
 */
class FireCueCommand : public Command {
public:
    explicit FireCueCommand(double cueNumber) : m_number(cueNumber) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "FireCue"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    double m_number;
};

/**
 * Add a cue tag at the given timestamp. Fails if a cue with the same
 * number already exists. Undo removes the cue.
 *
 * JSON: {"type":"AddCueAt","number":1.5,"timestamp":1000000,"label":""}
 */
class AddCueAtCommand : public UndoableCommand {
public:
    AddCueAtCommand(double number, Timecode timestamp, std::string label)
        : m_number(number), m_timestamp(timestamp), m_label(std::move(label)) {}

    bool execute(Engine& engine) override;
    bool undo(Engine& engine) override;
    bool redo(Engine& engine) override;
    const char* getTypeName() const override { return "AddCueAt"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    double      m_number;
    Timecode    m_timestamp;
    std::string m_label;
    bool        m_inserted{false};
};

/**
 * Remove the cue with the given number. Captures the full pre-state so
 * undo can re-insert with the original timestamp + label.
 *
 * JSON: {"type":"RemoveCue","number":1.5}
 */
class RemoveCueCommand : public UndoableCommand {
public:
    explicit RemoveCueCommand(double number) : m_number(number) {}

    bool execute(Engine& engine) override;
    bool undo(Engine& engine) override;
    bool redo(Engine& engine) override;
    const char* getTypeName() const override { return "RemoveCue"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    double  m_number;
    bool    m_captured{false};
    CueTag  m_previousState;
};

/**
 * Edit an existing cue's number/timestamp/label. UI sets `setPreviousState`
 * to the pre-edit snapshot so undo restores exactly that; scripts skip it
 * and execute() auto-captures from live state on first run.
 *
 * Fails if `newNumber` collides with another cue (unless == oldNumber).
 *
 * JSON: {"type":"EditCue","oldNumber":1.0,"newNumber":1.5,
 *        "newTimestamp":2000000,"newLabel":"Verse 1"}
 */
class EditCueCommand : public UndoableCommand {
public:
    EditCueCommand(double oldNumber, double newNumber,
                   Timecode newTimestamp, std::string newLabel)
        : m_oldNumber(oldNumber), m_newNumber(newNumber),
          m_newTimestamp(newTimestamp), m_newLabel(std::move(newLabel)) {}

    void setPreviousState(CueTag prev) {
        m_previousState = std::move(prev);
        m_hasPreviousState = true;
    }

    bool execute(Engine& engine) override;
    bool undo(Engine& engine) override;
    bool redo(Engine& engine) override;
    const char* getTypeName() const override { return "EditCue"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    double      m_oldNumber;
    double      m_newNumber;
    Timecode    m_newTimestamp;
    std::string m_newLabel;
    bool        m_hasPreviousState{false};
    CueTag      m_previousState;
};

/**
 * Assert the timeline has exactly N cue tags. Used by integration tests.
 *
 * JSON: {"type":"AssertCueCount","count":3}
 */
class AssertCueCountCommand : public Command {
public:
    explicit AssertCueCountCommand(size_t count) : m_count(count) {}
    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "AssertCueCount"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    static CommandPtr fromJson(const nlohmann::json& j);
private:
    size_t m_count;
};

/**
 * Assert that a cue with the given number exists.
 *
 * JSON: {"type":"AssertCueExists","number":1.5}
 */
class AssertCueExistsCommand : public Command {
public:
    explicit AssertCueExistsCommand(double number) : m_number(number) {}
    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "AssertCueExists"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    static CommandPtr fromJson(const nlohmann::json& j);
private:
    double m_number;
};

// ============================================================================
// Ripple time edits (Phase C #4) — wrap Timeline::ripple{Insert,Delete}Time
// and own the captured undo records.
// ============================================================================

class RippleInsertTimeCommand : public UndoableCommand {
public:
    RippleInsertTimeCommand(FrameNumber insertFrame, FrameNumber durationFrames)
        : m_insertFrame(insertFrame), m_duration(durationFrames) {}

    bool execute(Engine& engine) override;
    bool undo(Engine& engine) override;
    bool redo(Engine& engine) override;
    const char* getTypeName() const override { return "RippleInsertTime"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    FrameNumber m_insertFrame;
    FrameNumber m_duration;
    Timeline::RippleInsertResult m_record;
};

class RippleDeleteTimeCommand : public UndoableCommand {
public:
    RippleDeleteTimeCommand(FrameNumber rangeStart, FrameNumber rangeEnd)
        : m_rangeStart(rangeStart), m_rangeEnd(rangeEnd) {}

    bool execute(Engine& engine) override;
    bool undo(Engine& engine) override;
    bool redo(Engine& engine) override;
    const char* getTypeName() const override { return "RippleDeleteTime"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    FrameNumber m_rangeStart;
    FrameNumber m_rangeEnd;
    Timeline::RippleDeleteResult m_record;
};

class SetClipDurationCommand : public UndoableCommand {
public:
    SetClipDurationCommand(int trackIndex, int clipIndex, FrameNumber duration)
        : m_trackIndex(trackIndex), m_clipIndex(clipIndex), m_duration(duration) {}

    void setPreviousDuration(FrameNumber prev) { m_previousDuration = prev; }

    bool execute(Engine& engine) override;
    bool undo(Engine& engine) override;
    const char* getTypeName() const override { return "SetClipDuration"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    int m_trackIndex;
    int m_clipIndex;
    FrameNumber m_duration;
    std::optional<FrameNumber> m_previousDuration;
};

/**
 * Set a clip's mediaStartFrame (in-point) — slip edit. Leaves duration
 * untouched, so the timeline footprint stays the same and the source
 * window slides. Clamps to [0, totalMediaFrames - 1].
 *
 * JSON shape:
 *   { "type": "SetClipMediaStartFrame",
 *     "trackIndex": 0,
 *     "clipIndex": 0,
 *     "mediaStartFrame": 12 }
 */
class SetClipMediaStartFrameCommand : public UndoableCommand {
public:
    SetClipMediaStartFrameCommand(int trackIndex, int clipIndex, FrameNumber mediaStartFrame)
        : m_trackIndex(trackIndex), m_clipIndex(clipIndex), m_mediaStartFrame(mediaStartFrame) {}

    void setPreviousMediaStartFrame(FrameNumber prev) { m_previousMediaStartFrame = prev; }

    bool execute(Engine& engine) override;
    bool undo(Engine& engine) override;
    const char* getTypeName() const override { return "SetClipMediaStartFrame"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    int m_trackIndex;
    int m_clipIndex;
    FrameNumber m_mediaStartFrame;
    std::optional<FrameNumber> m_previousMediaStartFrame;
};

/**
 * Set a clip's mediaOutFrame (out-point) — controls where source playback
 * ends. INCLUSIVE: the last frame played (industry convention). Leaves
 * duration (timeline footprint) untouched: the clip keeps its timeline
 * length, but the playback window shrinks/grows. Past the out-point, the
 * clip's playbackMode (Freeze / Loop / PingPong) decides what plays.
 * Clamps to [mediaStartFrame, totalMediaFrames - 1].
 *
 * JSON shape:
 *   { "type": "SetClipMediaOutFrame",
 *     "trackIndex": 0,
 *     "clipIndex": 0,
 *     "mediaOutFrame": 30 }
 */
class SetClipMediaOutFrameCommand : public UndoableCommand {
public:
    SetClipMediaOutFrameCommand(int trackIndex, int clipIndex, FrameNumber mediaOutFrame)
        : m_trackIndex(trackIndex), m_clipIndex(clipIndex), m_mediaOutFrame(mediaOutFrame) {}

    void setPreviousMediaOutFrame(FrameNumber prev) { m_previousMediaOutFrame = prev; }

    bool execute(Engine& engine) override;
    bool undo(Engine& engine) override;
    const char* getTypeName() const override { return "SetClipMediaOutFrame"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    int m_trackIndex;
    int m_clipIndex;
    FrameNumber m_mediaOutFrame;
    std::optional<FrameNumber> m_previousMediaOutFrame;
};

/**
 * Sleep the calling (editor) thread for `ms` milliseconds using a wall-clock
 * sleep. Simulates an editor modal-loop stall so that integration tests can
 * verify the show thread keeps advancing independently (Stage 3c gate).
 *
 * JSON: {"type":"SleepMs","ms":500}
 */
class SleepMsCommand : public Command {
public:
    explicit SleepMsCommand(uint32_t ms) : m_ms(ms) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "SleepMs"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    Affinity getAffinity() const override { return Affinity::Editor; }
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    uint32_t m_ms;
};

/**
 * Assert that the show thread has completed at least `minCount` ticks since
 * the engine started. Used by integration tests to verify the show thread
 * keeps advancing even when the editor thread is stalled (Stage 3c gate).
 *
 * JSON: {"type":"AssertShowFrameCountAtLeast","minCount":10}
 */
class AssertShowFrameCountAtLeastCommand : public Command {
public:
    explicit AssertShowFrameCountAtLeastCommand(uint64_t minCount) : m_minCount(minCount) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "AssertShowFrameCountAtLeast"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    Affinity getAffinity() const override { return Affinity::Editor; }
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    uint64_t m_minCount;
};

/**
 * Create a synthetic Model entity with a default screen-quad mesh (no OBJ file).
 * **Test-only** — intended exclusively for headless integration scripts; no UI
 * command surface exposes it.
 *
 * Shares the pre-existing show-thread registry-read race documented at
 * OutputManager.cpp:610-612 (same race-surface as AddScreen / createDefaultScreen).
 * The fix is the OutputManager Stage 4 mesh-store refactor; tracked separately.
 *
 * JSON: {"type":"AddSyntheticModel","name":"TestModel"}
 */
class AddSyntheticModelCommand : public Command {
public:
    explicit AddSyntheticModelCommand(std::string name) : m_name(std::move(name)) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "AddSyntheticModel"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    Affinity getAffinity() const override { return Affinity::Editor; }
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    std::string m_name;
};

/**
 * Assert that the total mesh upload count equals exactly `expected`.
 * Used by mesh_upload_paces.json to verify the cold-start gate (0 uploads
 * before the show thread presents) and the one-per-tick steady-state cap.
 *
 * JSON: {"type":"AssertMeshUploadCount","expected":0}
 */
class AssertMeshUploadCountCommand : public Command {
public:
    explicit AssertMeshUploadCountCommand(uint64_t expected) : m_expected(expected) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "AssertMeshUploadCount"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    Affinity getAffinity() const override { return Affinity::Editor; }
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    uint64_t m_expected;
};

// ============================================================================
// Object Animation Layer commands (Phase 3.3)
// ============================================================================

/**
 * CreateObjectAnimationLayerCommand — debug / script command for Phase 3.3.
 *
 * Creates an OA layer entity on the given track at the given start frame with
 * the given duration. Sets ObjectAnimationLayer::target to the first Screen
 * entity found in the registry (placeholder — Phase 3.5 will expose a
 * target-picker in the LayersWindow).
 *
 * Params:
 *   trackIndex       — 0-based index into timeline tracks
 *   startFrame       — first frame of the OA layer on the timeline
 *   duration         — length in timeline frames
 *   sectionBehavior  — optional "Normal" (default) | "Locked" (Phase 3.8)
 */
class CreateObjectAnimationLayerCommand : public Command {
public:
    CreateObjectAnimationLayerCommand(int trackIndex, FrameNumber startFrame,
                                      FrameNumber duration,
                                      SectionBehavior behavior = SectionBehavior::Normal)
        : m_trackIndex(trackIndex), m_startFrame(startFrame), m_duration(duration)
        , m_sectionBehavior(behavior) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "CreateObjectAnimationLayer"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    Affinity getAffinity() const override { return Affinity::Editor; }
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    int             m_trackIndex;
    FrameNumber     m_startFrame;
    FrameNumber     m_duration;
    SectionBehavior m_sectionBehavior{SectionBehavior::Normal};
    entt::entity    m_createdEntity{entt::null};
};

// ============================================================================
// Input Bus commands
// ============================================================================

/**
 * SetInputChannelCommand — write a float to a named InputBus channel.
 *
 * Lets scripts drive gameplay inputs (Muncher player axes, future
 * controllers) without a real joystick / OSC / MIDI source attached.
 * Same path OSC/MIDI plugins will eventually use, just with the value
 * arriving from a script line instead of a network packet.
 *
 * Params:
 *   channel — channel name, e.g. "muncher.input.x"
 *   value   — float; for axis channels keep in [-1, 1]
 */
class SetInputChannelCommand : public Command {
public:
    SetInputChannelCommand(std::string channel, float value)
        : m_channel(std::move(channel)), m_value(value) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "SetInputChannel"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    Affinity getAffinity() const override { return Affinity::Editor; }
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    std::string m_channel;
    float       m_value;
};

// ============================================================================
// Generative Layer commands (Muncher)
// ============================================================================

/**
 * CreateMuncherLayerCommand — script / UI command for creating a Muncher
 * generative layer on the timeline.
 *
 * Mirrors CreateObjectAnimationLayerCommand. Targets the first Screen
 * entity found in the registry; the user retargets via the Properties
 * panel once that surfaces for generative layers.
 *
 * Params:
 *   trackIndex — 0-based index into timeline tracks
 *   startFrame — first frame of the layer on the timeline
 *   duration   — length in timeline frames
 */
class CreateMuncherLayerCommand : public Command {
public:
    CreateMuncherLayerCommand(int trackIndex, FrameNumber startFrame,
                              FrameNumber duration)
        : m_trackIndex(trackIndex), m_startFrame(startFrame), m_duration(duration) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "CreateMuncherLayer"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    Affinity getAffinity() const override { return Affinity::Editor; }
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    int          m_trackIndex;
    FrameNumber  m_startFrame;
    FrameNumber  m_duration;
    entt::entity m_createdEntity{entt::null};
};

/**
 * AssertScreenSnapshotCommand — integration-test assertion.
 *
 * Calls PlaybackTimeAuthority::buildSceneSnapshot on demand and asserts a
 * named field of the ScreenSnapshot for a screen identified by name.
 *
 * field values:
 *   "positionX" | "positionY" | "positionZ"
 *   "rotationX" | "rotationY" | "rotationZ"
 *   "scaleX"    | "scaleY"    | "scaleZ"
 */
class AssertScreenSnapshotCommand : public Command {
public:
    AssertScreenSnapshotCommand(std::string screenName, std::string field,
                                float expected, float tolerance = 0.01f)
        : m_screenName(std::move(screenName)), m_field(std::move(field)),
          m_expected(expected), m_tolerance(tolerance) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "AssertScreenSnapshot"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    Affinity getAffinity() const override { return Affinity::Editor; }
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    std::string m_screenName;
    std::string m_field;
    float       m_expected;
    float       m_tolerance;
};

/**
 * AssertObjectAnimationOutputCommand — integration-test assertion.
 *
 * Resolves an OA layer entity by track index + layer index (same addressing
 * as AssertClipMediaFrame), reads its ObjectAnimationOutput component, and
 * asserts that the named field is within [expected ± tolerance].
 *
 * field values:
 *   "positionX" | "positionY" | "positionZ"
 *   "rotationX" | "rotationY" | "rotationZ"
 *   "sizeX"     | "sizeY"     | "sizeZ"
 *   "hasPosition" | "hasRotation" | "hasSize"  (assert 1.0 = true, 0.0 = false)
 */
class AssertObjectAnimationOutputCommand : public Command {
public:
    AssertObjectAnimationOutputCommand(int trackIndex, int layerIndex,
                                       std::string field, float expected, float tolerance = 0.01f)
        : m_trackIndex(trackIndex), m_layerIndex(layerIndex),
          m_field(std::move(field)), m_expected(expected), m_tolerance(tolerance) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "AssertObjectAnimationOutput"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    Affinity getAffinity() const override { return Affinity::Editor; }
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    int         m_trackIndex;
    int         m_layerIndex;
    std::string m_field;
    float       m_expected;
    float       m_tolerance;
};

// ============================================================================
// Per-layer effects (issue #54)
// ============================================================================

/**
 * Append a new effect of the given kind onto a layer's effect chain.
 * Creates the EffectChain component on the layer if absent and the new
 * effect entity with empty EffectParameters / EffectAnimatedParameters.
 *
 * JSON format:
 * {
 *     "type": "AddEffect",
 *     "layerEntity": 12345,    // raw entt::entity ID (uint32)
 *     "kindStableId": "core.gaussian_blur"
 * }
 *
 * Entity IDs aren't stable across project loads — use this command for
 * live UI / scripted sessions, not for project persistence.
 */
class AddEffectCommand : public UndoableCommand {
public:
    AddEffectCommand(entt::entity layerEntity, std::string kindStableId)
        : m_layerEntity(layerEntity), m_kindStableId(std::move(kindStableId)) {}

    bool execute(Engine& engine) override;
    bool undo(Engine& engine) override;
    const char* getTypeName() const override { return "AddEffect"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    Affinity getAffinity() const override { return Affinity::Editor; }
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    entt::entity m_layerEntity;
    std::string  m_kindStableId;
    std::optional<entt::entity> m_createdEffectEntity;
};

/**
 * Remove an effect from its layer's chain and destroy the effect entity.
 * Records enough state to recreate it on undo.
 *
 * JSON format:
 * {
 *     "type": "RemoveEffect",
 *     "layerEntity": 12345,
 *     "effectEntity": 67890
 * }
 */
class RemoveEffectCommand : public UndoableCommand {
public:
    RemoveEffectCommand(entt::entity layerEntity, entt::entity effectEntity)
        : m_layerEntity(layerEntity), m_effectEntity(effectEntity) {}

    bool execute(Engine& engine) override;
    bool undo(Engine& engine) override;
    const char* getTypeName() const override { return "RemoveEffect"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    Affinity getAffinity() const override { return Affinity::Editor; }
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    entt::entity m_layerEntity;
    entt::entity m_effectEntity;

    // Captured on execute, replayed on undo.
    bool                    m_savedExecuted{false};
    std::uint32_t           m_savedKindId{0};
    bool                    m_savedEnabled{true};
    float                   m_savedGraphX{0.0f};
    float                   m_savedGraphY{0.0f};
    std::vector<ParamValue> m_savedParams;
    std::size_t             m_savedPositionInChain{0};
};

/**
 * Toggle an effect's enabled flag.
 *
 * JSON format:
 * { "type": "SetEffectEnabled", "effectEntity": 67890, "enabled": true }
 */
class SetEffectEnabledCommand : public UndoableCommand {
public:
    SetEffectEnabledCommand(entt::entity effectEntity, bool enabled)
        : m_effectEntity(effectEntity), m_enabled(enabled) {}

    void setPreviousEnabled(bool v) { m_previousEnabled = v; }

    bool execute(Engine& engine) override;
    bool undo(Engine& engine) override;
    const char* getTypeName() const override { return "SetEffectEnabled"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    Affinity getAffinity() const override { return Affinity::Editor; }
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    entt::entity m_effectEntity;
    bool         m_enabled;
    std::optional<bool> m_previousEnabled;
};

/**
 * Update a Float parameter on an effect, identified by parameter name.
 * Slot index is resolved at execute time via EffectKindRegistry so the
 * command stays wire-stable across schema reordering.
 *
 * Only Float params are supported in v1; non-Float types arrive when
 * the UI grows color pickers / vec2 inputs in Phase 3+.
 *
 * JSON format:
 * { "type": "SetEffectFloatParam", "effectEntity": 67890,
 *   "paramName": "radius", "value": 4.5 }
 */
class SetEffectFloatParamCommand : public UndoableCommand {
public:
    SetEffectFloatParamCommand(entt::entity effectEntity,
                                std::string paramName,
                                float value)
        : m_effectEntity(effectEntity),
          m_paramName(std::move(paramName)),
          m_value(value) {}

    void setPreviousValue(float prev) { m_previousValue = prev; }

    bool execute(Engine& engine) override;
    bool undo(Engine& engine) override;
    const char* getTypeName() const override { return "SetEffectFloatParam"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    Affinity getAffinity() const override { return Affinity::Editor; }
    static CommandPtr fromJson(const nlohmann::json& j);

private:
    entt::entity m_effectEntity;
    std::string  m_paramName;
    float        m_value;
    std::optional<float> m_previousValue;
};

} // namespace entity
