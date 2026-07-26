#pragma once

#include "Types.hpp"
#include "entity/audio/AudioEngine.hpp"
#include "entity/core/ClipClipboard.hpp"
#include "entity/core/SceneState.hpp"
#include "entity/project/ProjectManager.hpp"
#include "entity/core/Settings.hpp"
#include "entity/media/TranscodeManager.hpp"
#include <entt/entt.hpp>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>
#include <filesystem>
#include <string>

// Forward declarations
struct GLFWwindow;

// Included for bus::SceneSnapshot cached in Engine (show thread).
#include "entity/bus/Message.hpp"

// Included for entity::plugin::SignalEmitPod used in postSignalEmit().
#include "entity/plugin/PluginContext.hpp"
// Included for SignalOutputSystem show-thread member.
#include "entity/systems/SignalOutputSystem.hpp"

namespace entity::core {
class EnginePluginContext;
} // namespace entity::core

namespace entity {

// Forward declarations
class IRenderer;
class UiTestHarness;
class Timeline;
class WindowManager;
class Decoder;
class DecodeSystem;
class AnimationSystem;
class GenerativeSystem;
class RoutingLibrarySystem;
class TextSystem;
class AudioSystem;
class CommandDispatcher;
class InputBus;
class OutputManager;
class FrameCache;
class OcioManager;
class PlaybackTimeAuthority;
class SectionScheduler;
class PlaybackPresenter;
class SeekSyncController;
namespace effects { class EffectKindRegistry; }
namespace remote { class RemoteControlStore; }
class PrecompLibrary; // ADR-0029 — Engine-owned precomp definition
                      // library. Defined in entity/precomp/.
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
class ContentScanner;   // #27 — periodic poll of project's content/ folder
                        // for files added or removed by external tools.
class MediaProbeWorker; // Off-thread FFmpeg metadata probe so MediaBin's
                        // first-row-render doesn't freeze on a 4K ProRes
                        // avformat_open_input. Lives in entity/media/.
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
     * Get the UI test harness (ImGui Test Engine). Null unless the build
     * has ENTITY_ENABLE_UI_TESTS — Ui* script commands fail cleanly then.
     */
    UiTestHarness* getUiTestHarness() { return m_uiTestHarness.get(); }

    /**
     * Get the Director (Phase D entry — owns Timeline, ProjectManager,
     * TranscodeManager, CommandDispatcher, AnimationSystem). Returns null
     * until initialize().
     */
    Director* getDirector() { return m_director.get(); }
    const Director* getDirector() const { return m_director.get(); }

    // Effect kind registry — owned by Engine, populated at initialize().
    // Used by PropertyWindow's effects section to enumerate kinds and by
    // command execute() to resolve kind IDs / schemas. Read-only after
    // registerBuiltins; user-effect mutation (Phase 6) is editor-thread only.
    effects::EffectKindRegistry* getEffectKindRegistry() {
        return m_effectKindRegistry.get();
    }
    const effects::EffectKindRegistry* getEffectKindRegistry() const {
        return m_effectKindRegistry.get();
    }

    // Live remote-control value plane (ADR-0028). Written by plugin
    // threads + editor UI; sampled lock-free by the show thread.
    remote::RemoteControlStore* getRemoteControlStore() {
        return m_remoteControlStore.get();
    }

    // Precomp definition library (ADR-0029). Editor-thread authoring
    // state; definition Timelines share m_registry and are never handed
    // to playback systems. Returns null until initialize().
    PrecompLibrary* getPrecompLibrary() { return m_precompLibrary.get(); }
    const PrecompLibrary* getPrecompLibrary() const {
        return m_precompLibrary.get();
    }

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

    /**
     * Get the DecodeSystem (per-clip decode workers). Used by the Clip Info
     * playback readout to show the worker's decoded / target frame.
     */
    DecodeSystem* getDecodeSystem() { return m_decodeSystem; }

    /**
     * Get the PlaybackPresenter (GPU upload pass). Used by the Clip Info
     * playback readout for the last frame actually presented to the texture.
     */
    PlaybackPresenter* getPlaybackPresenter() { return m_playbackPresenter; }

    // TODO: Implement this when class is ready
    // Transport* getTransport() { return m_transport.get(); }

    /**
     * Get the bus message transport. Used by the plugin scaffold
     * (EnginePluginContext) to expose the bus to plugins. Returns null
     * before initialize() and after shutdown().
     */
    bus::IMessageTransport* getBusTransport() { return m_transport.get(); }

    // Audio engine — owns the device, mixer, and rate source. Null until
    // initialize(); stop() is called in shutdown().
    AudioEngine* getAudioEngine() { return m_audioEngine.get(); }

    /**
     * Get the cross-system input channel bus. Input sources (OSC plugins,
     * editor keyboard handler, MIDI plugins, script `SetInputChannel`)
     * write to it; gameplay systems (GenerativeSystem) read from it.
     * Lives for Engine's lifetime; returned pointer is valid between
     * initialize() and shutdown().
     */
    InputBus* getInputBus() { return m_inputBus.get(); }
    const InputBus* getInputBus() const { return m_inputBus.get(); }

    /**
     * Register a plugin shutdown hook. Hooks fire at the start of
     * Engine::shutdown(), in registration order, before any subsystem is
     * torn down — gives plugins a chance to join worker threads that
     * touch the bus or dispatcher. The function pointer must remain
     * valid until the engine has finished shutting down. Used by
     * EnginePluginContext::registerShutdownHook(); plugins should not
     * call this directly.
     */
    void addPluginShutdownHook(void (*hook)()) {
        if (hook) m_pluginShutdownHooks.push_back(hook);
    }

    /**
     * Wire the active EnginePluginContext into Engine so postSignalEmit()
     * can reach it. Called from main.cpp after registerStaticPlugins().
     * Non-owning: Engine does not delete the context.
     */
    void setPluginContext(entity::core::EnginePluginContext* ctx) {
        m_pluginCtx = ctx;
    }

