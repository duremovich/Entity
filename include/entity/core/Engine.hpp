#pragma once

#include "Types.hpp"
#include "entity/core/SceneState.hpp"
#include "entity/project/ProjectManager.hpp"
#include "entity/core/Settings.hpp"
#include "entity/media/TranscodeManager.hpp"
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
class Timeline;
class WindowManager;
class Decoder;
class DecodeSystem;
class AnimationSystem;
class CommandDispatcher;
class OutputManager;
class FrameCache;
class OcioManager;
class PlaybackTimeAuthority;
class PlaybackPresenter;
class Director;       // Phase D entry — owns Timeline, ProjectManager,
                      // TranscodeManager, CommandDispatcher,
                      // AnimationSystem (subtasks 4 + 5).
class Renderer;       // Phase D entry — owns D3D12Renderer, OutputManager,
                      // FrameCache, OcioManager, CompositorSystem,
                      // DecodeSystem (subtask 5).
class ProjectLauncher;  // ADR-0009 — Recent/New/Open dialog rendered when
                        // no project is loaded. Defined in entity/ui/.
class RecentProjects;   // ADR-0009 — persisted recent-projects list backing
                        // the launcher's Recent panel.
struct DecodedFrame;
namespace bus {
class IMessageTransport;
struct SetOutputEnabled;
struct ApplySettings;
struct RequestComposeCapture;
struct CaptureCompleted;
struct ProvisionClipResources;
struct ResourcesProvisioned;
}
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
     * TranscodeManager, CommandDispatcher, AnimationSystem). Returns null
     * until initialize().
     */
    Director* getDirector() { return m_director.get(); }
    const Director* getDirector() const { return m_director.get(); }

    /**
     * Get the Renderer service (Phase D entry — owns D3D12Renderer,
     * OutputManager, FrameCache, OcioManager, CompositorSystem,
     * DecodeSystem). Returns null until initialize().
     */
    Renderer* getRendererService() { return m_rendererService.get(); }
    const Renderer* getRendererService() const { return m_rendererService.get(); }

    /**
     * Get the OutputManager (display enumeration + physical output driving).
     */
    OutputManager* getOutputManager() { return m_outputManager; }

    /**
     * Get the engine-global OcioManager (Phase C.12).
     */
    OcioManager* getOcioManager() { return m_ocioManager; }

    /**
     * Get the engine-global FrameCache (decoded frames for all active clips).
     */
    FrameCache* getFrameCache() { return m_frameCache; }

    // TODO: Implement this when class is ready
    // Transport* getTransport() { return m_transport.get(); }

    /**
     * Get the bus message transport. Used by the plugin scaffold
     * (EnginePluginContext) to expose the bus to plugins. Returns null
     * before initialize() and after shutdown().
     */
    bus::IMessageTransport* getBusTransport() { return m_transport.get(); }

    /**
     * Get the GLFW window.
     */
    GLFWwindow* getWindow() { return m_window; }

    // --- Structured imports (ADR-0009) --------------------------------------

    /**
     * What an import does with the source file. Default is `Link` to
     * keep legacy behavior (script-driven imports of pre-existing test
     * media). Interactive imports flip the default to `Copy` via the
     * MediaBin toolbar.
     */
    enum class ImportMode {
        Link = 0,  // Store absolute path; pathKind = Linked
        Copy = 1,  // Copy into content/<sub>/<filename>; pathKind = Managed
    };

    /**
     * Set the import mode + target subfolder used by subsequent
     * File > Open Video imports. Called by the MediaBin toolbar when
     * the user changes either setting; persists for the rest of the
     * session.
     */
    void setImportMode(ImportMode mode, const std::string& subfolder);
    ImportMode  importMode()      const { return m_importMode; }
    const std::string& importSubfolder() const { return m_importSubfolder; }

    /**
     * Apply the current import mode to a source file and return the
     * canonical originalPath that should be used as the clip / media
     * library identity. For `Copy`, this physically copies the file
     * into `<projectRoot>/content/<subfolder>/<filename>` and returns
     * the project-relative path (Managed). For `Link`, returns the
     * input absolute path unchanged (Linked).
     *
     * Falls back to Link in two cases: no project loaded, or
     * `<projectRoot>/content/` doesn't exist (legacy v6 project).
     * Both surface a console warning.
     *
     * `outKind` receives the resolved kind so callers can register
     * the entry correctly.
     *
     * Returns empty string on copy failure.
     */
    std::string applyImportMode(const std::string& sourceAbsolutePath,
                                ImportMode mode,
                                const std::string& subfolder,
                                ProjectManager::PathKind* outKind);

    // --- Project Launcher (ADR-0009) ----------------------------------------

    /**
     * Enable launcher mode: the editor UI is hidden and the Project
     * Launcher (Recent / New / Open) renders fullscreen until the user
     * picks a project. Call after `initialize()` and before `run()`.
     *
     * `--script` runs bypass this (the script drives state via
     * OpenProject commands). main.cpp decides whether to call this.
     */
    void showLauncher();

    /**
     * Whether the engine is currently in launcher mode. While true,
     * `update()` skips per-tick simulation work and `render()` draws
     * only the launcher.
     */
    bool isLauncherActive() const { return m_showLauncher; }

    /**
     * Get delta time for current frame (in seconds). Forwards to PlaybackTimeAuthority.
     */
    double getDeltaTime() const;

    /**
     * Get total elapsed time since engine start (in seconds). Forwards to PlaybackTimeAuthority.
     */
    double getElapsedTime() const;

    /**
     * Get current frame number. Forwards to PlaybackTimeAuthority.
     */
    uint64_t getFrameCount() const;

    /**
     * Get the current decoded video frame for display.
     * Returns nullptr if no frame is available. Forwards to PlaybackPresenter
     * (Renderer side) which queries the authority for clip-frame math.
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
     * Load and execute a script file.
     * @param filepath Path to JSON script file
     * @return true if script was loaded successfully
     */
    bool runScript(const std::string& filepath);

    // Phase D entry, subtask 7: Director-side publishers for the
    // Renderer-touching messages. Each helper serializes its argument and
    // posts on the D2R channel; Renderer drains during `render()`.
    // No-ops if `m_transport` is null (which is only possible before
    // initialize() or after shutdown()). Defined in Engine.cpp.
    void publishSetOutputEnabled(const bus::SetOutputEnabled& msg);
    void publishApplySettings(const bus::ApplySettings& msg);
    void publishProvisionClipResources(const bus::ProvisionClipResources& msg);

