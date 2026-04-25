#pragma once

#include "Types.hpp"
#include "entity/project/ProjectManager.hpp"
#include "entity/core/Settings.hpp"
#include "entity/media/TranscodeManager.hpp"
#include "entity/systems/System.hpp"
#include <entt/entt.hpp>
#include <memory>
#include <optional>
#include <vector>
#include <filesystem>
#include <string>

// Forward declarations
struct GLFWwindow;

namespace entity {

// Forward declarations
class IRenderer;
class D3D12Renderer;  // Owned internally; external callers get IRenderer*.
class Timeline;
class WindowManager;
class PlaybackController;
class Decoder;
class DecodeSystem;
class AnimationSystem;
class CommandDispatcher;
class OutputManager;
struct DecodedFrame;
// class Transport;

/**
 * Engine - Main engine class for Entity Media Server.
 *
 * Manages the ECS registry, main loop, systems, and core subsystems.
 * This is the top-level orchestrator for the entire application.
 */
class Engine {
public:
    Engine();
    ~Engine();

    // Non-copyable
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /**
     * Initialize the engine and all subsystems.
     *
     * @param windowWidth Width of the main window
     * @param windowHeight Height of the main window
     * @param windowTitle Window title
     * @param headless If true, window is created hidden (for integration tests / CI)
     * @return Result::Success on success, error code otherwise
     */
    Result initialize(uint32_t windowWidth, uint32_t windowHeight, const char* windowTitle, bool headless = false);

    /**
     * Shutdown the engine and clean up resources.
     */
    void shutdown();

    /**
     * Run the main application loop.
     * This will block until the application is closed.
     */
    void run();

    /**
     * Request the engine to stop and exit the main loop.
     */
    void requestExit();

    /**
     * Get the EnTT registry for entity/component management.
     */
    entt::registry& getRegistry() { return m_registry; }
    const entt::registry& getRegistry() const { return m_registry; }

    /**
     * Get the D3D12 renderer.
     */
    IRenderer* getRenderer();  // Definition in Engine.cpp — returns m_renderer.get() upcast.

    /**
     * Get the Timeline.
     */
    Timeline* getTimeline() { return m_timeline.get(); }

    /**
     * Get the CommandDispatcher.
     */
    CommandDispatcher* getCommandDispatcher() { return m_commandDispatcher.get(); }

    /**
     * Get the OutputManager (display enumeration + physical output driving).
     */
    OutputManager* getOutputManager() { return m_outputManager.get(); }

    // TODO: Implement this when class is ready
    // Transport* getTransport() { return m_transport.get(); }

    /**
     * Get the GLFW window.
     */
    GLFWwindow* getWindow() { return m_window; }

    /**
     * Get delta time for current frame (in seconds). Forwards to PlaybackController.
     */
    double getDeltaTime() const;

    /**
     * Get total elapsed time since engine start (in seconds). Forwards to PlaybackController.
     */
    double getElapsedTime() const;

    /**
     * Get current frame number. Forwards to PlaybackController.
     */
    uint64_t getFrameCount() const;

    /**
     * Get the current decoded video frame for display.
     * Returns nullptr if no frame is available. Forwards to PlaybackController.
     */
    const DecodedFrame* getCurrentVideoFrame() const;

    /**
     * Get list of loaded media files.
     */
    const std::vector<ProjectManager::MediaLibraryEntry>& getLoadedMediaFiles() const;  // forwards to ProjectManager

    /**
     * Remove a media library entry: cancel any in-flight transcode worker,
     * delete the cached HAP file from disk if present, drop the library
     * entry. Clips on the timeline that referenced this media keep playing
     * from the original source path via decoderPathFor's fallback.
     */
    void removeMediaFromLibrary(const std::string& originalPath);

    /// Non-HAP import policy preference (persisted with project).
    ProjectManager::NonHapImportPolicy nonHapImportPolicy() const;
    void setNonHapImportPolicy(ProjectManager::NonHapImportPolicy policy);

    // --- Pending transcode decision (first-import modal) ------------------
    //
    // When policy == Ask and the user imports a non-HAP file, we stash the
    // decision here and let MediaBinWindow render a modal to resolve it.
    // Only one decision can be pending at a time — a second import while
    // one is queued gets dropped with a warning.
    struct PendingImport {
        std::string filepath;
        MediaType   mediaType;
    };
    const PendingImport* pendingImport() const;