    /**
     * Request a SignalOutputSystem reset on the next show-thread tick.
     * Call on any timeline discontinuity (seek, load, stop) so the
     * crossing-detector arm state and continuous rate-limit don't carry over.
     * Thread-safe: sets an atomic flag; the show thread applies it.
     */
    void requestSignalReset() noexcept {
        m_signalResetPending.store(true, std::memory_order_release);
    }

    /**
     * Post a SignalEmit event for consumption by registered plugins
     * (Phase 2+). Thread-safe: delegates to EnginePluginContext which
     * guards its queue with a mutex. Safe to call from the show thread
     * (Phase 6) or from editor-thread command handlers (Phase 2 test hook).
     * No-op when no plugin context has been wired in.
     */
    void postSignalEmit(const entity::plugin::SignalEmitPod& emit) noexcept;

    /**
     * Convenience overload: converts bus::SignalEmit -> SignalEmitPod internally
     * so the show-thread signal pipeline can pass the bus type directly without
     * manually constructing a pod. Truncates args beyond kMaxArgs and strings
     * beyond kMaxStringLen; address is clamped to kMaxAddressLen.
     * Thread-safe (delegates to the pod overload).
     */
    void postSignalEmit(const bus::SignalEmit& emit) noexcept;

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
     * ADR-0009 — one-shot "Collect Linked media into the project folder"
     * action. For every `Linked` entry in the media library whose source
     * file resolves on disk:
     *
     *   1. Copy the source into
     *      `<projectRoot>/content/<subfolder>/<filename>`,
     *      with collision-suffixed names if needed.
     *   2. If a cache-dir transcode exists for the entry, treat the
     *      collected source as the pre-transcode original: copy the
     *      source into `content/<subfolder>/.archive/<filename>` and
     *      move the cache-dir HAP to the canonical content path. Set
     *      `archivedOriginal` + `originalCodec` accordingly. Otherwise
     *      no archive is created.
     *   3. Flip the entry to `Managed` with the new project-relative
     *      `originalPath`; clear `transcodedPath` (Managed convention:
     *      the canonical path IS the playable file, regardless of
     *      whether a transcode was collected).
     *   4. Walk the registry and rewrite `clip.filepath` for every clip
     *      that referenced the old absolute `originalPath`.
     *
     * `Managed` entries and `Linked` entries whose source no longer
     * resolves are left untouched; the latter is logged so the operator
     * can decide whether to relink or remove them. The original Linked
     * source files at their pre-collect paths are NOT deleted — the
     * operation is non-destructive on the user's disk outside the
     * project folder.
     *
     * Returns the number of entries successfully collected.
     */
    int collectLinkedIntoProject(const std::string& subfolder = "unsorted");

    /**
     * Number of `Linked` entries in the loaded media library. Used by
     * the File menu to disable "Collect Linked Media..." when there's
     * nothing to do.
     */
    int countLinkedEntries() const;

    /**
     * ADR-0009 — orphan-showfile recovery actions.
     *
     * `rebuildProjectStructure()` — idempotent mkdir of canonical
     * subdirectories under the loaded project. Call after opening an
     * .entity file produced by the bare "Save Project As..." path
     * (no bundle) at a location that doesn't have content/, presets/,
     * etc. Returns the number of dirs actually created.
     *
     * `findMissingMedia(searchDir)` — recursively walks `searchDir`,
     * matches by filename against Managed mediaLibrary entries whose
     * canonical path is missing on disk, and copies the matches into
     * their canonical content paths. After a successful run that
     * restored at least one file, the project is reloaded so clip
     * decode state attaches to the freshly-restored media. Returns
     * the number of entries successfully restored.
     */
    int rebuildProjectStructure();
    int findMissingMedia(const std::filesystem::path& searchDir);

    /**
     * ADR-0009 — Restore the pre-transcode original for a Managed
     * mediaLibrary entry that has an archive on disk. Wraps
     * ProjectManager::restoreOriginal: cancels any in-flight
     * TranscodeManager worker keyed by canonicalPath, runs the
     * archive→canonical swap, reloads the project so the decoder
     * picks up the new codec.
     *
     * Returns true on success.
     */
    bool restoreOriginalMedia(const std::string& canonicalPath);

    /**
     * ADR-0009 — orchestrate a HAP transcode for a registered media
     * library entry. Branches on `pathKind`:
     *
     *  - Managed: copy `<root>/<canonicalPath>` to
     *    `<root>/content/<sub>/.archive/<filename>`, then enqueue the
     *    transcode reading from the (about-to-be-overwritten) source
     *    and writing to the canonical content path. The original lives
     *    in `.archive/` after success; archivedOriginal + originalCodec
     *    are written when the worker reports Done (in pollTranscodes).
     *  - Linked: legacy behavior — enqueue with the cache dir as
     *    output. Fields stay on `transcodedPath`.
     *
     * No-op (returns false) if no entry is registered for canonicalPath
     * or if any filesystem step fails. Already-transcoded entries
     * (Managed: archivedOriginal set; Linked: transcodedPath set) are
     * also no-ops.
     */
    bool scheduleTranscode(const std::string& canonicalPath,
                           MediaType sourceMediaType,
                           const std::string& variant = "hap_alpha",
                           double srcFps = 0.0);

    /**
     * How to resolve a name collision when Copy-importing into a
     * directory that already has a file with the same name. Shared
     * across media and object imports.
     *
     *  - `Replace`  overwrites the existing file.
     *  - `KeepBoth` appends "(1)", "(2)", … to the new file's stem
     *               (the silent-fallback behavior on auto-discovery
     *               paths and pinned policies).
     */
    enum class DuplicateResolution {
        Replace,
        KeepBoth,
    };

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
     * the entry correctly. `resolution` controls behavior when the
     * target path already has a file: defaults to KeepBoth so silent
     * paths (pinned policy / auto-discovery) don't surprise-overwrite;
     * modal flows route collisions through `PendingMediaDuplicate` and
     * pass the user's chosen resolution back here.
     *
     * Returns empty string on copy failure.
     */
    std::string applyImportMode(const std::string& sourceAbsolutePath,
                                ImportMode mode,
                                const std::string& subfolder,
                                ProjectManager::PathKind* outKind,
                                DuplicateResolution resolution =
                                    DuplicateResolution::KeepBoth);

