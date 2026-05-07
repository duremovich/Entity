#pragma once

#include "Command.hpp"
#include "UndoableCommand.hpp"
#include "entity/core/Types.hpp"
#include "entity/components/Clip.hpp"   // PlaybackMode
#include "entity/components/AnimatedProperties.hpp"  // AnimatableProperty, InterpolationType
#include "entity/timeline/Timeline.hpp"  // Ripple{Insert,Delete}Result
#include "entity/timeline/CueTag.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <optional>
#include <array>

namespace entity {

// ============================================================================
// Transport Commands
// ============================================================================

class PlayCommand : public Command {
public:
    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "Play"; }
    nlohmann::json toJson() const override { return {{"type", "Play"}}; }

    static CommandPtr fromJson(const nlohmann::json& j);
};

class PauseCommand : public Command {
public:
    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "Pause"; }
    nlohmann::json toJson() const override { return {{"type", "Pause"}}; }

    static CommandPtr fromJson(const nlohmann::json& j);
};

class TogglePlayPauseCommand : public Command {
public:
    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "TogglePlayPause"; }
    nlohmann::json toJson() const override { return {{"type", "TogglePlayPause"}}; }

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

    static CommandPtr fromJson(const nlohmann::json& j);
};

class SeekToEndCommand : public Command {
public:
    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "SeekToEnd"; }
    nlohmann::json toJson() const override { return {{"type", "SeekToEnd"}}; }

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

class DeleteClipCommand : public Command {
public:
    // Delete selected clip (default)
    DeleteClipCommand() = default;

    // Delete specific entity
    explicit DeleteClipCommand(uint32_t entityId) : m_entityId(entityId) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "DeleteClip"; }
    nlohmann::json toJson() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    std::optional<uint32_t> m_entityId;
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

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    uint32_t m_count;
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
// Named-section commands (Phase C #5) — scriptable so save/load round-trip
// can be regression-tested via integration scripts.
// ============================================================================

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

class AssertSectionExistsCommand : public Command {
public:
    explicit AssertSectionExistsCommand(std::string name) : m_name(std::move(name)) {}
    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "AssertSectionExists"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;
    static CommandPtr fromJson(const nlohmann::json& j);
private:
    std::string m_name;
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

} // namespace entity