    /**
     * Called by the modal when the user picks a choice.
     *
     * @param transcode     true = enqueue the transcode worker;
     *                      false = create a clip on the source as-is
     * @param dontAskAgain  if true, save the choice to the project policy
     *                      (AlwaysTranscode / NeverTranscode) so future
     *                      imports don't prompt again
     */
    void resolvePendingImport(bool transcode, bool dontAskAgain);

    /**
     * Get the current decoder (for metadata access).
     * Returns nullptr if no media is loaded.
     */
    const Decoder* getDecoder() const { return m_decoder.get(); }

    /**
     * Register a system with the engine.
     * Systems are updated in registration order.
     */
    void registerSystem(std::unique_ptr<System> system);

    /**
     * Handle window resize event.
     * Called by GLFW callback.
     */
    void onWindowResize(uint32_t width, uint32_t height);

    /**
     * Handle keyboard input event.
     * Called by GLFW callback.
     */
    void onKeyEvent(int key, int scancode, int action, int mods);

    /**
     * Save the current project to a file.
     * @param filepath Path to save to (empty = use current project path or
     *                 ProjectManager's default fallback — non-interactive)
     * @return True if save was successful
     */
    bool saveProject(const std::filesystem::path& filepath = "");

    /**
     * Load a project from a file.
     * @param filepath Path to load from
     * @return True if load was successful
     */
    bool loadProject(const std::filesystem::path& filepath);

    /**
     * Save flow bound to Ctrl+S / "File > Save Project". Writes to the
     * current project path if one is set; otherwise prompts the user via
     * Save-As dialog.
     */
    bool saveProjectInteractive();

    /**
     * Save flow bound to Ctrl+Shift+S / "File > Save Project As...". Always
     * prompts the user via Save-As dialog.
     */
    bool saveProjectAsInteractive();

    /**
     * Open flow bound to Ctrl+O / "File > Open Project...". Prompts the
     * user via Open dialog and loads the chosen project.
     */
    bool openProjectInteractive();

    /**
     * Get the current project file path.
     */
    const std::filesystem::path& getProjectPath() const;  // forwards to ProjectManager

    /**
     * Import a video file onto the timeline.
     * @param filepath Path to the video file
     * @param trackIndex Target track index (0-based)
     * @param position Position on timeline (in microseconds)
     * @return true if import was successful
     */
    bool importVideo(const std::string& filepath, int trackIndex = 0, Timecode position = 0);

    /**
     * Capture a screenshot of the compose target.
     * @param filepath Output path for PNG file
     * @return true if capture was successful
     */
    bool captureScreenshot(const std::string& filepath);

    /**
     * Capture a screenshot of the entire window.
     * @param filepath Output path for PNG file
     * @return true if capture was successful
     */
    bool captureWindowScreenshot(const std::string& filepath);

    /**
     * Hash the compose target's pixel output (integration test harness).
     * Computes FNV-1a 64-bit hash of raw RGBA bytes, writes the hex hash
     * and dimensions to `hashFilepath`. If `goldenFilepath` is non-empty,
     * reads expected hash from that file and fails on mismatch.
     *
     * @param hashFilepath Where to write the computed hash (created/overwritten)
     * @param goldenFilepath Optional golden hash file to compare against; empty = record-only
     * @param composeSlot Compose target slot to read (default 0)
     * @return true if capture succeeded and (if golden given) hash matched
     */
    bool captureHash(const std::string& hashFilepath,
                     const std::string& goldenFilepath,
                     uint32_t composeSlot = 0);

    /**
     * Load and execute a script file.
     * @param filepath Path to JSON script file
     * @return true if script was loaded successfully
     */
    bool runScript(const std::string& filepath);

private:
    /**
     * Update all systems for the current frame.
     */
    void update();

    /**
     * Render the current frame.
     */
    void render();

    /**
     * Process window and input events.
     */
    void processEvents();

    /**
     * Test all ECS components (Phase 3).
     * Called during initialization to verify component functionality.
     */
    void testComponents();

    /**
     * Create test entities for rendering (Phase 4.1).
     * Called during initialization to create test quads for visual verification.
     */
    void createTestEntities();

    /**
     * Handle video file selection from File > Open Video menu.
     * Called when user selects a video file from the file dialog.
     */
    void onVideoFileSelected(const std::string& filePath);

    /**
     * Open a decoder, create a Clip + Transform + MediaLayer + VideoTexture +
     * FrameBuffer + ClipDecodeState, place it on the timeline. Shared by
     * `onVideoFileSelected`'s NeverTranscode branch and
     * `resolvePendingImport`'s Skip branch.
     */
    void ingestVideoClip(const std::string& filePath, MediaType mediaType);