    /**
     * Same as applyImportMode but for 3D object imports. Copies into
     * `objects/<subfolder>/`; returns a project-relative path
     * (forward-slashed) for Managed mode, or the absolute source path
     * for Linked. `resolution` controls collision behavior; modal
     * flows resolve through `PendingObjectDuplicate`.
     */
    std::string applyObjectImportMode(const std::string& sourceAbsolutePath,
                                      ImportMode mode,
                                      const std::string& subfolder,
                                      ProjectManager::PathKind* outKind,
                                      DuplicateResolution resolution =
                                          DuplicateResolution::KeepBoth);

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
     * Get list of loaded media files.
     */
    const std::vector<ProjectManager::MediaLibraryEntry>& getLoadedMediaFiles() const;  // forwards to ProjectManager

    /**
     * Get the ProjectManager pointer for UI windows that need richer
     * access than the narrow forwarding accessors (e.g. the object
     * library + version grouping in ModelBinWindow). May be nullptr
     * before initialization.
     */
    ProjectManager* getProjectManager() const { return m_projectManager; }

    /**
     * Resolve a stored media path (Managed = relative under content/,
     * Linked = absolute) to a path the OS / decoder / FFmpeg can open.
     * Pass-through to ProjectManager::decoderPathFor — picks the right
     * version (auto-roll vs pinned, #27) and routes Managed paths
     * through the project root. Empty path in → empty path out.
     */
    std::string resolveMediaPath(const std::string& storedPath) const;

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
    // #32 — when either Settings.importStoragePolicy or
    // ProjectManager.nonHapImportPolicy is "Ask", `onVideoFileSelected`
    // stashes a PendingImport here and MediaBinWindow renders the unified
    // modal. The modal asks only the questions that aren't already
    // pre-decided.
    //
    // Critically, storage decision must precede `applyImportMode` (the
    // file copy hasn't happened yet at this point), so PendingImport
    // carries the *source* path, not a canonicalized one.
    struct PendingImport {
        std::string sourceFilePath;  // user-picked source, pre-copy
        MediaType   mediaType;

        // Whether this file is eligible for HAP transcode (non-HAP
        // container + transcode infra available). When false, the modal
        // hides the transcode question entirely.
        bool transcodeEligible{false};

        // True if storage was already decided by the persisted policy
        // (AlwaysCopy / AlwaysLink). The modal hides storage in that case.
        bool storageDecided{false};
        ImportMode  resolvedMode{ImportMode::Link};

        // True if transcode was already decided (AlwaysTranscode /
        // NeverTranscode). Modal hides transcode in that case.
        bool transcodeDecided{false};
        bool resolvedTranscode{false};
    };
    const PendingImport* pendingImport() const;

    /**
     * Called by the unified modal when the user picks a choice (#32).
     *
     * @param mode             chosen storage mode
     * @param subfolder        target subfolder under content/ (Copy mode only,
     *                         empty = root)
     * @param transcode        true = enqueue HAP transcode (eligible imports
     *                         only)
     * @param rememberStorage  persist storage choice to Settings as
     *                         AlwaysCopy / AlwaysLink
     * @param rememberTranscode persist transcode choice to project's
     *                         NonHapImportPolicy
     */
    void resolvePendingImport(ImportMode mode,
                               const std::string& subfolder,
                               bool transcode,
                               bool rememberStorage,
                               bool rememberTranscode);

    // --- Pending media duplicate (file-already-exists prompt) -----------
    //
    // When resolvePendingImport notices that the chosen Copy target
    // already exists at `content/<subfolder>/<filename>`, it stashes a
    // PendingMediaDuplicate here instead of silently suffixing.
    // MediaBinWindow renders a second modal with the user's three
    // options; resolvePendingMediaDuplicate finalizes (the transcode
    // decision the user already made in the first modal rides along).
    struct PendingMediaDuplicate {
        std::string sourceFilePath;   // pre-copy source the user picked
        std::string subfolder;        // chosen subfolder under content/
        std::string targetFilename;   // colliding filename
        std::string suggestedRenamed; // pre-computed auto-suffix preview
        MediaType   mediaType{MediaType::Unknown};
        bool        transcodeEligible{false};
        bool        transcode{false}; // user's transcode decision from modal 1
    };
    const PendingMediaDuplicate* pendingMediaDuplicate() const;

    /**
     * Resolve the file-already-exists prompt for media. Completes the
     * import using the user's chosen resolution + the transcode
     * decision already captured in the pending state.
     */
    void resolvePendingMediaDuplicate(DuplicateResolution resolution);

    /** Abandon a pending media duplicate import (Cancel button / ESC). */
    void cancelPendingMediaDuplicate();

    // --- Pending object import (3D models) -------------------------------
    //
    // Same Ask / AlwaysCopy / AlwaysLink mechanism as the media import,
    // but stripped down: objects have no transcode equivalent so the
    // modal only asks the storage question. The Settings.importStoragePolicy
    // value is *shared* with media — "Don't ask Storage again" carries
    // across both kinds (matches the "should act the same as media"
    // framing). The transcode pieces of PendingImport are absent here.
    struct PendingObjectImport {
        std::string sourceFilePath;  // user-picked source, pre-copy
        bool        storageDecided{false};
        ImportMode  resolvedMode{ImportMode::Link};
    };
    const PendingObjectImport* pendingObjectImport() const;

