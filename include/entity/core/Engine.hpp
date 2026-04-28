#pragma once

#include "Types.hpp"
#include "entity/core/SceneState.hpp"
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
class FrameCache;
class OcioManager;
class Director;       // Phase D entry — owns Timeline, ProjectManager,
                      // TranscodeManager, CommandDispatcher (subtask 4).
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

    // Phase D entry: wraps m_registry behind a Director/Renderer-aware
    // interface. New code added in subtasks 4+ uses SceneState::ReadHandle
    // / WriteHandle. Existing call sites continue to use getRegistry().
    SceneState& getSceneState() { return m_sceneState; }
    const SceneState& getSceneState() const { return m_sceneState; }

    /**
     * Get the D3D12 renderer.
     */
    IRenderer* getRenderer();  // Definition in Engine.cpp — returns m_renderer.get() upcast.

    /**
     * Get the Timeline. Forwards to Director (Phase D entry).
     */
    Timeline* getTimeline() { return m_timeline; }

    /**
     * Get the CommandDispatcher. Forwards to Director (Phase D entry).
     */
    CommandDispatcher* getCommandDispatcher() { return m_commandDispatcher; }

    /**
     * Get the Director (Phase D entry — owns Timeline, ProjectManager,
     * TranscodeManager, CommandDispatcher). Returns null until initialize().
     */
    Director* getDirector() { return m_director.get(); }
    const Director* getDirector() const { return m_director.get(); }

    /**
     * Get the OutputManager (display enumeration + physical output driving).
     */
    OutputManager* getOutputManager() { return m_outputManager.get(); }

    /**
     * Get the engine-global OcioManager (Phase C.12).
     */
    OcioManager* getOcioManager() { return m_ocioManager.get(); }

    /**
     * Get the engine-global FrameCache (decoded frames for all active clips).
     */
    FrameCache* getFrameCache() { return m_frameCache.get(); }

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

    /**
     * Phase C.12 #9 — per-clip OCIO input color-space override. Empty string
     * means "Auto (decoder)" — use whatever the codec/decoder tagged the
     * source as. A non-empty value forces the named OCIO color space for
     * every clip that resolves to this media entry, replacing the decoder
     * tag at upload time. Persisted with the project (only when non-empty).
     */
    void setInputColorSpaceOverride(const std::string& originalPath,
                                    const std::string& override);
    std::string inputColorSpaceOverride(const std::string& originalPath) const;

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

    // Director/Renderer access seam over m_registry. Declared after the
    // registry so the in-class member-initializer-list ordering is sound.
    SceneState m_sceneState{m_registry};

    // Phase D entry: Director owns Timeline, ProjectManager,
    // TranscodeManager, CommandDispatcher. Declared *before* the raw-pointer
    // shortcuts so destructor order tears the subsystems down (m_director
    // last) only after the shortcuts are no longer used. The unique_ptr
    // also outlives PlaybackController etc., which hold raw pointers into
    // these owned subsystems.
    std::unique_ptr<Director> m_director;

    // Core subsystems
    std::unique_ptr<D3D12Renderer> m_renderer;

    // Raw shortcut into m_director->getTimeline(). Set during initialize();
    // valid for the rest of Engine's lifetime. Existing call sites use the
    // member unchanged.
    Timeline* m_timeline{nullptr};

    std::unique_ptr<WindowManager> m_windowManager;

    // Raw shortcut into m_director->getCommandDispatcher().
    CommandDispatcher* m_commandDispatcher{nullptr};

    std::unique_ptr<OutputManager> m_outputManager;

    // Machine-global settings (frame-cache budget etc). Loaded from
    // settingsPath() at construction; persisted by saveSettings() whenever
    // the user clicks OK in the Preferences dialog. Per-project state lives
    // in ProjectManager / .entity files; this is the other side of that line.
    Settings m_settings{};

    // Engine-global frame cache. Sized from m_settings.frameCacheBytes at
    // initialize() and re-budgeted live when the user changes the value in
    // Preferences. Owned here; injected by raw pointer into DecodeSystem
    // (producer) and PlaybackController (consumer).
    std::unique_ptr<FrameCache> m_frameCache;

    // OpenColorIO config + processor cache (Phase C.12 #1, wired into the
    // renderer in #5). Owned here; D3D12Renderer holds a raw pointer.
    std::unique_ptr<OcioManager> m_ocioManager;

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
    // timer) lives in ProjectManager. Owned by Director (Phase D entry);
    // raw shortcut here keeps the existing call sites unchanged. See
    // include/entity/project/ProjectManager.hpp.
    ProjectManager* m_projectManager{nullptr};

    // Set when onVideoFileSelected runs into a non-HAP source with policy =
    // Ask. MediaBinWindow renders a modal; resolvePendingImport() clears it.
    std::optional<PendingImport> m_pendingImport;

    // Background HAP-transcode workers for imported media. Owned by
    // Director (Phase D entry); raw shortcut keeps existing call sites
    // unchanged. Status polled each tick; finished workers update
    // ProjectManager's media library with their output paths.
    TranscodeManager* m_transcodeManager{nullptr};

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
    TranscodeManager* getTranscodeManager() { return m_transcodeManager; }
    const TranscodeManager* getTranscodeManager() const { return m_transcodeManager; }
};

} // namespace entity