private:
    // Subtask 7. Pre-beginFrame drain (Renderer side): pulls
    // RequestComposeCapture out of D2R and runs the existing capture
    // pass while the GPU is in the settled state captureBackBufferToPNG
    // and tonemapAndReadbackComposeTarget both require. Other D2R
    // messages stash and replay after beginFrame.
    void drainCaptureRequestsPreFrame();

    // Subtask 7. Post-render drain (Director side): consumes
    // CaptureCompleted replies and routes them to the broker for
    // script-result resolution.
    void drainRendererToDirector();

    // Subtask 7. Single-message Renderer-side handler for
    // RequestComposeCapture; published reply lives on R2D.
    void handleCaptureRequest(const bus::RequestComposeCapture& req);

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
     *
     * `canonicalPath` is the lookup key — relative for Managed, absolute
     * for Linked. The decoder is opened against `decoderPathFor(canonicalPath)`
     * so the same code path works regardless of pathKind. The caller is
     * responsible for having already registered the entry via
     * `addMediaFile(canonicalPath, kind)`.
     */
    void ingestVideoClip(const std::string& canonicalPath, MediaType mediaType);

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

    /**
     * Resolve a launcher Action::Open: load the project, refresh the
     * recent-projects list, leave launcher mode. On load failure the
     * stale entry is removed from Recent and we stay in launcher mode
     * so the user can pick something else.
     */
    void onLauncherOpenProject(const std::filesystem::path& path);