    /**
     * Called by the object import modal when the user picks a choice.
     *
     * @param mode             chosen storage mode
     * @param subfolder        target subfolder under objects/ (Copy mode
     *                         only, empty = root)
     * @param rememberStorage  persist storage choice to Settings as
     *                         AlwaysCopy / AlwaysLink (shared with media)
     */
    void resolvePendingObjectImport(ImportMode mode,
                                    const std::string& subfolder,
                                    bool rememberStorage);

    // --- Pending object duplicate (file-already-exists prompt) ----------
    //
    // When resolvePendingObjectImport notices that the chosen Copy
    // target already exists at `objects/<subfolder>/<filename>`, it
    // stashes a PendingObjectDuplicate here instead of silently
    // suffixing. ModelBinWindow renders a second modal with the user's
    // three options; resolvePendingObjectDuplicate finalizes the copy
    // with their chosen resolution.
    //
    // Only Copy imports hit this — Link mode doesn't write anywhere
    // new, so there's nothing to collide with.
    struct PendingObjectDuplicate {
        std::string sourceFilePath;   // pre-copy source the user picked
        std::string subfolder;        // chosen subfolder under objects/
        std::string targetFilename;   // colliding filename (e.g. "chair.obj")
        std::string suggestedRenamed; // pre-computed auto-suffix preview
    };
    const PendingObjectDuplicate* pendingObjectDuplicate() const;

    /**
     * Resolve the file-already-exists prompt. Passing `Replace`
     * completes the import by overwriting; `KeepBoth` completes by
     * auto-suffixing the new file's name; clearing without a resolve
     * (handled separately as Cancel via `cancelPendingObjectDuplicate`)
     * abandons the import entirely.
     */
    void resolvePendingObjectDuplicate(DuplicateResolution resolution);

    /** Abandon a pending duplicate import (Cancel button / ESC). */
    void cancelPendingObjectDuplicate();

    /**
     * Handle 3D model file selection from ModelBinWindow's Import OBJ
     * button or external drag-drop onto the model bin. Routes through
     * the unified storage modal (or applies the pinned policy silently
     * if `Settings::importStoragePolicy != Ask`).
     */
    void onModelFileSelected(const std::string& filePath);

    /**
     * Register a model file in the ProjectManager's object library and
     * create-or-update the Model entity for its logical group.
     *
     * `canonicalPath` is the lookup key — project-relative for Managed,
     * absolute for Linked. The mesh is parsed via `ObjLoader::load` on
     * the editor thread; cached size is recorded for hot-reload.
     *
     * If a Model entity already exists for the file's logical group
     * (same `groupKeyOf`), it is updated in place — its mesh is reloaded
     * from the now-latest version and GPU resources are invalidated so
     * the renderer re-uploads next tick. Otherwise a new Model entity
     * is created with the logical path as its `filepath`.
     */
    void ingestObjectFile(const std::string& canonicalPath,
                          ProjectManager::PathKind kind);

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
     * Stage a project to be loaded on the next render frame, with the
     * "Loading…" overlay painted in between so the user doesn't stare
     * at a blank window during deserialize. Used by the file-association
     * (CLI positional arg) entry point so it shares the same handshake
     * the launcher uses for its Open action. main.cpp calls this
     * before engine.run() instead of calling loadProject() directly.
     *
     * On load failure, the engine falls back to the launcher (same
     * recovery path as the previous synchronous call site).
     */
    void requestProjectLoad(const std::filesystem::path& filepath);

    /**
     * Off-thread FFmpeg metadata probe. Returns nullptr before
     * initialize() / after shutdown(). MediaBinWindow uses this to
     * fetch codec / resolution / framerate without blocking the
     * render thread on `avformat_open_input`.
     */
    MediaProbeWorker* probeWorker() { return m_probeWorker.get(); }
    const MediaProbeWorker* probeWorker() const { return m_probeWorker.get(); }

    /**
     * ADR-0009 — close the current project and return to the launcher.
     * Tears down physical output windows, joins in-flight transcodes,
     * clears the timeline (clips + tracks + sections + selection),
     * destroys project-scoped entities (OutputSurface, OutputDisplay,
     * Model, ClipDecodeState), drops undo/redo history, and clears
     * ProjectManager state. Then `showLauncher()` so the next render
     * shows Recent / New / Open. Safe to call when no project is open.
     */
    void closeProject();

    /**
     * Rebind RemotePatch components to RemoteControlStore slots after a
     * project load (storeSlot is runtime-only and not persisted). Duplicate
     * or invalid ids are auto-suffixed with a warning. Editor thread only.
     */
    void bindRemotePatchesAfterLoad();

    /**
     * Phase 11 — editor layout embed (item 3). captureEditorLayoutForSave()
     * snapshots WindowManager's ImGui ini + hidden-window set + lock state +
     * focused tabs into ProjectManager's editorLayout blob right before a
     * save; applyEditorLayoutAfterLoad() reads that blob back after a load and
     * hands it to WindowManager::requestIniApply (deferred to a frame
     * boundary) plus visibility/lock. Both are no-ops when WindowManager is
     * absent (pure headless) or the blob is empty (legacy files). Editor
     * thread only.
     */
    void captureEditorLayoutForSave();
    void applyEditorLayoutAfterLoad();

    /**
     * Save flow bound to Ctrl+S / "File > Save Project". Writes to the
     * current project path if one is set; otherwise prompts the user via
     * Save-As dialog.
     */
    void saveProjectInteractive();

    /**
     * Save flow bound to Ctrl+Shift+S / "File > Save Project As...". Always
     * prompts the user via Save-As dialog. Writes only the .entity file —
     * for Managed projects the result is an "orphan" showfile whose
     * Managed paths only resolve at the original project root. Use the
     * "Save Project As Bundle..." menu item (handled by the WindowManager
     * modal + ProjectManager::saveAsBundle) for a portable copy.
     *
     * Dialog is async (Issue #21); returns immediately after launching it.
     * The actual save lands on a subsequent frame when the picker completes.
     */
    void saveProjectAsInteractive();

