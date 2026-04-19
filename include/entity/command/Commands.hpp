#pragma once

#include "Command.hpp"
#include "entity/core/Types.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <optional>

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

class SetClipBlendModeCommand : public Command {
public:
    SetClipBlendModeCommand(int trackIndex, int clipIndex, BlendMode mode)
        : m_trackIndex(trackIndex), m_clipIndex(clipIndex), m_blendMode(mode) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "SetClipBlendMode"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    int m_trackIndex;
    int m_clipIndex;
    BlendMode m_blendMode;
};

class SetClipOpacityCommand : public Command {
public:
    SetClipOpacityCommand(int trackIndex, int clipIndex, float opacity)
        : m_trackIndex(trackIndex), m_clipIndex(clipIndex), m_opacity(opacity) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "SetClipOpacity"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    int m_trackIndex;
    int m_clipIndex;
    float m_opacity;
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

class SetClipRotationCommand : public Command {
public:
    SetClipRotationCommand(int trackIndex, int clipIndex, float rotX, float rotY, float rotZ)
        : m_trackIndex(trackIndex), m_clipIndex(clipIndex),
          m_rotX(rotX), m_rotY(rotY), m_rotZ(rotZ) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "SetClipRotation"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    int m_trackIndex;
    int m_clipIndex;
    float m_rotX, m_rotY, m_rotZ;
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
class SetClipTargetScreenCommand : public Command {
public:
    SetClipTargetScreenCommand(int trackIndex, int clipIndex, const std::string& screenName)
        : m_trackIndex(trackIndex), m_clipIndex(clipIndex), m_screenName(screenName) {}

    bool execute(Engine& engine) override;
    const char* getTypeName() const override { return "SetClipTargetScreen"; }
    nlohmann::json toJson() const override;
    std::string getDescription() const override;

    static CommandPtr fromJson(const nlohmann::json& j);

private:
    int m_trackIndex;
    int m_clipIndex;
    std::string m_screenName;
};

} // namespace entity