private:
    // ECS registry
    entt::registry m_registry;

    // Director/Renderer access seam over m_registry. Declared after the
    // registry so the in-class member-initializer-list ordering is sound.
    SceneState m_sceneState{m_registry};

    // Phase D entry: Renderer service owns the GPU-side stack
    // (D3D12Renderer, OutputManager, FrameCache, OcioManager,
    // CompositorSystem, DecodeSystem, TestSystem). Declared *before*
    // m_director so it constructs first (Director's ProjectManager wants
    // an IRenderer*) and *before* the raw-pointer shortcuts below so the
    // shortcuts stay valid until shutdown explicitly nulls them.
    std::unique_ptr<Renderer> m_rendererService;

    // Phase D entry: Director owns Timeline, ProjectManager,
    // TranscodeManager, CommandDispatcher, AnimationSystem,
    // PlaybackTimeAuthority. Declared *before* the raw-pointer shortcuts
    // so destructor order tears the subsystems down (m_director last)
    // only after the shortcuts are no longer used.
    std::unique_ptr<Director> m_director;

    // Raw shortcuts into m_rendererService. Set during initialize(); valid
    // for the rest of Engine's lifetime. Existing call sites use the member
    // unchanged. Subtask 8 will replace the per-tick reads with the
    // RenderFrame bus message.
    IRenderer*       m_renderer{nullptr};
    OutputManager*   m_outputManager{nullptr};
    FrameCache*      m_frameCache{nullptr};
    OcioManager*     m_ocioManager{nullptr};
    DecodeSystem*    m_decodeSystem{nullptr};

    // Raw shortcut into m_director->getTimeline(). Set during initialize();
    // valid for the rest of Engine's lifetime. Existing call sites use the
    // member unchanged.
    Timeline* m_timeline{nullptr};

    std::unique_ptr<WindowManager> m_windowManager;

    // ADR-0009: Project Launcher state. The launcher and its recent-
    // projects backing store outlive a single project (the user can
    // close one project and re-enter the launcher). Both are
    // unique_ptr so the forward declarations above stay sufficient.
    std::unique_ptr<ProjectLauncher> m_launcher;
    std::unique_ptr<RecentProjects>  m_recentProjects;
    bool m_showLauncher{false};

    // ADR-0009 — current import mode + target subfolder. Default is Link
    // so legacy / script-driven flows keep their pre-launcher behavior;
    // MediaBin's toolbar flips it to Copy + "unsorted" on first render.
    ImportMode  m_importMode{ImportMode::Link};
    std::string m_importSubfolder{"unsorted"};

    // Raw shortcut into m_director->getCommandDispatcher().
    CommandDispatcher* m_commandDispatcher{nullptr};

    // Raw shortcut into m_director->getAnimationSystem(). Director owns
    // AnimationSystem (Phase D entry, subtask 5).
    AnimationSystem* m_animationSystem{nullptr};

    // Machine-global settings (frame-cache budget etc). Loaded from
    // settingsPath() at construction; persisted by saveSettings() whenever
    // the user clicks OK in the Preferences dialog. Per-project state lives
    // in ProjectManager / .entity files; this is the other side of that line.
    Settings m_settings{};

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

    // Phase D entry, subtask 6: PlaybackController is gone -- split into
    // PlaybackTimeAuthority (Director-side) and PlaybackPresenter
    // (Renderer-side). Both live on their respective owners; the raw
    // shortcuts here let Engine glue keep its existing call sites short.
    PlaybackTimeAuthority* m_timeAuthority{nullptr};
    PlaybackPresenter*     m_playbackPresenter{nullptr};

    // Phase D entry, subtask 8: Director->Renderer per-tick state-snapshot
    // travels through this transport. In-process today (no threads, no
    // UDP); Phase E swaps the implementation without touching the message
    // format. Engine builds the message Director-side, drains it
    // Renderer-side inside render() right after beginFrame().
    std::unique_ptr<bus::IMessageTransport> m_transport;

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