    /**
     * Open flow bound to Ctrl+O / "File > Open Project...". Prompts the
     * user via Open dialog and loads the chosen project. Async — returns
     * after launching the dialog.
     */
    void openProjectInteractive();

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

    // Centered "Loading project... / <name>" ImGui overlay painted on
    // the launcher's frame and again on the dedicated handshake frame
    // before the blocking load runs. See Engine::render() launcher
    // branch for the two-frame handshake.
    void paintLauncherLoadingOverlay(const std::filesystem::path& path);

    // Modal popup shown during the post-load probe sweep so the user
    // sees explicit progress while MediaBin metadata fills in. Driven
    // by m_showProjectLoadModal + MediaProbeWorker counters; auto-
    // closes when pendingCount() drops to 0.
    void renderProjectLoadModal();

    // Bottom-right corner toast for live drops into content/ while the
    // editor is running. NoDecoration / NoInputs window so it doesn't
    // steal focus or block clicks. Only renders when the project-load
    // modal is NOT active and the worker has pending probes.
    void renderIndexingToast();

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
    // UI test harness (ImGui Test Engine). Only ever constructed on
    // ENTITY_ENABLE_UI_TESTS builds; null otherwise. Declared *before*
    // m_rendererService so implicit destruction (any path that skips
    // shutdown()) tears the renderer down first: the test-engine context
    // must be destroyed after ImGui::DestroyContext. Its ctor is trivial;
    // real startup is the explicit initialize() after renderer init.
    std::unique_ptr<UiTestHarness> m_uiTestHarness;

    std::unique_ptr<Renderer> m_rendererService;

    // Audio engine (Phase B): owns device + mixer + rate source. Stopped
    // in shutdown() before director teardown. Declared *before* m_director
    // so ~Engine destroys the Director first on any path that skips
    // shutdown(): ~Director runs AudioSystem::shutdown, whose MixSource
    // unregister dereferences the AudioSystem's raw AudioEngine pointer
    // (crash-logs 2026-07-04T14-02-*; shutdown() handles the ordering
    // explicitly, this declaration order covers everything else).
    std::unique_ptr<AudioEngine> m_audioEngine;

    // Phase D entry: Director owns Timeline, ProjectManager,
    // TranscodeManager, CommandDispatcher, AnimationSystem,
    // PlaybackTimeAuthority. Declared *before* the raw-pointer shortcuts
    // so destructor order tears the subsystems down (m_director last)
    // only after the shortcuts are no longer used.
    std::unique_ptr<Director> m_director;

    // Effect kind registry (issue #54). Owned by Engine so both the editor-
    // side bake (PlaybackTimeAuthority) and the show-side renderer
    // (D3D12Renderer's PSO cache) can read it through a stable pointer.
    // registerBuiltins is called once during initialize; user-effect
    // mutation (Phase 6) happens only on the editor thread.
    std::unique_ptr<effects::EffectKindRegistry> m_effectKindRegistry;

    // Live remote-control value plane (ADR-0028). Written by plugin
    // threads + editor UI; sampled lock-free by the show thread.
    std::unique_ptr<remote::RemoteControlStore> m_remoteControlStore;

    // Precomp definition library (ADR-0029 Decision 2). Engine-owned;
    // definition Timelines share m_registry and are never handed to
    // playback systems.
    std::unique_ptr<PrecompLibrary> m_precompLibrary;

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
    std::unique_ptr<ContentScanner>  m_contentScanner;
    std::unique_ptr<MediaProbeWorker> m_probeWorker;
    // Written on the editor thread (launcher enter/exit, project open),
    // read every tick by the show loop — which since issue #76 gates
    // output-window teardown on it (the launcher-idle branch destroys all
    // windows). Atomic so that dependency isn't riding on a torn/stale
    // plain-bool read.
    std::atomic<bool> m_showLauncher{false};

    // Two-frame handshake for project open from the launcher: when the
    // user picks Open, frame N stores the path here and paints the
    // overlay on top of the launcher. Frame N+1 detects the pending
    // path, paints the overlay alone, presents, then runs the blocking
    // load. Empty == no pending load.
    std::filesystem::path m_pendingLauncherLoad{};

    // File-association entry point (apps/editor/main.cpp). Same idea as
    // m_pendingLauncherLoad but for the path where there's no launcher
    // visible — main.cpp sets this via requestProjectLoad() before
    // engine.run(). Engine::render() runs a parallel two-frame handshake
    // (paint overlay, present, then load) and falls back to the
    // launcher on failure. m_fileAssocPhase tracks which leg of the
    // handshake we're on so render() knows whether to present-and-stage
    // or run the actual load.
    enum class FileAssocPhase { Idle, OverlayStaged };
    std::filesystem::path m_pendingFileAssociationLoad{};
    FileAssocPhase        m_fileAssocPhase{FileAssocPhase::Idle};

    // Set at the end of loadProject() if the probe worker has work
    // outstanding. Drives a blocking modal "Loading project — N/M
    // files" so the user knows the editor is still warming up. Cleared
    // when the worker drains. Live drops while the editor is running
    // use the toast path (see renderIndexingToast) instead.
    bool m_showProjectLoadModal{false};

    // #27 — drain ContentScanner deltas, dedupe against existing
    // library, call addMediaFile / mark-missing on the main thread.
    void drainContentScannerDeltas();

    // Send one InvalidateEffectPso per user kind the last effect scan
    // touched (project-load rescan + hot reload share this).
    void broadcastEffectPsoInvalidations();

    // ADR-0009 — current import mode + target subfolder. Default is Link
    // so legacy / script-driven flows keep their pre-launcher behavior;
    // MediaBin's toolbar flips it to Copy + "unsorted" on first render.
    // #32 — per-import storage mode is now decided per-file via the unified
    // modal (or via Settings.importStoragePolicy when the user has clicked
    // "don't ask again"). No persistent toolbar state on Engine anymore.

    // Raw shortcut into m_director->getCommandDispatcher().
    CommandDispatcher* m_commandDispatcher{nullptr};