    /**
     * Handle media dropped onto timeline track.
     * Creates a clip at the specified track and position.
     */
    void onMediaDroppedOnTimeline(const std::string& filePath, int trackIndex, Timecode position);

    /**
     * Handle new clip creation (from split/duplicate).
     * Creates decoder and GPU resources for the new clip.
     */
    void onClipCreated(entt::entity clipEntity, const std::string& filepath);

    /**
     * Create the default screen and model on startup.
     */
    void createDefaultScreen();

private:
    // ECS registry
    entt::registry m_registry;

    // Core subsystems
    std::unique_ptr<D3D12Renderer> m_renderer;
    std::unique_ptr<Timeline> m_timeline;
    std::unique_ptr<WindowManager> m_windowManager;
    std::unique_ptr<CommandDispatcher> m_commandDispatcher;
    std::unique_ptr<OutputManager> m_outputManager;

    // Machine-global settings (frame-cache budget etc). Loaded from
    // settingsPath() at construction; persisted by saveSettings() whenever
    // the user clicks OK in the Preferences dialog. Per-project state lives
    // in ProjectManager / .entity files; this is the other side of that line.
    Settings m_settings{};

    // Systems
    std::vector<std::unique_ptr<System>> m_systems;
    DecodeSystem* m_decodeSystem{nullptr};  // Raw pointer for direct access (owned by m_systems)
    AnimationSystem* m_animationSystem{nullptr};  // Raw pointer for direct access (owned by m_systems)

    // Non-owning ref to the active TimelineWidget. Used by the Edit menu and
    // keyboard handler to read the current range selection. Owned by the
    // WindowManager via TimelineWindow.
    class TimelineWidget* m_timelineWidget{nullptr};

    // TODO: implement when class is ready
    // std::unique_ptr<Transport> m_transport;

    // Window
    GLFWwindow* m_window{nullptr};
    uint32_t m_windowWidth{1920};
    uint32_t m_windowHeight{1080};

    // Playback coordination (frame timing, clip-frame math, per-frame seek-aware updates).
    // Constructed after Timeline + renderer exist; DecodeSystem is injected after registration.
    std::unique_ptr<PlaybackController> m_playbackController;

    // FPS display tracking (window title only)
    double m_fpsAccumulator{0.0};
    uint32_t m_fpsFrameCount{0};
    uint32_t m_currentFPS{0};

    // State
    bool m_initialized{false};
    bool m_running{false};
    bool m_resizePending{false};
    uint32_t m_pendingWidth{0};
    uint32_t m_pendingHeight{0};

    // Video playback (legacy single-clip for backwards compatibility)
    std::unique_ptr<Decoder> m_decoder;
    std::unique_ptr<DecodedFrame> m_currentFrame;

    // Per-clip-entity decode state is now a proper component
    // (include/entity/components/ClipDecodeState.hpp); access via
    // registry.try_get<ClipDecodeState>(entity) from systems.

    // Project-scoped state (project path, loaded media library, autosave
    // timer) lives in ProjectManager. Engine owns it and forwards save/load
    // / autosave calls. See include/entity/project/ProjectManager.hpp.
    std::unique_ptr<ProjectManager> m_projectManager;

    // Set when onVideoFileSelected runs into a non-HAP source with policy =
    // Ask. MediaBinWindow renders a modal; resolvePendingImport() clears it.
    std::optional<PendingImport> m_pendingImport;

    // Background HAP-transcode workers for imported media. Status polled
    // each tick; finished workers update ProjectManager's media library
    // with their output paths.
    std::unique_ptr<TranscodeManager> m_transcodeManager;

    // Per-tick autosave hook — delegates to m_projectManager->tickAutosave.
    void autoSaveTick(double deltaTime);

    // Per-tick transcode-status poll. Moves Done workers' output paths
    // into the media library and prunes finished entries.
    void pollTranscodes();

    /**
     * Point the transcode manager at <projectDir>/.cache/hap, creating
     * the dir on demand. Called on project save/load. Falls back to the
     * system temp dir when no project path is set yet.
     */
    void updateTranscodeCacheDir();
public:
    TranscodeManager* getTranscodeManager() { return m_transcodeManager.get(); }
    const TranscodeManager* getTranscodeManager() const { return m_transcodeManager.get(); }
};

} // namespace entity