    // Raw shortcut into m_director->getAnimationSystem(). Director owns
    // AnimationSystem (Phase D entry, subtask 5).
    AnimationSystem* m_animationSystem{nullptr};

    // Raw shortcut into m_director->getGenerativeSystem(). Director owns
    // GenerativeSystem (first generative layer kind: Muncher).
    GenerativeSystem* m_generativeSystem{nullptr};

    // Raw shortcut into m_director->getRoutingLibrarySystem(). Director
    // owns the system; Engine ticks it each editor frame to reconcile
    // ContentRoutingAsset auto-direct entries against the Screen set
    // (ADR-0022).
    RoutingLibrarySystem* m_routingLibrarySystem{nullptr};

    // Raw shortcut into m_director->getTextSystem(). Director owns the
    // system; Engine ticks it each editor frame to rasterize dirty
    // TextLayerState components into video-pool slots.
    TextSystem* m_textSystem{nullptr};

    // Engine-owned input channel bus. Constructed in initialize(),
    // destroyed in shutdown(). Plumbed into GenerativeSystem so the
    // Muncher kind can read its X/Y axes each editor tick.
    std::unique_ptr<InputBus> m_inputBus;

    // True while at least one Muncher movement key is held. Used by the
    // per-tick keyboard poller to know when to stop writing
    // `muncher.input.*` channels (so scripts / OSC / MIDI can drive them
    // without the keyboard zeroing them out every tick).
    bool m_muncherKeyboardActive{false};

    // Machine-global settings (frame-cache budget etc). Loaded from
    // settingsPath() at construction; persisted by saveSettings() whenever
    // the user clicks OK in the Preferences dialog. Per-project state lives
    // in ProjectManager / .entity files; this is the other side of that line.
    Settings m_settings{};

    // Non-owning ref to the active TimelineWidget. Used by the Edit menu and
    // keyboard handler to read the current range selection. Owned by the
    // WindowManager via TimelineWindow.
    class TimelineWidget* m_timelineWidget{nullptr};

    // Single-slot in-process clipboard for clip Cut/Copy/Paste. See
    // ClipClipboard.hpp for the snapshot shape. Cleared on closeProject
    // and loadProject because routing-asset entity refs only survive
    // within one project session.
    std::vector<ClipClipboardSnapshot> m_clipClipboard;

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
    SectionScheduler*      m_sectionScheduler{nullptr};
    PlaybackPresenter*     m_playbackPresenter{nullptr};
    AudioSystem*           m_audioSystem{nullptr};

    // Phase C (seek-sync): polls video + audio worker readiness each editor
    // tick and releases the Timeline seek-sync gate once all active decoders
    // have prerolled to the parked frame.  Constructed in initialize(); never
    // null after that point.
    std::unique_ptr<SeekSyncController> m_seekSyncController;

    // Phase D entry, subtask 8: Director->Renderer per-tick state-snapshot
    // travels through this transport. In-process today (no threads, no
    // UDP); Phase E swaps the implementation without touching the message
    // format. Engine builds the message Director-side, drains it
    // Renderer-side inside render() right after beginFrame().
    std::unique_ptr<bus::IMessageTransport> m_transport;

    // Plugin shutdown hooks. Fired at the top of Engine::shutdown() so
    // plugins can join worker threads before bus/dispatcher tear down.
    // Populated by EnginePluginContext::registerShutdownHook().
    std::vector<void(*)()> m_pluginShutdownHooks;

    // Non-owning pointer to the active EnginePluginContext. Set by
    // setPluginContext() after registerStaticPlugins() in main.cpp; valid
    // until Engine::shutdown() clears it. Used by postSignalEmit() so the
    // show thread (Phase 6) and script commands (Phase 2 verification) can
    // push signal events without knowing the concrete context type.
    entity::core::EnginePluginContext* m_pluginCtx{nullptr};

    // Show-thread Signal Output System. Owned here so prevFrame tracking and
    // armed/rate-limit state survive across ticks. ADR-0014: no registry
    // reads/writes; pure snapshot consumer.
    SignalOutputSystem m_signalOutputSystem;

    // When true, showThreadMain resets m_signalOutputSystem at the top of its
    // next evaluate() call. Set from the editor thread on seek/load/stop.
    // Atomic so the editor thread can write it without holding the show mutex.
    std::atomic<bool> m_signalResetPending{false};

    // Reusable per-tick output buffer for SignalOutputSystem::evaluate().
    // Hoisted out of showThreadMain so it retains capacity across ticks —
    // zero allocation in steady state when signal layers are active.
    // Accessed only from the show thread.
    std::vector<bus::SignalEmit> m_signalEmitsBuffer;

    // FPS display tracking (window title only)
    double m_fpsAccumulator{0.0};
    uint32_t m_fpsFrameCount{0};
    uint32_t m_currentFPS{0};

    // Editor-thread deltaTime (seconds, wall clock). Updated at the top of
    // each Engine::run iteration; used by Engine::update() (SectionScheduler
    // tick, etc.). Independent from PlaybackTimeAuthority's deltaTime, which
    // the show thread owns.
    double m_editorDeltaTime{0.0};

    // Heartbeat for the show thread's "is the editor stalled?" check.
    // Engine::update() stamps this with steady_clock::now(); the show thread
    // reads it and falls back to ticking Timeline itself when the gap exceeds
    // a threshold (interactive Win32 resize, modal dialog, slow load, ...).
    // steady_clock::time_point underlying type is signed integral; using its
    // count() with std::atomic<int64_t> works on every platform we ship.
    std::atomic<int64_t> m_lastEditorTickNs{0};

    // Issue #74 — nonzero while the editor thread is inside a bulk registry
    // mutation (loadProject / closeProject), set via RegistryMutationScope.
    // The show thread's stall fallback checks it alongside heartbeat
    // staleness: a synchronous load IS a >50 ms stall, so staleness alone
    // can't distinguish "editor pinned in a modal loop" from "editor
    // actively rewriting the registry". The fallback is registry-free, so
    // this is not a safety gate — it suppresses wasted steering of doomed
    // workers (stale-catalog targets competing with load I/O) and closes
    // the theoretical entity-id version-wrap alias between the old
    // project's cached catalog and a new project's workers. Counter (not
    // bool) so nested scopes compose.
    std::atomic<int> m_bulkRegistryMutation{0};

    // RAII marker for bulk registry mutation on the editor thread. Wrap any
    // code path that destroys/clears/emplaces registry state en masse so
    // new bulk-mutation sites can't forget the flag. acq_rel on the counter
    // RMWs so the flag transition can't reorder against the mutation work
    // on either side of the scope. The destructor also refreshes the
    // heartbeat: the scope's whole reason to exist is a long editor
    // operation, so on exit the heartbeat (stamped at iteration top,
    // pre-mutation) is guaranteed stale — without the refresh the fallback
    // would re-engage against the not-yet-republished (old-project)
    // snapshot for the remainder of the editor iteration.
    class RegistryMutationScope {
    public:
        explicit RegistryMutationScope(Engine& e) : m_engine(e) {
            m_engine.m_bulkRegistryMutation.fetch_add(1, std::memory_order_acq_rel);
        }
        ~RegistryMutationScope() {
            m_engine.stampEditorHeartbeat();
            m_engine.m_bulkRegistryMutation.fetch_sub(1, std::memory_order_acq_rel);
        }
        RegistryMutationScope(const RegistryMutationScope&) = delete;
        RegistryMutationScope& operator=(const RegistryMutationScope&) = delete;
    private:
        Engine& m_engine;
    };

    // Store steady_clock-now into m_lastEditorTickNs. Called at the top of
    // each editor iteration, again just before Engine::update() (so the
    // show thread doesn't treat a healthy-but-slow iteration's tail as a
    // stall and steer workers concurrently with the editor's own
    // DecodeSystem/AudioSystem tick), and by RegistryMutationScope on exit.
    void stampEditorHeartbeat();

    // Stage 3: Show thread. Owns Director tick, RenderFrame production/
    // consumption, CompositorSystem, OutputManager, and projector Present.
    // Editor thread keeps GLFW + ImGui + editor swap chain. This split means
    // Win32 modal loops (title-bar drag, resize) cannot stall the show.
    std::thread           m_showThread;
    std::atomic<bool>     m_showStopRequested{false};
    // Incremented once per show tick. Integration tests read this to assert
    // the show thread advanced while the editor thread was stalled.
    std::atomic<uint64_t> m_showFrameCount{0};

    // Cached scene snapshot for the show thread. Editor half of
    // buildRenderFrame publishes via D2R; show thread merges it in.
    bus::SceneSnapshot    m_cachedSceneSnapshot;

    // EnTT signal: fires before a Model component is removed so we can
    // schedule deferred GPU slot release before the component data is gone.
    void onModelDestroyed(entt::registry& reg, entt::entity e);

    // EnTT signal: fires before a VideoTexture component is removed (clip
    // deleted, project cleared, etc.). Schedules a deferred GPU slot release
    // via IRenderer::scheduleVideoTextureSlotFree so the show thread can drain
    // it at beginShowFrame after the fence wait — no editor-thread GPU stall.
    void onVideoTextureDestroyed(entt::registry& reg, entt::entity e);

    // The show thread's body.
    void showThreadMain();

public:
    // Load-bearing for mesh upload pacing — see Engine.cpp update() where the
    // cold-start gate defers all Model uploads until this returns >= 1.
    uint64_t getShowFrameCount() const { return m_showFrameCount.load(std::memory_order_relaxed); }

    // Total successful mesh uploads since construction. Used by integration
    // tests (mesh_upload_paces.json) to verify the cold-start gate and
    // steady-state one-upload-per-tick cap.
    uint64_t getMeshUploadCount() const;

    // Device-lost auto-relaunch. Set from the bus::DeviceLost R2D drain after
    // autosave completes. main.cpp checks these after engine.shutdown() to
    // decide whether to spawn a recovery process.
    bool shouldRelaunch() const { return m_relaunchRequested; }
    std::filesystem::path relaunchProjectPath() const { return m_relaunchProjectPath; }

    // Mark this process as a recovery relaunch (started via --device-lost-recovery).
    // Suppresses setting m_relaunchRequested on a subsequent device-loss so we
    // don't chain relaunches in a device-lost storm.
    void setRecoveryRelaunch() { m_isRecoveryRelaunch = true; }

    /**
     * Create an Object Animation layer on the given track at the given start
     * frame and duration. Targets the provided screen entity (entt::null means
     * no target assigned — user picks one in the Properties panel).
     * Returns the created entity, or entt::null on failure.
     */
    entt::entity createObjectAnimationLayer(entt::entity targetScreen,
                                            int trackIndex,
                                            FrameNumber startFrame,
                                            FrameNumber duration);

    /**
     * Create a Signal Output layer on the given track at the given start frame
     * and duration. Archetype: Layer (Kind::Signal) + SignalLayer +
     * AnimatedProperties. Deliberately NO MediaLayer, NO Transform — not a
     * content layer. Returns the created entity, or entt::null on failure.
     */
    entt::entity createSignalLayer(int trackIndex,
                                   FrameNumber startFrame,
                                   FrameNumber duration);

    /**
     * Create a Clip layer entity with no media (filepath empty). Used when the
     * user drags a Clip kind from the Layers panel onto the timeline — media is
     * assigned later via the Properties panel or drag-and-drop.
     * Returns the created entity, or entt::null on failure.
     */
    entt::entity createEmptyClipLayer(int trackIndex,
                                      FrameNumber startFrame,
                                      FrameNumber duration);

    /**
     * Create a Generative layer entity of the "Muncher" kind. The layer is
     * attached to `trackIndex` at `startFrame` with `duration` timeline frames.
     * `targetScreen` may be `entt::null`; the command path picks the first
     * available Screen at creation time. Returns the new entity, or
     * `entt::null` on failure.
     */
    entt::entity createMuncherLayer(entt::entity targetScreen,
                                    int trackIndex,
                                    FrameNumber startFrame,
                                    FrameNumber duration);

    entt::entity createTextLayer(entt::entity targetScreen,
                                 int trackIndex,
                                 FrameNumber startFrame,
                                 FrameNumber duration);

    entt::entity createSolidLayer(entt::entity targetScreen,
                                  int trackIndex,
                                  FrameNumber startFrame,
                                  FrameNumber duration);

    // --- Clipboard + clip clone (timeline ergonomics) -----------------
    //
    // Unified deep-clone path for Duplicate, Cut/Copy/Paste, and any
    // future "make an independent copy of a clip" surface. The split
    // helpers exist so Paste can reuse `materialize` against a
    // clipboard snapshot without re-walking the source registry.

    /**
     * Capture a clip's full archetype into a typed clipboard snapshot:
     * Layer placement, Clip media metadata, Transform, MediaLayer,
     * VideoTexture markers, AnimatedProperties, ContentRoutingRef,
     * EffectChain + each referenced Effect entity. Returns an invalid
     * snapshot (valid==false) if `src` isn't a clip on any track.
     */
    ClipClipboardSnapshot snapshotClipForClipboard(entt::entity src) const;

    /**
     * Rebuild a clip from a snapshot on `dstTrackIndex` at
     * `dstStartFrame`. Deep-clones the EffectChain (new Effect entities
     * per node) and remaps EffectConnection / outputNode through an
     * oldToNew map. ContentRoutingRef::asset is reused as-is (shared
     * library asset by design — ADR-0022). Calls `onClipCreated` so the
     * fresh entity gets its decoder + GPU slot. Returns the new entity,
     * or entt::null on failure (invalid snapshot, track out of range).
     */
    entt::entity materializeClipFromSnapshot(const ClipClipboardSnapshot& snap,
                                              FrameNumber dstStartFrame,
                                              int         dstTrackIndex);

    /**
     * End-to-end: snapshot src, then materialize at the destination.
     * Used by DuplicateClipCommand (parity-fixed Ctrl+D).
     */
    entt::entity cloneClipEntity(entt::entity src,
                                  FrameNumber  dstStartFrame,
                                  int          dstTrackIndex);

    /**
     * Single-slot in-process clipboard for CopyClipCommand / CutClipCommand /
     * PasteClipCommand. Cleared on closeProject / loadProject because the
     * routing-asset entity reference only survives within one project
     * session.
     */
    // Phase 13: the clipboard is a group (vector) of snapshots so a
    // multi-selection copy/paste round-trips. A single-clip copy is a
    // one-element vector. Empty vector == empty clipboard.
    const std::vector<ClipClipboardSnapshot>& clipClipboard() const {
        return m_clipClipboard;
    }
    bool hasClipClipboard() const { return !m_clipClipboard.empty(); }
    void setClipClipboard(std::vector<ClipClipboardSnapshot> snaps) {
        m_clipClipboard = std::move(snaps);
    }
    // Convenience for the single-clip copy/cut path.
    void setClipClipboard(ClipClipboardSnapshot snap) {
        m_clipClipboard.clear();
        m_clipClipboard.push_back(std::move(snap));
    }
    void clearClipClipboard() { m_clipClipboard.clear(); }

    /**
     * Tear down the decode worker for a clip. Forwards to
     * DecodeSystem::destroyClipWorker; safe to call when no worker
     * exists. Used by SetClipMediaCommand to swap media synchronously.
     */
    void destroyClipWorker(entt::entity clipEntity);

private:

    // State
    bool m_initialized{false};
    bool m_running{false};
    bool m_resizePending{false};
    // #91: deadline for retrying a deferred (NotReady) renderer resize. Each
    // failed attempt can block up to 3x the fence timeout, so this bounds
    // ATTEMPTS not total wall time — worst case is deadline + one overshooting
    // attempt. (Re)armed only in onWindowResize when a new resize is requested.
    std::chrono::steady_clock::time_point m_resizeRetryDeadline{};
    bool m_deviceLostPosted{false};  // Guard: post bus::DeviceLost at most once.
    bool m_relaunchRequested{false};         // Set on device-lost drain; checked by main.cpp.
    bool m_isRecoveryRelaunch{false};        // True when started via --device-lost-recovery; suppresses re-relaunch.
    std::filesystem::path m_relaunchProjectPath;  // Autosave path to load in the recovery process.
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

    // Set when resolvePendingImport notices the chosen Copy target
    // collides with an existing file. MediaBinWindow renders the
    // file-already-exists modal; resolvePendingMediaDuplicate /
    // cancelPendingMediaDuplicate clear it.
    std::optional<PendingMediaDuplicate> m_pendingMediaDuplicate;

    // Set when onModelFileSelected sees Settings::importStoragePolicy = Ask.
    // ModelBinWindow renders the unified Copy/Link modal;
    // resolvePendingObjectImport() clears it. Same single-pending-decision
    // pattern as PendingImport — additional drops while one is pending are
    // dropped with a log line.
    std::optional<PendingObjectImport> m_pendingObjectImport;

    // Set when resolvePendingObjectImport notices the chosen Copy target
    // collides with an existing file. ModelBinWindow renders the second
    // modal (Replace / Keep both / Cancel); resolvePendingObjectDuplicate
    // or cancelPendingObjectDuplicate clears it.
    std::optional<PendingObjectDuplicate> m_pendingObjectDuplicate;

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

    // Per-tick metadata-probe drain. Copies freshly-completed
    // MediaProbeWorker results (and the size+mtime captured at probe
    // time) back into MediaLibraryEntry so the next save persists the
    // probe cache. Skipping the FFmpeg open on next reopen is the
    // whole point of v12's project schema.
    void pollProbeCompletions();

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
