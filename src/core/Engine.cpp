#include "entity/core/Engine.hpp"
#include "entity/bus/InMemoryMessageTransport.hpp"
#include "entity/bus/Message.hpp"
#include "entity/bus/Serialization.hpp"
#include "entity/director/CaptureBroker.hpp"
#include "entity/director/Director.hpp"
#include "entity/renderer/Renderer.hpp"
#include "entity/director/PlaybackTimeAuthority.hpp"
#include "entity/renderer/PlaybackPresenter.hpp"
#include "entity/render/IRenderer.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/ui/WindowManager.hpp"
#include "entity/ui/ProjectLauncher.hpp"
#include "entity/project/RecentProjects.hpp"
#include "entity/ui/TimelineWindow.hpp"
#include "entity/ui/StageWindow.hpp"
#include "entity/ui/MediaBinWindow.hpp"
#include "entity/ui/PropertyWindow.hpp"
#include "entity/ui/MappingWindow.hpp"
#include "entity/ui/ModelBinWindow.hpp"
#include "entity/ui/ScreensWindow.hpp"
#include "entity/systems/TimelineSystem.hpp"
#include "entity/systems/CompositorSystem.hpp"
#include "entity/systems/AnimationSystem.hpp"
#include "entity/systems/DecodeSystem.hpp"
#include "entity/media/Decoder.hpp"
#include "entity/media/DecodedFrame.hpp"
#include "entity/color/OcioManager.hpp"
#include "entity/media/FrameCache.hpp"
#include "entity/project/ProjectSerializer.hpp"
#include "entity/project/ProjectManager.hpp"
#include "entity/command/CommandDispatcher.hpp"
#include "entity/command/Commands.hpp"
#include "entity/render/OutputManager.hpp"
#include <imgui.h>
#include "entity/components/Transform.hpp"
#include "entity/components/MediaLayer.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/ClipDecodeState.hpp"
#include "entity/components/VideoTexture.hpp"
#include "entity/components/FrameBuffer.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/components/OutputMapping.hpp"
#include "entity/components/Screen.hpp"
#include "entity/components/Model.hpp"
#include "entity/media/ObjLoader.hpp"
#include <GLFW/glfw3.h>
#include <cmath>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include <iostream>
#include <fstream>
#include <filesystem>
#include <cassert>
#include <algorithm>
#include <cstdio>

namespace entity {

// Static callback for GLFW framebuffer resize events
static void framebufferResizeCallback(GLFWwindow* window, int width, int height) {
    // Get Engine instance from GLFW window user pointer
    Engine* engine = static_cast<Engine*>(glfwGetWindowUserPointer(window));
    if (engine) {
        engine->onWindowResize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    }
}

// Static callback for GLFW key events
static void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    // Get Engine instance from GLFW window user pointer
    Engine* engine = static_cast<Engine*>(glfwGetWindowUserPointer(window));
    if (engine) {
        engine->onKeyEvent(key, scancode, action, mods);
    }
}

Engine::Engine() {
    // Constructor
}

Engine::~Engine() {
    shutdown();
}

Result Engine::initialize(uint32_t windowWidth, uint32_t windowHeight, const char* windowTitle, bool headless) {
    if (m_initialized) {
        std::cerr << "Engine already initialized!" << std::endl;
        return Result::Failure;
    }

    std::cout << "Initializing Entity Media Server Engine..." << std::endl;

    m_windowWidth = windowWidth;
    m_windowHeight = windowHeight;

    // Initialize GLFW
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW!" << std::endl;
        return Result::Failure;
    }

    // Create window (no OpenGL context)
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    if (headless) {
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        std::cout << "  Headless mode: window hidden" << std::endl;
    }

    m_window = glfwCreateWindow(
        static_cast<int>(windowWidth),
        static_cast<int>(windowHeight),
        windowTitle,
        nullptr,
        nullptr
    );

    if (!m_window) {
        std::cerr << "Failed to create GLFW window!" << std::endl;
        glfwTerminate();
        return Result::Failure;
    }

    std::cout << "  Window created: " << windowWidth << "x" << windowHeight << std::endl;

    // Set window user pointer to this Engine instance for callbacks
    glfwSetWindowUserPointer(m_window, this);

    // Set framebuffer resize callback
    glfwSetFramebufferSizeCallback(m_window, framebufferResizeCallback);

    // Set keyboard input callback
    glfwSetKeyCallback(m_window, keyCallback);

    // Load machine-global settings up front -- the Renderer service sizes
    // its FrameCache from m_settings.frameCacheBytes, and the Preferences
    // dialog (wired below) reads the loaded values to populate its initial
    // state.
    m_settings = loadSettings();
    publishActiveSettings(m_settings);
    std::cout << "  Settings loaded: frameCacheBytes="
              << (m_settings.frameCacheBytes / (1024ull * 1024ull)) << " MiB"
              << " from " << reinterpret_cast<const char*>(settingsPath().u8string().c_str())
              << std::endl;

    // Phase D entry: Renderer service owns the GPU stack -- D3D12Renderer,
    // OutputManager, FrameCache, OcioManager, CompositorSystem,
    // DecodeSystem, TestSystem. Construction order inside mirrors what
    // Engine used to do step-by-step.
    m_rendererService = std::make_unique<Renderer>(m_registry, m_sceneState);
    if (Result r = m_rendererService->initialize(m_window, windowWidth, windowHeight,
                                                 static_cast<size_t>(m_settings.frameCacheBytes));
        r != Result::Success) {
        std::cerr << "Failed to initialize Renderer service!" << std::endl;
        return r;
    }
    m_renderer       = m_rendererService->getRenderer();
    m_outputManager  = m_rendererService->getOutputManager();
    m_frameCache     = m_rendererService->getFrameCache();
    m_ocioManager    = m_rendererService->getOcioManager();
    m_decodeSystem   = m_rendererService->getDecodeSystem();
    std::cout << "  Renderer service initialized (D3D12 + OutputManager + "
                 "FrameCache + OcioManager + Compositor + Decode)" << std::endl;

    // Phase D entry: Director owns Timeline, ProjectManager,
    // TranscodeManager, CommandDispatcher, AnimationSystem,
    // PlaybackTimeAuthority. Constructed here -- after the renderer (so
    // ProjectManager::initialize can route through it for Screen
    // render-target allocation). The raw shortcuts below let existing
    // Engine code continue to use m_timeline / m_projectManager /
    // m_transcodeManager / m_commandDispatcher / m_animationSystem /
    // m_timeAuthority unchanged.
    m_director = std::make_unique<Director>(m_registry, m_sceneState, m_renderer);
    m_timeline          = m_director->getTimeline();
    m_projectManager    = m_director->getProjectManager();
    m_transcodeManager  = m_director->getTranscodeManager();
    m_commandDispatcher = m_director->getCommandDispatcher();
    m_animationSystem   = m_director->getAnimationSystem();
    m_timeAuthority     = m_director->getTimeAuthority();
    std::cout << "  Director initialized (Timeline + ProjectManager + "
                 "TranscodeManager + CommandDispatcher + AnimationSystem + "
                 "PlaybackTimeAuthority)" << std::endl;

    // Cross-side wiring -- the Director side owns Timeline; the
    // Renderer-side systems need a raw read-only pointer to it. Subtask 8
    // replaces this with the per-tick RenderFrame bus message.
    if (auto* compositor = m_rendererService->getCompositorSystem()) {
        compositor->setTimeline(m_timeline);
        compositor->setDebugLogging(false);
    }
    if (m_decodeSystem) {
        m_decodeSystem->setTimeline(m_timeline);
    }

    // PlaybackPresenter (Renderer side, owned by Renderer service)
    // consumes a `bus::RenderFrame` per tick -- playState arrives on the
    // message body, so no Timeline pointer is wired here.
    m_playbackPresenter = m_rendererService->getPlaybackPresenter();

    // Phase D entry, subtask 8: Director->Renderer per-tick state-snapshot
    // travels through this transport. In-process today; Phase E swaps the
    // implementation without touching the wire format.
    m_transport = std::make_unique<bus::InMemoryMessageTransport>();

    // Phase D entry, subtask 7: capture-command request/reply broker on
    // the Director side needs both the transport (to publish requests)
    // and the dispatcher (to resolve script-results). Dispatcher is wired
    // inside Director's ctor; transport is wired here, after the
    // transport instance exists.
    if (auto* broker = m_director->getCaptureBroker()) {
        broker->setTransport(m_transport.get());
    }

    // Background HAP transcoder cache dir gets set lazily via
    // updateTranscodeCacheDir() when a project path is established.
    updateTranscodeCacheDir();

    // Set up callback for when new clips are created (split, duplicate)
    m_timeline->setClipCreatedCallback([this](entt::entity clipEntity, const std::string& filepath) {
        this->onClipCreated(clipEntity, filepath);
    });

    // Initialize window manager
    m_windowManager = std::make_unique<WindowManager>();
    m_windowManager->initialize();

    // Lend OcioManager to the Preferences dialog so its Color section can
    // populate color-space / display / view dropdowns from the active OCIO
    // config. Renderer service owns OcioManager -- it outlives the
    // WindowManager.
    if (m_ocioManager) {
        m_windowManager->setOcioManager(m_ocioManager);
    }

    // Parent native modal dialogs (Save/Open) to our GLFW window. Must run
    // in --headless too: glfwGetWin32Window on the hidden window is fine,
    // and dialogs are only opened via UI paths that never fire in headless.
#ifdef _WIN32
    if (m_window) {
        m_windowManager->setOwnerWindow(glfwGetWin32Window(m_window));
    }
#endif

    // Set up video file callback
    m_windowManager->setVideoFileCallback([this](const std::string& filePath) {
        this->onVideoFileSelected(filePath);
    });

    // Set up project save/load callbacks
    m_windowManager->setSaveProjectCallback([this]() {
        this->saveProjectInteractive();
    });
    m_windowManager->setSaveProjectAsCallback([this]() {
        this->saveProjectAsInteractive();
    });
    m_windowManager->setOpenProjectCallback([this](const std::string& filePath) {
        this->loadProject(filePath);
    });

    // Set up exit callback
    m_windowManager->setExitCallback([this]() {
        this->requestExit();
    });

    // Set up run script callback
    m_windowManager->setRunScriptCallback([this](const std::string& filePath) {
        this->runScript(filePath);
    });

    // ADR-0009 — File > Collect Linked Media...
    m_windowManager->setCanCollectMediaCallback([this]() {
        return countLinkedEntries() > 0;
    });
    m_windowManager->setCollectMediaCallback([this]() {
        const int n = collectLinkedIntoProject();
        std::cout << "[Engine] Collect ran: " << n << " entries collected." << std::endl;
    });

    // ADR-0009 — File > Save Project As Bundle... (modal in WindowManager).
    m_windowManager->setCurrentProjectInfoCallback([this]() {
        std::string parent, name;
        if (m_projectManager && !m_projectManager->projectPath().empty()) {
            const auto& p = m_projectManager->projectPath();
            parent = p.parent_path().parent_path().string();  // grandparent (the bundle's typical parentDir)
            name   = p.parent_path().filename().string();     // current root dir name
        }
        return std::make_pair(parent, name);
    });
    m_windowManager->setSaveBundleCallback(
        [this](const std::string& parentDir, const std::string& name,
               std::string* errorOut) -> bool {
            if (!m_projectManager) {
                if (errorOut) *errorOut = "Project subsystem not initialized.";
                return false;
            }
            if (!m_projectManager->saveAsBundle(parentDir, name)) {
                if (errorOut) {
                    *errorOut = "Bundle save failed (see console for details).";
                }
                return false;
            }
            // Touch Recent so the bundled copy shows up in the launcher's
            // Recent panel — it's the new "current project" the editor is
            // working in.
            if (m_recentProjects) {
                m_recentProjects->touch(m_projectManager->projectPath().string());
                m_recentProjects->save();
            }
            return true;
        });

    // ADR-0009 — File > Rebuild Project Structure / Find Missing Media...
    m_windowManager->setRebuildStructureCallback([this]() {
        return rebuildProjectStructure();
    });
    m_windowManager->setFindMissingMediaCallback([this](const std::string& dir) {
        return findMissingMedia(dir);
    });

    // Register windows with window manager
    m_windowManager->registerWindow(std::make_unique<MediaBinWindow>(this));

    // Create and configure TimelineWindow
    auto timelineWindow = std::make_unique<TimelineWindow>(m_timeline);
    m_timelineWidget = timelineWindow->getWidget();
    TimelineWidget* timelineWidget = m_timelineWidget;
    if (timelineWidget) {
        // Set up callback for media dropped onto timeline tracks
        timelineWidget->setMediaDropCallback([this](const std::string& filepath, int trackIndex, Timecode position) {
            this->onMediaDroppedOnTimeline(filepath, trackIndex, position);
        });
    }
    m_windowManager->registerWindow(std::move(timelineWindow));

    // Edit menu wiring — undo/redo + ripple ops bridged through CommandDispatcher.
    // TimelineWidget owns the range selection; CommandDispatcher owns the undo
    // stack. Engine just routes between them.
    m_windowManager->setHasRangeSelectionCallback([timelineWidget]() {
        return timelineWidget && timelineWidget->hasRangeSelection();
    });
    m_windowManager->setRippleInsertCallback([this, timelineWidget]() {
        if (!timelineWidget || !timelineWidget->hasRangeSelection()) return;
        FrameNumber startF = m_timeline->timeToFrame(timelineWidget->getRangeStart());
        FrameNumber endF   = m_timeline->timeToFrame(timelineWidget->getRangeEnd());
        FrameNumber dur = endF - startF;
        if (dur <= 0) return;
        m_commandDispatcher->enqueue(std::make_unique<RippleInsertTimeCommand>(startF, dur));
        timelineWidget->clearRangeSelection();
    });
    m_windowManager->setRippleDeleteCallback([this, timelineWidget]() {
        if (!timelineWidget || !timelineWidget->hasRangeSelection()) return;
        FrameNumber startF = m_timeline->timeToFrame(timelineWidget->getRangeStart());
        FrameNumber endF   = m_timeline->timeToFrame(timelineWidget->getRangeEnd());
        if (endF <= startF) return;
        m_commandDispatcher->enqueue(std::make_unique<RippleDeleteTimeCommand>(startF, endF));
        timelineWidget->clearRangeSelection();
    });
    m_windowManager->setUndoCallback([this]() { m_commandDispatcher->undo(*this); });
    m_windowManager->setRedoCallback([this]() { m_commandDispatcher->redo(*this); });
    m_windowManager->setCanUndoCallback([this]() { return m_commandDispatcher->getUndoDepth() > 0; });
    m_windowManager->setCanRedoCallback([this]() { return m_commandDispatcher->getRedoDepth() > 0; });

    // Preferences dialog — reads from m_settings on open; OK persists +
    // re-budgets the live FrameCache so the user gets the new limit
    // without a restart (shrinks evict LRU immediately, grows take effect
    // on the next put).
    m_windowManager->setCurrentSettingsCallback([this]() { return m_settings; });
    m_windowManager->setSettingsAppliedCallback([this](const Settings& updated) {
        m_settings = updated;
        publishActiveSettings(m_settings);
        // Renderer-side state (FrameCache budget, OCIO config path) flows
        // through the bus -- Engine no longer reaches into m_frameCache
        // from the UI callback.
        publishApplySettings(bus::ApplySettings{
            m_settings.frameCacheBytes,
            m_settings.ocioConfigPath});
        if (!saveSettings(m_settings)) {
            std::cerr << "[Engine] Could not persist settings to disk; "
                         "in-memory values are still updated." << std::endl;
        } else {
            std::cout << "[Engine] Settings saved (frameCacheBytes="
                      << (m_settings.frameCacheBytes / (1024ull * 1024ull)) << " MiB)"
                      << std::endl;
        }
    });

    m_windowManager->registerWindow(std::make_unique<StageWindow>(this));
    {
        auto propertyWindow = std::make_unique<PropertyWindow>(m_timeline);
        propertyWindow->setCommandDispatcher(m_commandDispatcher);
        m_windowManager->registerWindow(std::move(propertyWindow));
    }
    m_windowManager->registerWindow(std::make_unique<MappingWindow>(this));
    m_windowManager->registerWindow(std::make_unique<ModelBinWindow>(this, m_windowManager.get()));
    m_windowManager->registerWindow(std::make_unique<ScreensWindow>(this));

    // Create 5 empty tracks as default
    for (int i = 1; i <= 5; ++i) {
        m_timeline->createTrack("Video Track " + std::to_string(i));
    }
    std::cout << "Created 5 initial empty tracks" << std::endl;

    // Create default screen model and screen
    createDefaultScreen();

    // TODO: Initialize transport
    // m_transport = std::make_unique<Transport>();

    // Initialize timing -- authority is Director-side; Engine just
    // forwards startTiming + the per-tick updateTiming/incrementFrameCount.
    m_timeAuthority->startTiming();

    // ADR-0009 — Project Launcher subsystem. Constructed unconditionally so
    // both the no-arg launch (which calls showLauncher()) and the legacy
    // script path (which doesn't) share the same wiring. The recent-
    // projects file is loaded eagerly so the launcher's first render has
    // populated content; missing/corrupt files load as empty (see
    // RecentProjects).
    m_recentProjects = std::make_unique<RecentProjects>();
    m_recentProjects->load();
    m_launcher = std::make_unique<ProjectLauncher>();
    m_launcher->initialize(m_projectManager, m_recentProjects.get(), m_window);

    m_initialized = true;

    // Run component tests (Phase 3)
    testComponents();

    // Create test entities for rendering (Phase 4.1)
    // NOTE: Disabled - test entities obscure video content
    // createTestEntities();

    std::cout << "Engine initialized successfully!" << std::endl;

    return Result::Success;
}

void Engine::shutdown() {
    if (!m_initialized) {
        return;
    }

    std::cout << "Shutting down engine..." << std::endl;

    // Stop running if still active
    m_running = false;

    // Cancel + join any in-flight transcode workers before we tear down
    // FFmpeg state or systems they might be touching.
    if (m_transcodeManager) {
        m_transcodeManager->joinAll();
    }

    // Tear down Renderer-service-owned subsystems explicitly. Order inside
    // Renderer::shutdown(): PlaybackPresenter -> DecodeSystem (joins
    // worker threads) -> TestSystem -> CompositorSystem -> OutputManager
    // (releases swap-chain slots) -> OcioManager -> FrameCache ->
    // D3D12Renderer.
    m_playbackPresenter = nullptr;
    m_decodeSystem  = nullptr;
    m_ocioManager   = nullptr;
    m_frameCache    = nullptr;
    m_outputManager = nullptr;
    m_renderer      = nullptr;
    m_rendererService.reset();

    // ADR-0009 — Launcher reads ProjectManager + RecentProjects, so it
    // must be torn down before the Director (which owns ProjectManager).
    m_launcher.reset();
    m_recentProjects.reset();
    m_showLauncher = false;

    // Tear down Director-owned subsystems (Timeline, ProjectManager,
    // TranscodeManager, CommandDispatcher, AnimationSystem,
    // PlaybackTimeAuthority) explicitly so the raw shortcut pointers
    // stop being live before m_initialized is cleared. (Member
    // destruction order would also do this; doing it here surfaces
    // order bugs early instead of at process exit.)
    m_timeAuthority      = nullptr;
    m_animationSystem    = nullptr;
    m_commandDispatcher  = nullptr;
    m_transcodeManager   = nullptr;
    m_projectManager     = nullptr;
    m_timeline           = nullptr;
    m_director.reset();

    // Destroy window
    if (m_window) {
        glfwDestroyWindow(m_window);
        m_window = nullptr;
    }

    // Terminate GLFW
    glfwTerminate();

    m_initialized = false;
    std::cout << "Engine shutdown complete." << std::endl;
}

void Engine::showLauncher() {
    if (!m_initialized) {
        std::cerr << "[Engine] showLauncher() before initialize() — ignored." << std::endl;
        return;
    }
    if (!m_launcher) return;  // Should never happen; defensive.
    m_launcher->reset();
    m_showLauncher = true;
}

void Engine::onLauncherOpenProject(const std::filesystem::path& path) {
    if (path.empty()) return;

    if (loadProject(path)) {
        // loadProject() already bumped the Recent list — see the touch at
        // the bottom of that method. Just leave launcher mode.
        m_showLauncher = false;
    } else {
        std::cerr << "[Engine] Project failed to load from launcher: "
                  << path.string() << std::endl;
        // Treat a failed load as a stale Recent entry: drop it so the user
        // doesn't keep clicking a broken row. They can re-open via "Open
        // Project..." if the file recovers. Stay in launcher mode.
        if (m_recentProjects) {
            m_recentProjects->remove(path.string());
            m_recentProjects->save();
        }
    }
}

void Engine::run() {
    if (!m_initialized) {
        std::cerr << "Cannot run: Engine not initialized!" << std::endl;
        return;
    }

    std::cout << "Starting main loop..." << std::endl;
    m_running = true;

    while (m_running && !glfwWindowShouldClose(m_window)) {
        // Update timing
        m_timeAuthority->updateTiming();
        double deltaTime = m_timeAuthority->getDeltaTime();

        // Detect potential freeze (frame took > 100ms)
        if (deltaTime > 0.1) {
            std::cout << "[FREEZE WARNING] Frame " << m_timeAuthority->getFrameCount() << " took "
                      << (deltaTime * 1000.0) << "ms (timeline frame: "
                      << (m_timeline ? m_timeline->getCurrentFrame() : 0) << ")" << std::endl;
        }

        // Update FPS counter
        m_fpsAccumulator += deltaTime;
        m_fpsFrameCount++;

        // Update FPS display every 0.5 seconds
        if (m_fpsAccumulator >= 0.5) {
            m_currentFPS = static_cast<uint32_t>(m_fpsFrameCount / m_fpsAccumulator);
            m_fpsAccumulator = 0.0;
            m_fpsFrameCount = 0;

            // Update window title with FPS
            std::string title = "Entity Media Server - Editor | " + std::to_string(m_currentFPS) + " FPS";
            glfwSetWindowTitle(m_window, title.c_str());
        }

        // Process events
        processEvents();

        // ADR-0009 — Launcher mode: editor UI is suppressed and the per-
        // tick simulation work is skipped entirely. Render still runs;
        // the launcher renders inside it.
        if (m_showLauncher) {
            render();
            m_timeAuthority->incrementFrameCount();
            continue;
        }

        // Process command queue
        if (m_commandDispatcher) {
            m_commandDispatcher->processQueue(*this);
        }

        // Update systems
        update();

        // Render
        render();

        // Subtask 7: drain Renderer->Director replies (CaptureCompleted)
        // so capture-broker resolutions land on the script-results object
        // before this iteration's finishScript check below. Same-tick
        // resolution keeps the existing ctest semantics where Exit can
        // immediately follow CaptureHash without losing the result.
        drainRendererToDirector();

        // Defer-finishScript pump: processQueue no longer auto-finishes
        // the script. We do it here, after the bus has resolved any
        // outstanding capture replies, so the written script_result.json
        // reflects the final state.
        if (m_commandDispatcher && m_commandDispatcher->scriptReadyToFinish()) {
            auto* broker = m_director ? m_director->getCaptureBroker() : nullptr;
            if (!broker || !broker->hasPending()) {
                m_commandDispatcher->finishScript();
            }
        }

        // Bail on GPU device-lost. Keep rendering into a dead device and you
        // get silent hangs, not useful for a live show. Shut down cleanly so
        // the operator knows they need to restart.
        if (m_renderer && m_renderer->isDeviceLost()) {
            std::cerr << "[Engine] D3D12 device lost — exiting main loop." << std::endl;
            m_running = false;
        }

        autoSaveTick(deltaTime);
        pollTranscodes();

        m_timeAuthority->incrementFrameCount();
    }

    std::cout << "Main loop exited." << std::endl;
}

void Engine::requestExit() {
    m_running = false;
}

IRenderer* Engine::getRenderer() {
    // Raw shortcut into m_rendererService->getRenderer(); external callers
    // see only the interface.
    return m_renderer;
}

void Engine::autoSaveTick(double deltaTime) {
    if (m_projectManager) m_projectManager->tickAutosave(deltaTime);
}

void Engine::updateTranscodeCacheDir() {
    if (!m_transcodeManager) return;
    std::filesystem::path cacheDir;
    if (m_projectManager && !m_projectManager->projectPath().empty()) {
        cacheDir = m_projectManager->projectPath().parent_path() / ".cache" / "hap";
    } else {
        cacheDir = std::filesystem::temp_directory_path() / "entity_hap_cache";
    }
    m_transcodeManager->setCacheDir(cacheDir);
}

void Engine::pollTranscodes() {
    if (!m_transcodeManager || !m_projectManager) return;

    // Snapshot the canonical paths whose Linked-style transcode hasn't
    // been recorded yet. We re-fetch the entry inside the loop so updates
    // land on the live entry; the snapshot just avoids iterator
    // invalidation if setTranscodedPath grows the vector. Managed entries
    // stay in the snapshot regardless of archivedOriginal — we still want
    // to observe Done transitions for logging + worker reap.
    std::vector<std::string> pendingCanonical;
    pendingCanonical.reserve(m_projectManager->loadedMediaFiles().size());
    for (const auto& e : m_projectManager->loadedMediaFiles()) {
        const bool linkedDone = (e.pathKind == ProjectManager::PathKind::Linked
                                 && !e.transcodedPath.empty());
        if (linkedDone) continue;
        pendingCanonical.push_back(e.originalPath);
    }

    for (const auto& canonical : pendingCanonical) {
        auto st = m_transcodeManager->statusOf(canonical);
        if (!st) continue;
        if (st->state != TranscodeState::Done) continue;

        auto* entry = m_projectManager->findEntry(canonical);
        if (!entry) continue;

        if (entry->pathKind == ProjectManager::PathKind::Managed) {
            // Source at canonical content path is now HAP. archivedOriginal
            // + originalCodec were set in scheduleTranscode pre-flight.
            // transcodedPath stays empty — for Managed the canonical
            // path IS the playable file. Just log + reap.
            std::cout << "[Engine] Transcode done (Managed): " << canonical
                      << "  archive=" << entry->archivedOriginal << std::endl;
        } else {
            // Linked: cache-dir output recorded on the entry the way the
            // legacy code expects.
            m_projectManager->setTranscodedPath(canonical, st->outputPath,
                                                st->variant);
            std::cout << "[Engine] Transcode done (Linked): " << canonical
                      << " -> " << st->outputPath << std::endl;
        }
    }

    // Reap Done workers so the manager doesn't grow unbounded across
    // long sessions. Failed workers stay — the MediaBin shows them in
    // red + offers Retry, which uses remove(src) to wipe the entry
    // before enqueuing a fresh attempt.
    m_transcodeManager->clearDone();
}

const std::vector<ProjectManager::MediaLibraryEntry>& Engine::getLoadedMediaFiles() const {
    static const std::vector<ProjectManager::MediaLibraryEntry> kEmpty;
    return m_projectManager ? m_projectManager->loadedMediaFiles() : kEmpty;
}

void Engine::removeMediaFromLibrary(const std::string& originalPath) {
    if (!m_projectManager) return;

    // Capture transcoded path before removing the entry.
    std::string transcoded;
    if (auto* e = m_projectManager->findEntry(originalPath)) {
        transcoded = e->transcodedPath;
    }

    // Cancel + reap any in-flight worker. Safe in any state — remove()
    // requests cancel + joins the thread.
    if (m_transcodeManager) m_transcodeManager->remove(originalPath);

    // Delete cached HAP file from disk. error_code variant so a missing
    // file (already cleaned up by hand, never finished transcoding, etc)
    // doesn't throw.
    if (!transcoded.empty()) {
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path(transcoded), ec);
        if (ec) {
            std::cerr << "[Engine] Failed to delete cached HAP " << transcoded
                      << ": " << ec.message() << std::endl;
        }
    }

    m_projectManager->removeMediaFile(originalPath);
    std::cout << "[Engine] Removed from library: " << originalPath << std::endl;
}

ProjectManager::NonHapImportPolicy Engine::nonHapImportPolicy() const {
    return m_projectManager ? m_projectManager->nonHapImportPolicy()
                            : ProjectManager::NonHapImportPolicy::Ask;
}
void Engine::setNonHapImportPolicy(ProjectManager::NonHapImportPolicy policy) {
    if (m_projectManager) m_projectManager->setNonHapImportPolicy(policy);
}

void Engine::setInputColorSpaceOverride(const std::string& originalPath,
                                        const std::string& override) {
    if (!m_projectManager) return;
    auto* entry = m_projectManager->findEntry(originalPath);
    if (!entry) return;
    entry->inputColorSpaceOverride = override;
}

std::string Engine::inputColorSpaceOverride(const std::string& originalPath) const {
    if (!m_projectManager) return {};
    if (const auto* entry = m_projectManager->findEntry(originalPath)) {
        return entry->inputColorSpaceOverride;
    }
    return {};
}

const Engine::PendingImport* Engine::pendingImport() const {
    return m_pendingImport ? &*m_pendingImport : nullptr;
}

void Engine::resolvePendingImport(bool transcode, bool dontAskAgain) {
    if (!m_pendingImport) return;
    PendingImport p = *m_pendingImport;   // copy before clearing
    m_pendingImport.reset();

    if (dontAskAgain && m_projectManager) {
        m_projectManager->setNonHapImportPolicy(
            transcode ? ProjectManager::NonHapImportPolicy::AlwaysTranscode
                      : ProjectManager::NonHapImportPolicy::NeverTranscode);
    }

    // p.filepath is the canonical path stashed by onVideoFileSelected (post
    // applyImportMode). Resolve to absolute for the transcoder; addMediaFile
    // and clip identity stay on the canonical (relative-for-Managed) string.
    if (transcode) {
        if (m_projectManager) {
            // Kind is preserved from the original import — applyImportMode
            // already moved the file to the canonical location, so a Managed
            // entry's storedPath is project-relative; addMediaFile is
            // idempotent so re-registering keeps the existing kind. To
            // pick the right kind on first registration when the entry
            // doesn't yet exist, infer from path shape: absolute => Linked,
            // relative => Managed.
            const ProjectManager::PathKind kind =
                std::filesystem::path(p.filepath).is_absolute()
                    ? ProjectManager::PathKind::Linked
                    : ProjectManager::PathKind::Managed;
            m_projectManager->addMediaFile(p.filepath, kind);
        }
        scheduleTranscode(p.filepath, p.mediaType);
        return;
    }

    // User chose Skip — create the clip on the source directly.
    if (m_projectManager) {
        const ProjectManager::PathKind kind =
            std::filesystem::path(p.filepath).is_absolute()
                ? ProjectManager::PathKind::Linked
                : ProjectManager::PathKind::Managed;
        m_projectManager->addMediaFile(p.filepath, kind);
    }
    ingestVideoClip(p.filepath, p.mediaType);
}

const std::filesystem::path& Engine::getProjectPath() const {
    static const std::filesystem::path kEmpty;
    return m_projectManager ? m_projectManager->projectPath() : kEmpty;
}

void Engine::processEvents() {
    glfwPollEvents();

    // TODO: Handle input events
}

void Engine::update() {
    auto t0 = std::chrono::high_resolution_clock::now();
    double deltaTime = m_timeAuthority ? m_timeAuthority->getDeltaTime() : 0.0;

    // Update timeline
    if (m_timeline) {
        m_timeline->update(deltaTime);
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    // NOTE: updateClipVideos() moved to render() - must be called AFTER beginFrame()
    // because beginFrame() resets the command list

    // Legacy single-clip decode (for backwards compatibility)
    if (m_decoder && m_decoder->isOpen() && m_currentFrame) {
        FrameNumber currentFrame = m_timeline->getCurrentFrame();
        if (currentFrame != m_currentFrame->frameNumber) {
            Result result = m_decoder->decodeFrame(currentFrame, *m_currentFrame);
            m_currentFrame->valid = (result == Result::Success);
        }
    }
    auto t2 = std::chrono::high_resolution_clock::now();

    // Update Director + Renderer-side systems by hand. CompositorSystem
    // stays out of this list -- it must run inside render() between
    // beginFrame/endFrame so its draw calls land on a recording command
    // list. Order: Animation (Director) -> Decode (Renderer). DecodeSystem
    // doesn't depend on Animation; the order is "Director-side first,
    // Renderer-side second" to match the eventual director.tick() ->
    // renderer.tick() split (subtask 8).
    if (m_animationSystem) {
        m_animationSystem->update(m_registry, static_cast<float>(deltaTime));
    }
    if (m_decodeSystem) {
        m_decodeSystem->update(m_registry, static_cast<float>(deltaTime));
    }
    auto t3 = std::chrono::high_resolution_clock::now();

    // Log timing if any stage took > 50ms
    auto timelineMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    auto legacyDecodeMs = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
    auto systemsMs = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();
    auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t0).count();

    if (totalMs > 50) {
        std::cout << "[UPDATE TIMING] Total=" << totalMs << "ms"
                  << " (timeline=" << timelineMs
                  << ", legacyDecode=" << legacyDecodeMs
                  << ", systems=" << systemsMs << ")" << std::endl;
    }
}

double Engine::getDeltaTime() const {
    return m_timeAuthority ? m_timeAuthority->getDeltaTime() : 0.0;
}

double Engine::getElapsedTime() const {
    return m_timeAuthority ? m_timeAuthority->getElapsedTime() : 0.0;
}

uint64_t Engine::getFrameCount() const {
    return m_timeAuthority ? m_timeAuthority->getFrameCount() : 0;
}

const DecodedFrame* Engine::getCurrentVideoFrame() const {
    if (m_playbackPresenter && m_timeAuthority) {
        if (const DecodedFrame* frame = m_playbackPresenter->getCurrentVideoFrame(*m_timeAuthority)) {
            return frame;
        }
    }
    // Fall back to legacy single frame (pre-multi-clip path)
    if (m_currentFrame && m_currentFrame->valid) {
        return m_currentFrame.get();
    }
    return nullptr;
}

void Engine::render() {
    // Handle pending resize before starting a new frame
    if (m_resizePending && m_renderer && m_renderer->isInitialized()) {
        std::cout << "Applying deferred resize to " << m_pendingWidth << "x" << m_pendingHeight << std::endl;

        m_windowWidth = m_pendingWidth;
        m_windowHeight = m_pendingHeight;

        Result result = m_renderer->resize(m_pendingWidth, m_pendingHeight);
        if (result != Result::Success) {
            std::cerr << "Failed to resize D3D12 renderer!" << std::endl;
        }

        m_resizePending = false;
    }

    if (m_renderer && m_renderer->isInitialized()) {
        // ADR-0009 — Launcher mode: skip the compositor + outputs pass
        // entirely; just clear, run an ImGui frame containing the
        // launcher, and present. Avoids touching scene-state-dependent
        // systems (CompositorSystem, OutputManager) before a project
        // exists.
        if (m_showLauncher) {
            m_renderer->beginFrame();
            m_renderer->clear(0.05f, 0.06f, 0.08f, 1.0f);
            m_renderer->beginImGuiFrame();
            if (m_launcher) {
                auto result = m_launcher->render();
                switch (result.action) {
                    case ProjectLauncher::Action::Open:
                        // Defer the actual project load until *after*
                        // endFrame() — loadProject() touches GPU resources
                        // (allocateVideoTextureSlot etc.) and we don't want
                        // to mix that with an in-flight ImGui draw.
                        m_renderer->endImGuiFrame();
                        m_renderer->endFrame();
                        onLauncherOpenProject(result.path);
                        return;
                    case ProjectLauncher::Action::Quit:
                        requestExit();
                        break;
                    case ProjectLauncher::Action::None:
                        break;
                }
            }
            m_renderer->endImGuiFrame();
            m_renderer->endFrame();
            return;
        }

        auto t0 = std::chrono::high_resolution_clock::now();

        // Subtask 7: capture-request drain runs *before* beginFrame.
        // tonemapAndReadbackComposeTarget and captureBackBufferToPNG
        // both reset the command list themselves and assume the GPU is
        // settled (no in-flight beginFrame work). Anything else on D2R
        // (RenderFrame, SetOutputEnabled, ApplySettings) gets stashed
        // here and replayed after beginFrame.
        drainCaptureRequestsPreFrame();

        m_renderer->beginFrame();
        auto t1 = std::chrono::high_resolution_clock::now();

        // Phase D entry, subtask 8: Director publishes a per-tick
        // RenderFrame to the bus; Renderer drains it and applies it via
        // PlaybackPresenter. Sequential ticks on the main thread make
        // this a single send + drain pair per frame -- the in-memory
        // transport is just a serialization point. Phase E swaps the
        // transport for UDP without touching either endpoint.
        //
        // Must run after beginFrame() so the GPU upload calls land on a
        // recording command list.
        if (m_transport) {
            if (m_timeAuthority && m_playbackPresenter) {
                bus::RenderFrame rf;
                m_timeAuthority->buildRenderFrame(rf);
                m_transport->send(bus::Direction::D2R, bus::serialize(bus::Message{rf}));
            }
            m_transport->drain(bus::Direction::D2R, [this](std::vector<std::uint8_t>&& bytes) {
                auto msg = bus::deserialize(bytes);
                if (!msg) return;
                std::visit([this](auto& body) {
                    using T = std::decay_t<decltype(body)>;
                    if constexpr (std::is_same_v<T, bus::RenderFrame>) {
                        if (m_playbackPresenter) m_playbackPresenter->present(body);
                    } else if constexpr (std::is_same_v<T, bus::SetOutputEnabled>) {
                        if (m_outputManager) {
                            m_outputManager->setOutputEnabled(
                                static_cast<entt::entity>(body.entity), body.enabled);
                        }
                    } else if constexpr (std::is_same_v<T, bus::ApplySettings>) {
                        if (m_frameCache) {
                            m_frameCache->setMaxBytes(static_cast<size_t>(body.frameCacheBytes));
                        }
                        // OCIO config-path reload still requires a restart;
                        // tooltip in Preferences flags this. The path travels
                        // here so a future hot-reload subtask only touches
                        // Renderer-side code.
                        (void)body.ocioConfigPath;
                    } else if constexpr (std::is_same_v<T, bus::ProvisionClipResources>) {
                        // Renderer-side: allocate the GPU descriptor slot.
                        // ClipDecodeState already carries the decoder
                        // (Director opened it to read metadata before
                        // publishing); spawning the actual DecodeWorker
                        // happens lazily via DecodeSystem. Reply on R2D so
                        // Director writes the slot back to the VideoTexture
                        // component this same tick.
                        bus::ResourcesProvisioned reply{};
                        reply.entity = body.entity;
                        if (m_renderer) {
                            const uint32_t slot = m_renderer->allocateVideoTextureSlot();
                            if (slot == UINT32_MAX) {
                                reply.descriptorSlot = -1;
                                reply.ok = false;
                                reply.errorMessage = "no available video texture slots";
                            } else {
                                reply.descriptorSlot = static_cast<int>(slot);
                                reply.ok = true;
                            }
                        } else {
                            reply.descriptorSlot = -1;
                            reply.ok = false;
                            reply.errorMessage = "renderer not initialized";
                        }
                        if (m_transport) {
                            m_transport->send(bus::Direction::R2D,
                                              bus::serialize(bus::Message{reply}));
                        }
                    }
                    // Other variant alternatives are R2D-only or arrive on
                    // future subtasks; ignore them here.
                }, *msg);
            });
        }
        auto t2 = std::chrono::high_resolution_clock::now();

        // Clear to a nice teal/cyan color (to warm your heart!)
        m_renderer->clear(0.0f, 0.5f, 0.6f, 1.0f);

        // Render all layers via CompositorSystem. Must run inside the
        // beginFrame/endFrame window so draws land on the open command
        // list -- this is why it isn't in update().
        if (auto* compositor = m_rendererService ? m_rendererService->getCompositorSystem() : nullptr) {
            double deltaTime = m_timeAuthority ? m_timeAuthority->getDeltaTime() : 0.0;
            compositor->update(m_registry, static_cast<float>(deltaTime));
        }

        // Physical outputs: fan out each enabled output's compose target to
        // its dedicated swap chain. Must run after the compositor (compose
        // targets are now in SHADER_RESOURCE state) and before ImGui (so the
        // main RT stays selected for the overlay).
        if (m_outputManager) {
            m_outputManager->renderOutputs();
        }
        auto t3 = std::chrono::high_resolution_clock::now();

        // Begin ImGui frame for UI rendering
        m_renderer->beginImGuiFrame();

        // Render window manager (DockSpace and all windows)
        if (m_windowManager) {
            m_windowManager->render();
        }

        // End ImGui frame and render UI
        m_renderer->endImGuiFrame();
        auto t4 = std::chrono::high_resolution_clock::now();

        m_renderer->endFrame();
        auto t5 = std::chrono::high_resolution_clock::now();

        // Log timing if any stage took > 50ms
        auto beginFrameMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        auto clipVideosMs = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
        auto compositorMs = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();
        auto imguiMs = std::chrono::duration_cast<std::chrono::milliseconds>(t4 - t3).count();
        auto endFrameMs = std::chrono::duration_cast<std::chrono::milliseconds>(t5 - t4).count();
        auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(t5 - t0).count();

        if (totalMs > 50) {
            std::cout << "[RENDER TIMING] Total=" << totalMs << "ms"
                      << " (beginFrame=" << beginFrameMs
                      << ", clipVideos=" << clipVideosMs
                      << ", compositor=" << compositorMs
                      << ", imgui=" << imguiMs
                      << ", endFrame=" << endFrameMs << ")" << std::endl;
        }
    }
}

void Engine::onWindowResize(uint32_t width, uint32_t height) {
    // Handle minimized window (width/height = 0)
    if (width == 0 || height == 0) {
        std::cout << "Window minimized, skipping resize..." << std::endl;
        return;
    }

    // Enforce minimum window size
    constexpr uint32_t MIN_WIDTH = 640;
    constexpr uint32_t MIN_HEIGHT = 480;

    uint32_t newWidth = width;
    uint32_t newHeight = height;

    if (newWidth < MIN_WIDTH) newWidth = MIN_WIDTH;
    if (newHeight < MIN_HEIGHT) newHeight = MIN_HEIGHT;

    // Store pending resize - will be applied at the start of next frame
    // This prevents deadlock when resize happens mid-frame
    m_resizePending = true;
    m_pendingWidth = newWidth;
    m_pendingHeight = newHeight;

    std::cout << "Window resize requested to " << newWidth << "x" << newHeight << " (deferred)" << std::endl;
}

void Engine::onKeyEvent(int key, int scancode, int action, int mods) {
    // Mark unused parameters to avoid warnings
    (void)scancode;

    // Allow REPEAT events only for a small allowlist of keys where holding
    // makes sense (frame-step arrows). Everything else stays press-only so
    // accidental hold-Ctrl+Z doesn't undo a hundred edits.
    const bool isRepeatableKey = (key == GLFW_KEY_LEFT) || (key == GLFW_KEY_RIGHT);
    if (action == GLFW_PRESS) {
        // ok — fall through
    } else if (action == GLFW_REPEAT && isRepeatableKey) {
        // ok — fall through
    } else {
        return;
    }

    // Don't handle shortcuts if user is actively typing in a text field
    // WantTextInput is more specific than WantCaptureKeyboard - it's only true
    // when a text input widget is active and accepting input
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) {
        return;
    }

    // Check for Ctrl modifier
    bool ctrlPressed = (mods & GLFW_MOD_CONTROL) != 0;
    bool shiftPressed = (mods & GLFW_MOD_SHIFT) != 0;

    // Handle ESC — live-performance safety. If any Physical output is
    // currently driving a display, the first ESC disables them all so the
    // editor becomes visible again. Only if no outputs are live does ESC
    // exit the app. Avoids the "accidental ESC during a show kills the
    // whole program" footgun.
    if (key == GLFW_KEY_ESCAPE) {
        bool killedAny = false;
        if (m_outputManager) {
            auto view = m_registry.view<OutputDisplay>();
            for (auto [entity, output] : view.each()) {
                if (output.isPhysical() && output.enabled && output.outputWindowSlot != UINT32_MAX) {
                    publishSetOutputEnabled(bus::SetOutputEnabled{
                        static_cast<std::uint64_t>(entity), false});
                    killedAny = true;
                }
            }
        }
        if (killedAny) {
            std::cout << "ESC pressed: disabled active physical outputs." << std::endl;
        } else {
            std::cout << "ESC pressed, requesting exit..." << std::endl;
            requestExit();
        }
        return;
    }

    // Timeline keyboard shortcuts (only if timeline exists)
    if (m_timeline) {
        switch (key) {
            case GLFW_KEY_SPACE:
                // Toggle play/pause -- routed through CommandDispatcher so the
                // action is undoable + script-recordable (Phase D entry: this
                // unblocks the bus-routing work in subtask 7).
                if (m_commandDispatcher) {
                    m_commandDispatcher->enqueue(std::make_unique<TogglePlayPauseCommand>());
                }
                break;

            case GLFW_KEY_K:
                // K = Pause (industry standard)
                if (m_commandDispatcher) {
                    m_commandDispatcher->enqueue(std::make_unique<PauseCommand>());
                }
                break;

            case GLFW_KEY_J:
                // J = Step backward one frame at the timeline's frame rate.
                // StepBackwardCommand handles the pause-then-step semantics.
                if (m_commandDispatcher) {
                    m_commandDispatcher->enqueue(std::make_unique<StepBackwardCommand>(1));
                }
                if (m_timelineWidget) m_timelineWidget->ensurePlayheadVisible();
                break;

            case GLFW_KEY_L:
                // L = Step forward one frame at the timeline's frame rate.
                if (m_commandDispatcher) {
                    m_commandDispatcher->enqueue(std::make_unique<StepForwardCommand>(1));
                }
                if (m_timelineWidget) m_timelineWidget->ensurePlayheadVisible();
                break;

            case GLFW_KEY_LEFT:
                // Left arrow = step back by one zoom-tick increment.
                // Disguise convention. GLFW_REPEAT is allowed for this key
                // (allowlisted at the top of the handler) so holding the
                // arrow scrubs continuously by tick increments.
                {
                    FrameNumber step = m_timelineWidget ? m_timelineWidget->framesPerTick() : 1;
                    FrameNumber cur = m_timeline->getCurrentFrame();
                    FrameNumber target = cur > step ? cur - step : 0;
                    // Snap target to the tick grid so repeated presses don't
                    // drift off-grid when the playhead started between ticks
                    // (e.g. landed mid-tick from playback then user hit Left).
                    target = (target / step) * step;
                    m_timeline->seekToFrame(target);
                }
                if (m_timelineWidget) m_timelineWidget->ensurePlayheadVisible();
                break;

            case GLFW_KEY_RIGHT:
                // Right arrow = step forward by one zoom-tick increment.
                // Holding repeats (allowlisted for REPEAT events).
                {
                    FrameNumber step = m_timelineWidget ? m_timelineWidget->framesPerTick() : 1;
                    FrameNumber cur = m_timeline->getCurrentFrame();
                    // Snap cur up to the next tick boundary, then add step.
                    // Without the snap, repeated presses from a mid-tick start
                    // would forever-step from a non-aligned position.
                    FrameNumber aligned = ((cur + step - 1) / step) * step;
                    FrameNumber target = (aligned == cur) ? cur + step : aligned;
                    Timecode targetTime = m_timeline->frameToTime(target);
                    if (targetTime < m_timeline->getDuration()) {
                        m_timeline->seekToFrame(target);
                    }
                }
                if (m_timelineWidget) m_timelineWidget->ensurePlayheadVisible();
                break;

            case GLFW_KEY_HOME:
                // Home = Go to start
                m_timeline->seek(0);
                if (m_timelineWidget) m_timelineWidget->ensurePlayheadVisible();
                break;

            case GLFW_KEY_END:
                // End = Go to end of timeline duration
                m_timeline->seek(m_timeline->getDuration());
                if (m_timelineWidget) m_timelineWidget->ensurePlayheadVisible();
                break;

            case GLFW_KEY_S:
                // S = Split clip at playhead
                {
                    entt::entity selectedClip = m_timeline->getSelectedClip();
                    if (selectedClip != entt::null) {
                        FrameNumber currentFrame = m_timeline->getCurrentFrame();
                        entt::entity newClip = m_timeline->splitClip(selectedClip, currentFrame);
                        if (newClip != entt::null) {
                            std::cout << "Split clip at frame " << currentFrame << std::endl;
                        }
                    }
                }
                break;

            case GLFW_KEY_D:
                // Ctrl+D = Duplicate selected clip
                if (ctrlPressed) {
                    entt::entity selectedClip = m_timeline->getSelectedClip();
                    if (selectedClip != entt::null) {
                        entt::entity newClip = m_timeline->duplicateClip(selectedClip);
                        if (newClip != entt::null) {
                            std::cout << "Duplicated clip" << std::endl;
                        }
                    }
                }
                break;

            case GLFW_KEY_DELETE:
                // Delete = Delete selected clip
                {
                    entt::entity selectedClip = m_timeline->getSelectedClip();
                    if (selectedClip != entt::null) {
                        m_timeline->deleteClip(selectedClip);
                        m_timeline->setSelectedClip(entt::null);
                        std::cout << "Deleted selected clip" << std::endl;
                    }
                }
                break;

            default:
                break;
        }
    }

    // Global shortcuts (work even without timeline focus)
    switch (key) {
        case GLFW_KEY_S:
            // Ctrl+Shift+S = Save As; Ctrl+S = Save (prompts if no path yet)
            if (ctrlPressed && shiftPressed) {
                saveProjectAsInteractive();
            } else if (ctrlPressed) {
                saveProjectInteractive();
            }
            break;

        case GLFW_KEY_O:
            // Ctrl+O = Open project via native dialog
            if (ctrlPressed) {
                openProjectInteractive();
            }
            break;

        case GLFW_KEY_L:
            // Ctrl+L = Toggle layout lock
            if (ctrlPressed && m_windowManager) {
                bool newState = !m_windowManager->isLayoutLocked();
                m_windowManager->setLayoutLocked(newState);
                std::cout << "[Engine] Layout " << (newState ? "locked" : "unlocked") << std::endl;
            }
            break;

        case GLFW_KEY_Z:
            // Ctrl+Z = Undo, Ctrl+Shift+Z = Redo.
            if (ctrlPressed && m_commandDispatcher) {
                if (shiftPressed) {
                    m_commandDispatcher->redo(*this);
                } else {
                    m_commandDispatcher->undo(*this);
                }
            }
            break;

        case GLFW_KEY_Y:
            // Ctrl+Y = Redo (Windows convention; Ctrl+Shift+Z also works).
            if (ctrlPressed && m_commandDispatcher) {
                m_commandDispatcher->redo(*this);
            }
            break;

        case GLFW_KEY_I:
            // Ctrl+Shift+I = Insert time at the active range selection.
            // Same path as the Edit menu item — go through CommandDispatcher
            // so it's undoable + recordable.
            if (ctrlPressed && shiftPressed && m_timelineWidget && m_commandDispatcher
                && m_timelineWidget->hasRangeSelection()) {
                FrameNumber startF = m_timeline->timeToFrame(m_timelineWidget->getRangeStart());
                FrameNumber endF   = m_timeline->timeToFrame(m_timelineWidget->getRangeEnd());
                FrameNumber dur = endF - startF;
                if (dur > 0) {
                    m_commandDispatcher->enqueue(std::make_unique<RippleInsertTimeCommand>(startF, dur));
                    m_timelineWidget->clearRangeSelection();
                }
            }
            break;

        case GLFW_KEY_DELETE:
            // Ctrl+Shift+Delete = Remove selected time. (Plain Delete is left
            // alone — that's the existing clip-delete shortcut.)
            if (ctrlPressed && shiftPressed && m_timelineWidget && m_commandDispatcher
                && m_timelineWidget->hasRangeSelection()) {
                FrameNumber startF = m_timeline->timeToFrame(m_timelineWidget->getRangeStart());
                FrameNumber endF   = m_timeline->timeToFrame(m_timelineWidget->getRangeEnd());
                if (endF > startF) {
                    m_commandDispatcher->enqueue(std::make_unique<RippleDeleteTimeCommand>(startF, endF));
                    m_timelineWidget->clearRangeSelection();
                }
            }
            break;

        default:
            break;
    }
}

void Engine::testComponents() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Phase 3: Testing ECS Components" << std::endl;
    std::cout << "========================================\n" << std::endl;

    // Track all test entities for cleanup
    std::vector<entt::entity> testEntities;

    // Test 1: Transform Component
    std::cout << "Test 1: Transform Component" << std::endl;
    {
        auto entity = m_registry.create();
        testEntities.push_back(entity);
        auto& transform = m_registry.emplace<Transform>(entity);

        transform.setPosition(glm::vec3(100.0f, 200.0f, 0.0f));
        transform.setRotation(glm::vec3(0.0f, 0.0f, 45.0f));
        transform.setScale(glm::vec3(2.0f, 2.0f, 1.0f));

        const glm::mat4& matrix = transform.getMatrix();

        assert(transform.dirty == false); // Matrix should be updated
        assert(matrix[3][0] == 100.0f);   // Translation X
        assert(matrix[3][1] == 200.0f);   // Translation Y

        std::cout << "  ✓ Transform matrix calculation works" << std::endl;
        std::cout << "  ✓ Position: (" << transform.position.x << ", "
                  << transform.position.y << ", " << transform.position.z << ")" << std::endl;
    }

    // Test 2: MediaLayer Component
    std::cout << "\nTest 2: MediaLayer Component" << std::endl;
    {
        auto entity = m_registry.create();
        testEntities.push_back(entity);
        auto& layer = m_registry.emplace<MediaLayer>(entity);

        layer.zOrder = 5;
        layer.opacity = 0.75f;
        layer.blendMode = BlendMode::Add;
        layer.visible = true;

        assert(layer.shouldRender() == true);
        assert(layer.getOpacity() == 0.75f);

        layer.opacity = -0.5f;
        assert(layer.getOpacity() == 0.0f); // Should clamp to 0

        layer.opacity = 1.5f;
        assert(layer.getOpacity() == 1.0f); // Should clamp to 1

        layer.visible = false;
        assert(layer.shouldRender() == false);

        std::cout << "  ✓ MediaLayer opacity clamping works" << std::endl;
        std::cout << "  ✓ MediaLayer visibility check works" << std::endl;
    }

    // Test 3: Clip Component
    std::cout << "\nTest 3: Clip Component" << std::endl;
    {
        // Create TimelineSystem for testing clip logic
        TimelineSystem timelineSystem;

        auto entity = m_registry.create();
        testEntities.push_back(entity);
        auto& clip = m_registry.emplace<Clip>(entity);

        clip.filepath = "test_video.mov";
        clip.mediaType = MediaType::VideoProRes4444;
        clip.startFrame = 100;
        clip.duration = 150;
        clip.mediaStartFrame = 0;
        clip.framerate = 30.0;
        clip.width = 1920;
        clip.height = 1080;
        clip.hasAlpha = true;

        // Use TimelineSystem instead of component methods
        assert(timelineSystem.containsFrame(clip, 100) == true);
        assert(timelineSystem.containsFrame(clip, 150) == true);
        assert(timelineSystem.containsFrame(clip, 249) == true);
        assert(timelineSystem.containsFrame(clip, 250) == false); // Beyond duration
        assert(timelineSystem.containsFrame(clip, 99) == false);  // Before start

        assert(timelineSystem.mapToMediaFrame(clip, 100) == 0);   // Timeline 100 -> Media 0
        assert(timelineSystem.mapToMediaFrame(clip, 150) == 50);  // Timeline 150 -> Media 50

        std::cout << "  ✓ Clip timing calculations work (via TimelineSystem)" << std::endl;
        std::cout << "  ✓ Clip frame mapping works (via TimelineSystem)" << std::endl;
        std::cout << "  ✓ Media type: " << MediaTypeToString(clip.mediaType) << std::endl;
    }

    // Test 4: VideoTexture Component
    std::cout << "\nTest 4: VideoTexture Component" << std::endl;
    {
        auto entity = m_registry.create();
        testEntities.push_back(entity);
        auto& texture = m_registry.emplace<VideoTexture>(entity);

        texture.width = 1920;
        texture.height = 1080;
        texture.format = PixelFormat::RGBA8;
        texture.needsUpload = false;

        // Note: texture.isValid() will return false because we haven't created the D3D12 resource
        // This is expected - full texture creation happens during media loading
        assert(texture.isValid() == false); // No GPU resource yet

        std::cout << "  ✓ VideoTexture component created" << std::endl;
        std::cout << "  ✓ Resolution: " << texture.width << "x" << texture.height << std::endl;
    }

    // Test 5 (FrameBuffer atomic-state probe + BufferSystem) removed in
    // Phase C.10: FrameBuffer is now an empty marker, BufferSystem is gone,
    // and buffer state lives in the engine-global FrameCache instead.

    // Test 6: TimelineTrack Component
    std::cout << "\nTest 6: TimelineTrack Component" << std::endl;
    {
        auto trackEntity = m_registry.create();
        testEntities.push_back(trackEntity);
        auto& track = m_registry.emplace<TimelineTrack>(trackEntity);

        track.trackIndex = 0;

        // Create some clip entities and add them to track
        auto clip1 = m_registry.create();
        testEntities.push_back(clip1);
        auto clip2 = m_registry.create();
        testEntities.push_back(clip2);
        auto clip3 = m_registry.create();
        testEntities.push_back(clip3);

        track.addClip(clip1);
        track.addClip(clip2);
        track.addClip(clip3);

        assert(track.getClipCount() == 3);
        assert(track.isEmpty() == false);

        track.removeClip(clip2);
        assert(track.getClipCount() == 2);

        std::cout << "  ✓ TimelineTrack clip management works" << std::endl;
        std::cout << "  ✓ Clips in track: " << track.getClipCount() << std::endl;
    }

    // Test 7: OutputMapping Component
    std::cout << "\nTest 7: OutputMapping Component" << std::endl;
    {
        auto entity = m_registry.create();
        testEntities.push_back(entity);
        auto& output = m_registry.emplace<OutputMapping>(entity);

        output.displayIndex = 0;
        output.displayEDID = "EDID_ABC123";
        output.displayName = "Primary Display";
        output.position = glm::ivec2(0, 0);
        output.resolution = glm::ivec2(1920, 1080);
        output.isPrimary = true;
        output.isConnected = true;

        // Note: isValid() will return false until swap chain is created
        assert(output.isConnected == true);
        float aspectRatio = output.getAspectRatio();
        assert(aspectRatio == 1920.0f / 1080.0f); // 16:9

        std::cout << "  ✓ OutputMapping component created" << std::endl;
        std::cout << "  ✓ Display: " << output.displayName << " ("
                  << output.resolution.x << "x" << output.resolution.y << ")" << std::endl;
        std::cout << "  ✓ Aspect ratio: " << aspectRatio << std::endl;
    }

    // Test 8: EnTT View Iteration
    std::cout << "\nTest 8: EnTT View Iteration" << std::endl;
    {
        // Create multiple entities with different component combinations
        for (int i = 0; i < 5; i++) {
            auto entity = m_registry.create();
            testEntities.push_back(entity);
            m_registry.emplace<Transform>(entity);
            m_registry.emplace<MediaLayer>(entity);

            if (i % 2 == 0) {
                m_registry.emplace<Clip>(entity);
            }
        }

        // Test view with Transform + MediaLayer
        auto view = m_registry.view<Transform, MediaLayer>();
        int count = 0;
        for (auto [entity, transform, layer] : view.each()) {
            (void)entity;
            (void)transform;
            (void)layer;
            count++;
        }
        assert(count == 5);
        std::cout << "  ✓ Found " << count << " entities with Transform + MediaLayer" << std::endl;

        // Test view with all three components
        auto clipView = m_registry.view<Transform, MediaLayer, Clip>();
        int clipCount = 0;
        for (auto [entity, transform, layer, clip] : clipView.each()) {
            (void)entity;
            (void)transform;
            (void)layer;
            (void)clip;
            clipCount++;
        }
        assert(clipCount == 3); // Only entities with i % 2 == 0
        std::cout << "  ✓ Found " << clipCount << " entities with Transform + MediaLayer + Clip" << std::endl;
    }

    // Test 9: Component Removal
    std::cout << "\nTest 9: Component Removal" << std::endl;
    {
        auto entity = m_registry.create();
        testEntities.push_back(entity);
        m_registry.emplace<Transform>(entity);
        m_registry.emplace<MediaLayer>(entity);

        assert(m_registry.all_of<Transform>(entity) == true);
        assert(m_registry.all_of<MediaLayer>(entity) == true);

        m_registry.remove<MediaLayer>(entity);
        assert(m_registry.all_of<Transform>(entity) == true);
        assert(m_registry.all_of<MediaLayer>(entity) == false);

        std::cout << "  ✓ Component removal works" << std::endl;
    }

    // Test 10: Entity Destruction
    std::cout << "\nTest 10: Entity Destruction" << std::endl;
    {
        auto entity = m_registry.create();
        m_registry.emplace<Transform>(entity);

        assert(m_registry.valid(entity) == true);

        m_registry.destroy(entity);
        assert(m_registry.valid(entity) == false);

        std::cout << "  ✓ Entity destruction works" << std::endl;
    }

    // Clean up all test entities
    std::cout << "\nCleaning up " << testEntities.size() << " test entities..." << std::endl;
    for (auto entity : testEntities) {
        if (m_registry.valid(entity)) {
            m_registry.destroy(entity);
        }
    }

    std::cout << "\n========================================" << std::endl;
    std::cout << "All Component Tests Passed! ✓" << std::endl;
    std::cout << "========================================\n" << std::endl;
}

void Engine::createTestEntities() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Phase 4.1: Creating Test Render Entities" << std::endl;
    std::cout << "========================================\n" << std::endl;

    // Create 3 test entities at different positions with different layers

    // Entity 1: Red quad at top-left
    {
        auto entity = m_registry.create();
        auto& transform = m_registry.emplace<Transform>(entity);
        auto& layer = m_registry.emplace<MediaLayer>(entity);

        // Position in normalized device coordinates (-1 to 1)
        // Scale down to 0.3 so we can see multiple quads
        transform.setPosition(glm::vec3(-0.5f, 0.5f, 0.0f));
        transform.setScale(glm::vec3(0.3f, 0.3f, 1.0f));

        layer.zOrder = 1;
        layer.opacity = 1.0f;
        layer.visible = true;
        layer.blendMode = BlendMode::Normal;

        std::cout << "  Created Entity 1: Red quad at top-left (z=1)" << std::endl;
    }

    // Entity 2: Green quad at center (overlapping)
    {
        auto entity = m_registry.create();
        auto& transform = m_registry.emplace<Transform>(entity);
        auto& layer = m_registry.emplace<MediaLayer>(entity);

        transform.setPosition(glm::vec3(0.0f, 0.0f, 0.0f));
        transform.setScale(glm::vec3(0.4f, 0.4f, 1.0f));

        layer.zOrder = 2; // Higher z-order, renders on top
        layer.opacity = 0.8f; // Semi-transparent
        layer.visible = true;
        layer.blendMode = BlendMode::Normal;

        std::cout << "  Created Entity 2: Green quad at center (z=2, opacity=0.8)" << std::endl;
    }

    // Entity 3: Blue quad at bottom-right with rotation
    {
        auto entity = m_registry.create();
        auto& transform = m_registry.emplace<Transform>(entity);
        auto& layer = m_registry.emplace<MediaLayer>(entity);

        transform.setPosition(glm::vec3(0.5f, -0.5f, 0.0f));
        transform.setRotation(glm::vec3(0.0f, 0.0f, 45.0f)); // 45 degree rotation
        transform.setScale(glm::vec3(0.3f, 0.3f, 1.0f));

        layer.zOrder = 0; // Lowest z-order, renders first (behind others)
        layer.opacity = 1.0f;
        layer.visible = true;
        layer.blendMode = BlendMode::Normal;

        std::cout << "  Created Entity 3: Blue quad at bottom-right, rotated (z=0)" << std::endl;
    }

    std::cout << "\n========================================" << std::endl;
    std::cout << "Test Entities Created! ✓" << std::endl;
    std::cout << "========================================\n" << std::endl;
}

int Engine::rebuildProjectStructure() {
    if (!m_projectManager) return 0;
    return m_projectManager->rebuildStructure();
}

bool Engine::restoreOriginalMedia(const std::string& canonicalPath) {
    if (!m_projectManager) return false;

    // Wipe any TranscodeManager bookkeeping for this canonical path —
    // a Done worker still in the map would otherwise have its outputPath
    // (the now-deleted HAP) treated as fresh on the next pollTranscodes.
    if (m_transcodeManager) {
        m_transcodeManager->remove(canonicalPath);
    }

    if (!m_projectManager->restoreOriginal(canonicalPath)) {
        return false;
    }

    // Reload the project so DecodeSystem opens the now-original-codec
    // file with the right decoder. Same pattern as findMissingMedia.
    if (!m_projectManager->projectPath().empty()) {
        const auto path = m_projectManager->projectPath();
        std::cout << "[Engine] Reloading project after restoring " << canonicalPath
                  << std::endl;
        loadProject(path);
    }
    return true;
}

int Engine::findMissingMedia(const std::filesystem::path& searchDir) {
    if (!m_projectManager) return 0;
    auto result = m_projectManager->findMissingManagedMedia(searchDir);

    // If we restored at least one file, reload the project so the per-clip
    // decode state attaches to the now-resolvable media. Without this,
    // ClipDecodeState was never emplaced (loadProject's media callback
    // returns early when the file doesn't exist), so the freshly-restored
    // file wouldn't actually decode until a manual reload.
    if (result.copied > 0 && !m_projectManager->projectPath().empty()) {
        const auto path = m_projectManager->projectPath();
        std::cout << "[Engine] Reloading project after restoring "
                  << result.copied << " files." << std::endl;
        loadProject(path);
    }

    return result.copied;
}

int Engine::countLinkedEntries() const {
    if (!m_projectManager) return 0;
    int n = 0;
    for (const auto& e : m_projectManager->loadedMediaFiles()) {
        if (e.pathKind == ProjectManager::PathKind::Linked) ++n;
    }
    return n;
}

int Engine::collectLinkedIntoProject(const std::string& subfolder) {
    if (!m_projectManager) return 0;

    auto result = m_projectManager->collectLinkedIntoProject(subfolder);

    // Walk the clip registry once to rewrite every reference that the
    // collect step displaced. Doing this here (Engine-side) keeps
    // ProjectManager free of EnTT coupling — it sticks to project state.
    if (!result.rewrites.empty()) {
        auto view = m_registry.view<Clip>();
        for (auto [entity, clip] : view.each()) {
            for (const auto& [oldPath, newPath] : result.rewrites) {
                if (clip.filepath == oldPath) {
                    clip.filepath = newPath;
                    break;
                }
            }
        }
    }

    if (result.collected == 0 && result.missing == 0) {
        std::cout << "[Engine] Collect: nothing to do." << std::endl;
    }
    return result.collected;
}

bool Engine::scheduleTranscode(const std::string& canonicalPath,
                                MediaType sourceMediaType,
                                const std::string& variant,
                                double srcFps) {
    namespace fs = std::filesystem;

    if (!m_projectManager || !m_transcodeManager) return false;
    auto* entry = m_projectManager->findEntry(canonicalPath);
    if (!entry) {
        std::cerr << "[Engine] scheduleTranscode: " << canonicalPath
                  << " is not in the media library" << std::endl;
        return false;
    }

    // Skip if already transcoded. Two indicators per pathKind:
    if (entry->pathKind == ProjectManager::PathKind::Managed &&
        !entry->archivedOriginal.empty()) {
        return false;  // Managed: source replaced; archive present.
    }
    if (entry->pathKind == ProjectManager::PathKind::Linked &&
        !entry->transcodedPath.empty()) {
        return false;  // Linked: cache-dir transcode recorded.
    }

    const std::string srcAbs = m_projectManager->resolveMediaPath(canonicalPath);
    if (srcAbs.empty()) return false;

    if (entry->pathKind == ProjectManager::PathKind::Linked) {
        // Legacy path — output goes into the project (or temp) cache dir.
        // Worker reads source, writes hashed-name HAP file. setTranscodedPath
        // fires from pollTranscodes when the worker finishes.
        m_transcodeManager->enqueue(srcAbs, variant, srcFps);
        std::cout << "[Engine] Queued Linked transcode for " << canonicalPath
                  << " (cache dir output)" << std::endl;
        return true;
    }

    // Managed: archive original, transcode replaces source at canonical path.
    const fs::path projectRoot = m_projectManager->projectPath().parent_path();
    const fs::path canonicalRel(canonicalPath);
    const fs::path srcAbsPath(srcAbs);

    const fs::path archiveDirRel =
        canonicalRel.parent_path() / ProjectManager::kArchiveDir;
    const fs::path archiveDirAbs = projectRoot / archiveDirRel;
    const fs::path archiveAbs = archiveDirAbs / canonicalRel.filename();

    std::error_code ec;
    fs::create_directories(archiveDirAbs, ec);
    if (ec) {
        std::cerr << "[Engine] scheduleTranscode: failed to create archive dir "
                  << archiveDirAbs.string() << ": " << ec.message() << std::endl;
        return false;
    }

    // Copy (don't move) so a transcode failure leaves both source and archive
    // intact. The transcode then overwrites the source at the canonical path.
    // overwrite_existing covers the rare case of a re-import or retry.
    fs::copy_file(srcAbsPath, archiveAbs, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "[Engine] scheduleTranscode: failed to archive "
                  << srcAbsPath.string() << " -> " << archiveAbs.string()
                  << ": " << ec.message() << std::endl;
        return false;
    }

    // Stash the archive metadata on the entry now (relative to project root)
    // so a crash mid-transcode leaves a project file that still knows where
    // the original is. originalCodec records the pre-transcode media type.
    fs::path archiveRel = fs::relative(archiveAbs, projectRoot, ec);
    entry->archivedOriginal =
        ec ? archiveAbs.generic_string() : archiveRel.generic_string();
    entry->originalCodec = MediaTypeToString(sourceMediaType);

    // Worker reads from the archive copy and writes to the canonical
    // content path, overwriting the source. The TranscodeManager is
    // keyed by canonicalPath (the user-meaningful identity) rather
    // than archiveAbs — that keeps statusOf / remove / pollTranscodes
    // calls in MediaBin and Engine working off entry.originalPath the
    // same way they do for Linked entries.
    m_transcodeManager->enqueue(archiveAbs.string(), variant, srcFps,
                                /*explicitDstPath=*/srcAbs,
                                /*explicitKey=*/canonicalPath);
    std::cout << "[Engine] Queued Managed transcode for " << canonicalPath
              << " (archived original to " << entry->archivedOriginal << ")"
              << std::endl;
    return true;
}

void Engine::setImportMode(ImportMode mode, const std::string& subfolder) {
    m_importMode = mode;
    if (!subfolder.empty()) m_importSubfolder = subfolder;
}

std::string Engine::applyImportMode(const std::string& sourceAbsolutePath,
                                     ImportMode mode,
                                     const std::string& subfolder,
                                     ProjectManager::PathKind* outKind) {
    namespace fs = std::filesystem;

    auto setKind = [&](ProjectManager::PathKind k) {
        if (outKind) *outKind = k;
    };

    // Link mode (and the no-project / no-content-dir fallbacks) preserve
    // pre-v7 behavior exactly: absolute path stored as-is, pathKind = Linked.
    if (mode == ImportMode::Link) {
        setKind(ProjectManager::PathKind::Linked);
        return sourceAbsolutePath;
    }

    // Copy mode but no project loaded -> fall back to Link. We could refuse
    // the import instead, but that would break script-driven flows that
    // never call OpenProject. A console line keeps the choice visible.
    if (!m_projectManager || m_projectManager->projectPath().empty()) {
        std::cerr << "[Engine] Copy import requested but no project is "
                     "loaded; falling back to Link." << std::endl;
        setKind(ProjectManager::PathKind::Linked);
        return sourceAbsolutePath;
    }

    const fs::path projectRoot = m_projectManager->projectPath().parent_path();
    const fs::path contentDir  = projectRoot / ProjectManager::kContentDir;

    std::error_code ec;
    if (!fs::exists(contentDir, ec)) {
        // Legacy v6 project: no content/ tree on disk. Falling back to
        // Link is safe; the user can run "Collect into project folder"
        // later to upgrade.
        std::cerr << "[Engine] Project has no content/ directory (legacy v6?); "
                     "falling back to Link import for " << sourceAbsolutePath
                  << std::endl;
        setKind(ProjectManager::PathKind::Linked);
        return sourceAbsolutePath;
    }

    const std::string sub = subfolder.empty()
                                ? std::string(ProjectManager::kUnsortedDir)
                                : subfolder;
    const fs::path targetDir = contentDir / sub;
    fs::create_directories(targetDir, ec);
    if (ec) {
        std::cerr << "[Engine] Failed to create import subfolder "
                  << targetDir.string() << ": " << ec.message()
                  << "; falling back to Link." << std::endl;
        setKind(ProjectManager::PathKind::Linked);
        return sourceAbsolutePath;
    }

    const fs::path source(sourceAbsolutePath);
    fs::path target = targetDir / source.filename();

    // If target already exists with the same name, suffix to avoid silent
    // overwrite. "intro.mov" -> "intro (1).mov", "intro (2).mov", ...
    if (fs::exists(target, ec)) {
        const std::string stem = source.stem().string();
        const std::string ext  = source.extension().string();
        for (int i = 1; i < 1000; ++i) {
            fs::path candidate = targetDir /
                (stem + " (" + std::to_string(i) + ")" + ext);
            if (!fs::exists(candidate, ec)) {
                target = candidate;
                break;
            }
        }
    }

    fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "[Engine] Failed to copy " << source.string() << " -> "
                  << target.string() << ": " << ec.message()
                  << "; falling back to Link." << std::endl;
        setKind(ProjectManager::PathKind::Linked);
        return sourceAbsolutePath;
    }

    // Stored path is project-relative (forward slashes for portability —
    // Windows native filesystem APIs accept either, JSON readers see a
    // stable representation).
    fs::path relative = fs::relative(target, projectRoot, ec);
    std::string canonical = ec ? target.string() : relative.generic_string();

    std::cout << "[Engine] Imported (Copy) " << source.filename().string()
              << " -> " << canonical << std::endl;
    setKind(ProjectManager::PathKind::Managed);
    return canonical;
}

void Engine::onVideoFileSelected(const std::string& filePath) {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Video File Selected" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "File: " << filePath << std::endl;

    MediaType mediaType = detectMediaType(filePath);
    if (mediaType == MediaType::Unknown) {
        std::cerr << "ERROR: Unsupported media type: " << filePath << std::endl;
        std::cerr << "========================================\n" << std::endl;
        return;
    }

    std::cout << "Detected media type: " << MediaTypeToString(mediaType) << std::endl;

    // ADR-0009 — apply the active import mode (Copy/Link) before any
    // policy logic so downstream code keys off the canonical
    // originalPath the project will actually persist.
    ProjectManager::PathKind importedKind = ProjectManager::PathKind::Linked;
    const std::string canonicalPath = applyImportMode(
        filePath, m_importMode, m_importSubfolder, &importedKind);
    if (canonicalPath.empty()) {
        std::cerr << "ERROR: Import failed for " << filePath << std::endl;
        std::cerr << "========================================\n" << std::endl;
        return;
    }

    // Only ProRes / H264 / HEVC containers are transcode-eligible — vcpkg
    // FFmpeg lacks a PNG decoder so image sequences stay on the CPU path,
    // and HAP files obviously need no work.
    const bool transcodeEligible =
        !isHapMediaType(mediaType) &&
        mediaType == MediaType::VideoProRes4444 &&
        m_projectManager && m_transcodeManager;

    if (transcodeEligible) {
        const auto policy = m_projectManager->nonHapImportPolicy();
        switch (policy) {
            case ProjectManager::NonHapImportPolicy::AlwaysTranscode: {
                m_projectManager->addMediaFile(canonicalPath, importedKind);
                scheduleTranscode(canonicalPath, mediaType);
                std::cout << "========================================\n" << std::endl;
                return;
            }
            case ProjectManager::NonHapImportPolicy::Ask:
                if (m_pendingImport) {
                    std::cerr << "[Engine] Import already awaiting user decision ("
                              << m_pendingImport->filepath
                              << "); dropping " << canonicalPath << std::endl;
                    return;
                }
                // Stash the canonical (post-import) path so the modal's
                // resolve path operates on the same identity as everything
                // else. importedKind is implicit: by the time the user
                // clicks a modal button the file is already at the
                // canonical location, so addMediaFile uses the same kind.
                m_pendingImport = PendingImport{canonicalPath, mediaType};
                std::cout << "[Engine] Awaiting user decision on non-HAP import: "
                          << canonicalPath << std::endl;
                std::cout << "========================================\n" << std::endl;
                return;
            case ProjectManager::NonHapImportPolicy::NeverTranscode:
                break;  // fall through to ingest
        }
    }

    if (m_projectManager) {
        m_projectManager->addMediaFile(canonicalPath, importedKind);
    }
    ingestVideoClip(canonicalPath, mediaType);
}

void Engine::ingestVideoClip(const std::string& canonicalPath, MediaType mediaType) {
    // Create decoder for the media type
    m_decoder = createDecoder(mediaType);
    if (!m_decoder) {
        std::cerr << "ERROR: Failed to create decoder for media type: "
                  << MediaTypeToString(mediaType) << std::endl;
        std::cerr << "========================================\n" << std::endl;
        return;
    }

    std::cout << "Created decoder: " << MediaTypeToString(mediaType) << std::endl;

    // Resolve canonicalPath to an absolute filesystem path for the decoder.
    // Managed entries are project-relative; Linked entries pass through
    // unchanged. Caller is expected to have already registered the entry
    // with addMediaFile so the lookup succeeds.
    const std::string openPath = m_projectManager
        ? m_projectManager->resolveMediaPath(canonicalPath)
        : canonicalPath;

    // Open the media file
    Result result = m_decoder->open(openPath);
    if (result != Result::Success) {
        std::cerr << "ERROR: Failed to open media file: " << openPath << std::endl;
        m_decoder.reset();
        std::cerr << "========================================\n" << std::endl;
        return;
    }

    std::cout << "Media opened successfully:" << std::endl;
    std::cout << "  Resolution: " << m_decoder->getWidth() << "x" << m_decoder->getHeight() << std::endl;
    std::cout << "  Duration: " << m_decoder->getDuration() << " frames" << std::endl;
    std::cout << "  Frame rate: " << m_decoder->getFrameRate() << " fps" << std::endl;
    std::cout << "  Has alpha: " << (m_decoder->hasAlpha() ? "yes" : "no") << std::endl;

    // Create clip entity with all required components for rendering
    entt::entity clipEntity = m_registry.create();

    // Add Clip component with metadata. clip.filepath is the canonical
    // identity (matches mediaLibrary entry's originalPath) — relative
    // for Managed, absolute for Linked.
    auto& clip = m_registry.emplace<Clip>(clipEntity);
    clip.filepath = canonicalPath;
    clip.mediaType = mediaType;
    clip.width = m_decoder->getWidth();
    clip.height = m_decoder->getHeight();
    clip.framerate = m_decoder->getFrameRate();
    clip.totalMediaFrames = m_decoder->getDuration();  // Store original source length (in source frames)
    // Convert source frames to timeline frames for duration
    // E.g., 100 frames at 24fps on 30fps timeline = 100 * (30/24) = 125 timeline frames
    double timelineFrameRate = m_timeline ? m_timeline->getFrameRate() : 30.0;
    clip.duration = static_cast<FrameNumber>(std::ceil(
        clip.totalMediaFrames * (timelineFrameRate / clip.framerate)));
    clip.hasAlpha = m_decoder->hasAlpha();
    clip.startFrame = 0;  // Place at timeline start
    clip.mediaStartFrame = 0;
    clip.loaded = true;

    // Add Transform component (identity transform = fullscreen)
    auto& transform = m_registry.emplace<Transform>(clipEntity);
    transform.setPosition(glm::vec3(0.0f, 0.0f, 0.0f));
    transform.setScale(glm::vec3(1.0f, 1.0f, 1.0f));
    transform.setRotation(glm::vec3(0.0f, 0.0f, 0.0f));

    // Add MediaLayer component for rendering
    auto& layer = m_registry.emplace<MediaLayer>(clipEntity);
    layer.opacity = 1.0f;
    layer.visible = true;
    layer.blendMode = BlendMode::Normal;
    // zOrder will be set based on track (done below)

    // Add VideoTexture component; the GPU descriptor slot is allocated
    // Renderer-side via the bus (subtask 7). The slot stays UINT32_MAX
    // for one tick after publish; CompositorSystem's isAllocated() guard
    // skips the clip during that window.
    auto& videoTex = m_registry.emplace<VideoTexture>(clipEntity);
    publishProvisionClipResources(bus::ProvisionClipResources{
        static_cast<std::uint64_t>(clipEntity),
        AssetId{canonicalPath},
        mediaType,
        clip.framerate,
        clip.totalMediaFrames});

    // Marker tag for DecodeSystem's view query — frames go through the
    // engine-global FrameCache now, not a per-clip ring buffer.
    m_registry.emplace<FrameBuffer>(clipEntity);

    // Store decoder and create frame buffer for this clip (legacy path)
    auto& state = m_registry.emplace_or_replace<ClipDecodeState>(clipEntity);
    state.decoder = std::move(m_decoder);  // Transfer ownership
    state.frame = std::make_unique<DecodedFrame>();
    state.frame->allocate(clip.width, clip.height);
    state.lastDecodedFrame = UINT32_MAX;  // Force decode on first frame

    // Re-create m_decoder as nullptr (no longer used for this clip)
    m_decoder = nullptr;

    // Get or create track and add clip (with overlap detection)
    if (m_timeline->getTrackCount() == 0) {
        m_timeline->createTrack("Video Track 1");
    }

    // Find a track where the new clip won't overlap with existing clips
    const auto& tracks = m_timeline->getTracks();
    int finalTrackIndex = -1;
    FrameNumber newClipStart = clip.startFrame;
    FrameNumber newClipEnd = clip.startFrame + clip.duration;

    // Helper lambda to check if new clip overlaps with existing clips on a track
    auto wouldOverlap = [&](int checkTrackIndex) -> bool {
        if (checkTrackIndex < 0 || checkTrackIndex >= static_cast<int>(tracks.size())) {
            return true;  // Invalid track
        }
        auto& checkTrack = m_registry.get<TimelineTrack>(tracks[checkTrackIndex]);
        for (entt::entity existingClipEntity : checkTrack.clips) {
            auto* existingClip = m_registry.try_get<Clip>(existingClipEntity);
            if (!existingClip) continue;

            FrameNumber existingStart = existingClip->startFrame;
            FrameNumber existingEnd = existingClip->startFrame + existingClip->duration;

            // Check for overlap
            if (!(newClipEnd <= existingStart || newClipStart >= existingEnd)) {
                return true;  // Overlaps
            }
        }
        return false;  // No overlap
    };

    // Search from track 0 (top of timeline) downward for non-overlapping track
    for (int i = 0; i < static_cast<int>(tracks.size()); i++) {
        if (!wouldOverlap(i)) {
            finalTrackIndex = i;
            break;
        }
    }

    // If all existing tracks have overlap, create a new track
    if (finalTrackIndex < 0) {
        std::string newTrackName = "Video Track " + std::to_string(m_timeline->getTracks().size() + 1);
        m_timeline->createTrack(newTrackName);
        finalTrackIndex = static_cast<int>(m_timeline->getTracks().size()) - 1;
        std::cout << "Created new track " << (finalTrackIndex + 1) << " to avoid overlap" << std::endl;
    }

    // Add clip to selected track
    auto& finalTrack = m_registry.get<TimelineTrack>(m_timeline->getTracks()[finalTrackIndex]);
    finalTrack.addClip(clipEntity);
    finalTrack.sortClips(m_registry); // Maintain sorted order by start frame
    std::cout << "Added clip to track " << (finalTrackIndex + 1) << std::endl;

    // Set z-order based on track index
    // Track 0 (top of timeline UI) should render on top = highest z-order
    layer.zOrder = 1000 - static_cast<uint32_t>(finalTrackIndex);

    // Note: Timeline frame rate is NOT changed to match clip frame rate.
    // This allows mixed frame rate content on a fixed timeline (e.g., 24fps video on 30fps timeline).
    // Frame rate conversion is handled by mapToMediaFrame() and DecodeSystem.

    // Legacy: Reuse m_currentFrame for backwards compatibility with StageWindow
    // Only reallocate if dimensions changed, otherwise reuse existing buffer
    if (!m_currentFrame ||
        m_currentFrame->width != clip.width ||
        m_currentFrame->height != clip.height) {
        m_currentFrame = std::make_unique<DecodedFrame>();
        m_currentFrame->allocate(clip.width, clip.height);
    }

    std::cout << "Clip created with VideoTexture, Transform, MediaLayer components" << std::endl;
    std::cout << "Clip duration: " << clip.duration << " frames at " << clip.framerate << " fps" << std::endl;
    std::cout << "Ready for multi-layer compositing!" << std::endl;
    std::cout << "========================================\n" << std::endl;
}

void Engine::onMediaDroppedOnTimeline(const std::string& filePath, int trackIndex, Timecode position) {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Media Dropped on Timeline" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "File: " << filePath << std::endl;
    std::cout << "Track: " << (trackIndex + 1) << std::endl;
    std::cout << "Position: " << (position / 1000000.0) << " seconds" << std::endl;

    // If the source has a transcoded HAP sibling in the library, open that
    // instead — clip.filepath stays as the original-path identity the
    // MediaBin drag payload carries.
    const std::string openPath = m_projectManager
        ? m_projectManager->decoderPathFor(filePath)
        : filePath;
    if (openPath != filePath) {
        std::cout << "  Resolving to transcoded: " << openPath << std::endl;
    }

    // Detect media type from the path we're actually going to open.
    MediaType mediaType = detectMediaType(openPath);
    if (mediaType == MediaType::Unknown) {
        std::cerr << "ERROR: Unsupported media type: " << openPath << std::endl;
        std::cerr << "========================================\n" << std::endl;
        return;
    }

    // Create a new decoder for this clip
    auto decoder = createDecoder(mediaType);
    if (!decoder) {
        std::cerr << "ERROR: Failed to create decoder for media type: "
                  << MediaTypeToString(mediaType) << std::endl;
        std::cerr << "========================================\n" << std::endl;
        return;
    }

    // Open the media file (transcoded if available, else original).
    Result result = decoder->open(openPath);
    if (result != Result::Success) {
        std::cerr << "ERROR: Failed to open media file: " << filePath << std::endl;
        std::cerr << "========================================\n" << std::endl;
        return;
    }

    std::cout << "Media info:" << std::endl;
    std::cout << "  Resolution: " << decoder->getWidth() << "x" << decoder->getHeight() << std::endl;
    std::cout << "  Duration: " << decoder->getDuration() << " frames" << std::endl;
    std::cout << "  Frame rate: " << decoder->getFrameRate() << " fps" << std::endl;

    // Add to loaded media files list (idempotent — no kind change if entry
    // already exists). Path-shape inference handles the unusual case where
    // a drop fires for a path that wasn't registered via the import flow.
    if (m_projectManager) {
        const ProjectManager::PathKind kind =
            std::filesystem::path(filePath).is_absolute()
                ? ProjectManager::PathKind::Linked
                : ProjectManager::PathKind::Managed;
        m_projectManager->addMediaFile(filePath, kind);
    }

    // Create clip entity with all required components
    entt::entity clipEntity = m_registry.create();

    // Calculate start frame from drop position using TIMELINE frame rate
    double sourceFrameRate = decoder->getFrameRate();
    double timelineFrameRate = m_timeline ? m_timeline->getFrameRate() : 30.0;
    float positionSeconds = position / 1000000.0f;
    FrameNumber startFrame = static_cast<FrameNumber>(positionSeconds * timelineFrameRate);

    // Add Clip component with metadata
    auto& clip = m_registry.emplace<Clip>(clipEntity);
    clip.filepath = filePath;
    clip.mediaType = mediaType;
    clip.width = decoder->getWidth();
    clip.height = decoder->getHeight();
    clip.framerate = sourceFrameRate;
    clip.totalMediaFrames = decoder->getDuration();  // Store original source length (in source frames)
    // Convert source frames to timeline frames for duration
    // E.g., 100 frames at 24fps on 30fps timeline = 100 * (30/24) = 125 timeline frames
    clip.duration = static_cast<FrameNumber>(std::ceil(
        clip.totalMediaFrames * (timelineFrameRate / sourceFrameRate)));
    clip.hasAlpha = decoder->hasAlpha();
    clip.startFrame = startFrame;
    clip.mediaStartFrame = 0;
    clip.loaded = true;

    // Add Transform component (identity transform = fullscreen)
    auto& transform = m_registry.emplace<Transform>(clipEntity);
    transform.setPosition(glm::vec3(0.0f, 0.0f, 0.0f));
    transform.setScale(glm::vec3(1.0f, 1.0f, 1.0f));
    transform.setRotation(glm::vec3(0.0f, 0.0f, 0.0f));

    // Find a track where the new clip won't overlap with existing clips
    // Start from the requested track and search for a non-overlapping track
    const auto& tracks = m_timeline->getTracks();
    int finalTrackIndex = -1;
    FrameNumber newClipStart = startFrame;
    FrameNumber newClipEnd = startFrame + clip.duration;

    // Helper lambda to check if new clip overlaps with existing clips on a track
    auto wouldOverlap = [&](int checkTrackIndex) -> bool {
        if (checkTrackIndex < 0 || checkTrackIndex >= static_cast<int>(tracks.size())) {
            return true;  // Invalid track
        }
        auto& checkTrack = m_registry.get<TimelineTrack>(tracks[checkTrackIndex]);
        for (entt::entity existingClipEntity : checkTrack.clips) {
            auto* existingClip = m_registry.try_get<Clip>(existingClipEntity);
            if (!existingClip) continue;

            FrameNumber existingStart = existingClip->startFrame;
            FrameNumber existingEnd = existingClip->startFrame + existingClip->duration;

            // Check for overlap: NOT (newEnd <= existingStart OR newStart >= existingEnd)
            if (!(newClipEnd <= existingStart || newClipStart >= existingEnd)) {
                return true;  // Overlaps
            }
        }
        return false;  // No overlap
    };

    // First, check the requested track
    if (trackIndex >= 0 && trackIndex < static_cast<int>(tracks.size())) {
        if (!wouldOverlap(trackIndex)) {
            finalTrackIndex = trackIndex;
        }
    }

    // If requested track has overlap, search other tracks (starting from highest index)
    if (finalTrackIndex < 0) {
        for (int i = static_cast<int>(tracks.size()) - 1; i >= 0; i--) {
            if (!wouldOverlap(i)) {
                finalTrackIndex = i;
                break;
            }
        }
    }

    // If all existing tracks have overlap, create a new track
    if (finalTrackIndex < 0) {
        std::string newTrackName = "Video Track " + std::to_string(m_timeline->getTracks().size() + 1);
        m_timeline->createTrack(newTrackName);
        finalTrackIndex = static_cast<int>(m_timeline->getTracks().size()) - 1;
        std::cout << "Created new track " << (finalTrackIndex + 1) << " to avoid overlap" << std::endl;
    }

    // Add MediaLayer component for rendering
    auto& layer = m_registry.emplace<MediaLayer>(clipEntity);
    layer.opacity = 1.0f;
    layer.visible = true;
    layer.blendMode = BlendMode::Normal;
    // Track 0 (top of timeline UI) should render on top = highest z-order
    layer.zOrder = 1000 - static_cast<uint32_t>(finalTrackIndex);

    // Add VideoTexture component; slot allocation flows through the bus
    // (subtask 7). UINT32_MAX for one tick is fine -- isAllocated()
    // guards downstream.
    auto& videoTex = m_registry.emplace<VideoTexture>(clipEntity);
    (void)videoTex;
    publishProvisionClipResources(bus::ProvisionClipResources{
        static_cast<std::uint64_t>(clipEntity),
        AssetId{filePath},
        mediaType,
        clip.framerate,
        clip.totalMediaFrames});

    // Marker tag for DecodeSystem (decoded frames live in the engine cache).
    m_registry.emplace<FrameBuffer>(clipEntity);

    // Store decoder and create frame buffer for this clip (legacy path)
    auto& state = m_registry.emplace_or_replace<ClipDecodeState>(clipEntity);
    state.decoder = std::move(decoder);
    state.frame = std::make_unique<DecodedFrame>();
    state.frame->allocate(clip.width, clip.height);
    state.lastDecodedFrame = UINT32_MAX;  // Force decode on first frame

    // Add clip to the selected track
    auto& finalTrack = m_registry.get<TimelineTrack>(m_timeline->getTracks()[finalTrackIndex]);
    finalTrack.addClip(clipEntity);
    finalTrack.sortClips(m_registry); // Maintain sorted order by start frame
    std::cout << "Added clip to track " << (finalTrackIndex + 1) << " at frame " << startFrame << std::endl;

    // Extend timeline duration if needed
    float clipEndSeconds = (startFrame + clip.duration) / clip.framerate;
    Timecode clipEndTimecode = static_cast<Timecode>(clipEndSeconds * 1000000.0);
    if (clipEndTimecode > m_timeline->getDuration()) {
        m_timeline->setDuration(clipEndTimecode);
        std::cout << "Extended timeline duration to " << clipEndSeconds << " seconds" << std::endl;
    }

    std::cout << "Clip created and added to timeline" << std::endl;
    std::cout << "========================================\n" << std::endl;
}

bool Engine::saveProject(const std::filesystem::path& filepath) {
    if (!m_projectManager) return false;
    bool ok = m_projectManager->save(filepath);
    if (ok) updateTranscodeCacheDir();
    return ok;
}

bool Engine::loadProject(const std::filesystem::path& filepath) {
    if (!m_projectManager) return false;

    // Pre-load: tear down any currently-active physical output windows. The
    // load will clear the OutputDisplay entities that hold the slot IDs; if
    // we don't release them first, those renderer slots leak for the rest
    // of the session.
    if (m_outputManager) {
        auto view = m_registry.view<OutputDisplay>();
        std::vector<entt::entity> toDisable;
        for (auto [entity, out] : view.each()) {
            if (out.outputWindowSlot != UINT32_MAX) {
                toDisable.push_back(entity);
            }
        }
        for (auto e : toDisable) {
            m_outputManager->setOutputEnabled(e, false);
        }
    }

    bool ok = m_projectManager->load(filepath);
    if (!ok) return false;

    // Point the transcode cache at <projectDir>/.cache/hap so subsequent
    // enqueue() writes land next to the project, not in %TEMP%.
    updateTranscodeCacheDir();

    // Re-queue library entries whose transcoded side is missing, IFF the
    // project's policy is AlwaysTranscode. Ask/NeverTranscode projects
    // don't get silent re-transcode kicks on load — respect the user's
    // earlier decision. ADR-0009: skip Managed entries with archivedOriginal
    // set (the source has already been transcode-replaced) AND probe
    // through resolveMediaPath so detectMediaType reads the right file
    // for Managed entries.
    if (m_transcodeManager &&
        m_projectManager->nonHapImportPolicy() ==
            ProjectManager::NonHapImportPolicy::AlwaysTranscode) {
        // Iterate by canonical path; scheduleTranscode does the rest of
        // the policy work (skip-if-already-transcoded for both kinds).
        std::vector<std::string> candidates;
        for (const auto& entry : m_projectManager->loadedMediaFiles()) {
            candidates.push_back(entry.originalPath);
        }
        for (const auto& canonical : candidates) {
            const std::string srcAbs = m_projectManager->resolveMediaPath(canonical);
            const MediaType mt = detectMediaType(srcAbs);
            if (mt == MediaType::Unknown) continue;
            if (isHapMediaType(mt)) continue;
            const double srcFps = (mt == MediaType::PNGSequence) ? 30.0 : 0.0;
            if (scheduleTranscode(canonical, mt, "hap_alpha", srcFps)) {
                std::cout << "[Engine] Re-enqueued transcode for " << canonical << std::endl;
            }
        }
    }

    // Post-load: sync the output-index counter so new outputs the user
    // creates don't collide with loaded indices; then bring up windows for
    // any physical outputs that were saved as enabled.
    if (m_outputManager) {
        m_outputManager->syncCounterFromRegistry();

        auto view = m_registry.view<OutputDisplay>();
        std::vector<entt::entity> toEnable;
        for (auto [entity, out] : view.each()) {
            if (out.enabled && out.isPhysical() && out.physicalDisplayIndex >= 0) {
                toEnable.push_back(entity);
            }
        }
        for (auto e : toEnable) {
            // setOutputEnabled(true) will call ensureOutputWindow which
            // allocates the swap chain + back buffers for the display.
            m_outputManager->setOutputEnabled(e, true);
        }
    }

    // ADR-0009 — bump Recent on every successful project load, not just
    // launcher-driven ones. File > Open inside the editor and script
    // OpenProject commands flow through here too, so this is the right
    // place to centralize the touch.
    if (m_recentProjects) {
        m_recentProjects->touch(filepath.string());
        m_recentProjects->save();
    }
    return true;
}

bool Engine::saveProjectInteractive() {
    if (!m_projectManager) return false;
    const auto& currentPath = m_projectManager->projectPath();
    if (currentPath.empty()) {
        return saveProjectAsInteractive();
    }
    return saveProject(currentPath);
}

bool Engine::saveProjectAsInteractive() {
    if (!m_windowManager || !m_projectManager) return false;
    std::string suggested = m_projectManager->projectPath().empty()
                                ? std::string{}
                                : m_projectManager->projectPath().string();
    std::string chosen = m_windowManager->saveProjectFileDialog(suggested);
    if (chosen.empty()) return false;  // Cancelled
    return saveProject(chosen);
}

bool Engine::openProjectInteractive() {
    if (!m_windowManager) return false;
    std::string chosen = m_windowManager->openProjectFileDialog();
    if (chosen.empty()) return false;  // Cancelled
    return loadProject(chosen);
}

void Engine::onClipCreated(entt::entity clipEntity, const std::string& filepath) {
    std::cout << "[Engine] Setting up resources for new clip entity=" << static_cast<uint32_t>(clipEntity)
              << ", file=" << filepath << std::endl;

    // Detect media type
    MediaType mediaType = detectMediaType(filepath);
    if (mediaType == MediaType::Unknown) {
        std::cerr << "[Engine] Unsupported media type for: " << filepath << std::endl;
        return;
    }

    // Create decoder
    auto decoder = createDecoder(mediaType);
    if (!decoder) {
        std::cerr << "[Engine] Failed to create decoder for: " << filepath << std::endl;
        return;
    }

    // Open the media file
    Result result = decoder->open(filepath);
    if (result != Result::Success) {
        std::cerr << "[Engine] Failed to open media: " << filepath << std::endl;
        return;
    }

    // Get clip info
    auto* clip = m_registry.try_get<Clip>(clipEntity);
    if (!clip) {
        std::cerr << "[Engine] Clip component not found for entity" << std::endl;
        return;
    }

    // Update clip's loaded state
    clip->loaded = true;

    // Slot allocation flows through the bus (subtask 7). Mirrors the
    // pre-bus condition: only request a slot if the existing
    // VideoTexture's slot looks unset (descriptorSlot == 0 was the
    // sentinel duplicateClip() left behind; new VideoTexture's default
    // is UINT32_MAX, also treated as unset).
    if (auto* videoTex = m_registry.try_get<VideoTexture>(clipEntity);
        videoTex && (videoTex->descriptorSlot == 0 ||
                     videoTex->descriptorSlot == UINT32_MAX)) {
        publishProvisionClipResources(bus::ProvisionClipResources{
            static_cast<std::uint64_t>(clipEntity),
            AssetId{filepath},
            mediaType,
            clip->framerate,
            clip->totalMediaFrames});
    }

    // Store decoder and create frame buffer
    auto& state = m_registry.emplace_or_replace<ClipDecodeState>(clipEntity);
    state.decoder = std::move(decoder);
    state.frame = std::make_unique<DecodedFrame>();
    state.frame->allocate(clip->width, clip->height);
    state.lastDecodedFrame = UINT32_MAX;  // Force decode on first frame

    std::cout << "[Engine] New clip resources created successfully" << std::endl;
}

void Engine::createDefaultScreen() {
    // Create a default 16:9 plane model
    entt::entity modelEntity = m_registry.create();
    Model& model = m_registry.emplace<Model>(modelEntity);
    model.name = "Default 16:9 Plane";
    model.filepath = "";  // Built-in, no file
    model.mesh = createDefaultScreenMesh();
    std::cout << "[Engine] Created default model: " << model.name
              << " (" << model.mesh.vertices.size() << " vertices, "
              << model.mesh.indices.size() << " indices)" << std::endl;

    // Create the default screen using that model
    entt::entity screenEntity = m_registry.create();
    Screen& screen = m_registry.emplace<Screen>(screenEntity);
    screen.name = "Main Screen";
    screen.modelEntity = modelEntity;
    screen.width = 1920;
    screen.height = 1080;
    screen.position = {0.0f, 0.0f, 0.0f};
    screen.rotation = {0.0f, 0.0f, 0.0f};
    screen.scale = {1.0f, 1.0f, 1.0f};
    screen.visible = true;
    screen.opacity = 1.0f;
    screen.zOrder = 0;
    std::cout << "[Engine] Created default screen: " << screen.name
              << " (entity=" << static_cast<uint32_t>(screenEntity) << ", "
              << screen.width << "x" << screen.height << ")" << std::endl;
}

bool Engine::importVideo(const std::string& filepath, int trackIndex, Timecode position) {
    // This is a public wrapper around the internal onMediaDroppedOnTimeline
    // Check if file exists first
    if (!std::filesystem::exists(filepath)) {
        std::cerr << "[Engine] Import failed: file not found: " << filepath << std::endl;
        return false;
    }

    // Validate track index
    const auto& tracks = m_timeline->getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size())) {
        std::cerr << "[Engine] Import failed: invalid track index: " << trackIndex << std::endl;
        return false;
    }

    // Call the internal import method
    onMediaDroppedOnTimeline(filepath, trackIndex, position);
    return true;
}

bool Engine::runScript(const std::string& filepath) {
    if (!m_commandDispatcher) {
        std::cerr << "[Engine] Script failed: command dispatcher not initialized" << std::endl;
        return false;
    }

    return m_commandDispatcher->loadScript(filepath);
}

void Engine::publishSetOutputEnabled(const bus::SetOutputEnabled& msg) {
    if (!m_transport) return;
    m_transport->send(bus::Direction::D2R, bus::serialize(bus::Message{msg}));
}

void Engine::publishApplySettings(const bus::ApplySettings& msg) {
    if (!m_transport) return;
    m_transport->send(bus::Direction::D2R, bus::serialize(bus::Message{msg}));
}

void Engine::publishProvisionClipResources(const bus::ProvisionClipResources& msg) {
    if (!m_transport) return;
    m_transport->send(bus::Direction::D2R, bus::serialize(bus::Message{msg}));
}

void Engine::drainCaptureRequestsPreFrame() {
    if (!m_transport) return;
    // Skim D2R for capture requests; everything else gets re-published so
    // the post-beginFrame drain below picks it up in the original order.
    std::vector<std::vector<std::uint8_t>> deferred;
    m_transport->drain(bus::Direction::D2R, [&](std::vector<std::uint8_t>&& bytes) {
        auto msg = bus::deserialize(bytes);
        if (!msg) return;
        if (auto* req = std::get_if<bus::RequestComposeCapture>(&*msg)) {
            handleCaptureRequest(*req);
        } else {
            deferred.push_back(std::move(bytes));
        }
    });
    for (auto& bytes : deferred) {
        m_transport->send(bus::Direction::D2R, std::move(bytes));
    }
}

void Engine::handleCaptureRequest(const bus::RequestComposeCapture& req) {
    bus::CaptureCompleted reply{};
    reply.correlationId = req.correlationId;
    reply.ok = false;

    auto sendReply = [&]() {
        if (m_transport) {
            m_transport->send(bus::Direction::R2D,
                              bus::serialize(bus::Message{reply}));
        }
    };

    if (!m_renderer || !m_renderer->isInitialized()) {
        reply.errorMessage = "renderer not initialized";
        sendReply();
        return;
    }

    if (req.hashOnly) {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> pixels;
        if (!m_renderer->readComposeTargetPixels(static_cast<uint32_t>(req.slot),
                                                  width, height, pixels)) {
            reply.errorMessage = "readComposeTargetPixels failed";
            sendReply();
            return;
        }

        // FNV-1a 64 -- deterministic, dependency-free, good enough for
        // exact-match pixel comparisons. Same constants and emit format as
        // the pre-bus path so existing goldens stay valid.
        constexpr std::uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
        constexpr std::uint64_t FNV_PRIME  = 0x00000100000001b3ULL;
        std::uint64_t hash = FNV_OFFSET;
        for (uint8_t b : pixels) {
            hash ^= static_cast<std::uint64_t>(b);
            hash *= FNV_PRIME;
        }

        char buf[64];
        int n = std::snprintf(buf, sizeof(buf), "%016llx %ux%u\n",
                              static_cast<unsigned long long>(hash), width, height);
        if (n <= 0) {
            reply.errorMessage = "snprintf failed";
            sendReply();
            return;
        }
        reply.hexHash.assign(buf, static_cast<size_t>(n));
        reply.ok = true;
        sendReply();
        return;
    }

    // Screenshot path -- write a PNG from compose target or back buffer.
    bool ok = req.fullWindow
        ? m_renderer->captureBackBufferToPNG(req.pngPath)
        : m_renderer->captureComposeTargetToPNG(req.pngPath);
    if (!ok) {
        reply.errorMessage = req.fullWindow
            ? "captureBackBufferToPNG failed"
            : "captureComposeTargetToPNG failed";
    } else {
        reply.ok = true;
    }
    sendReply();
}

void Engine::drainRendererToDirector() {
    if (!m_transport) return;
    auto* broker = m_director ? m_director->getCaptureBroker() : nullptr;
    m_transport->drain(bus::Direction::R2D, [&](std::vector<std::uint8_t>&& bytes) {
        auto msg = bus::deserialize(bytes);
        if (!msg) return;
        std::visit([&](auto& body) {
            using T = std::decay_t<decltype(body)>;
            if constexpr (std::is_same_v<T, bus::CaptureCompleted>) {
                if (broker) broker->handleCaptureCompleted(body);
            } else if constexpr (std::is_same_v<T, bus::ResourcesProvisioned>) {
                // Director side: write the Renderer-allocated slot back
                // onto the VideoTexture component. The clip is stamped
                // with descriptorSlot=UINT32_MAX between
                // ProvisionClipResources publish and this reply -- the
                // compositor's isAllocated() guard skips the clip during
                // that window (one tick under sequential ticking).
                const auto entity = static_cast<entt::entity>(body.entity);
                if (auto* videoTex = m_registry.try_get<VideoTexture>(entity)) {
                    if (body.ok && body.descriptorSlot >= 0) {
                        videoTex->descriptorSlot =
                            static_cast<uint32_t>(body.descriptorSlot);
                    } else {
                        std::cerr << "[Engine] ResourcesProvisioned failed for entity="
                                  << static_cast<uint32_t>(entity)
                                  << ": " << body.errorMessage << std::endl;
                    }
                }
            }
            // Other R2D event types (DeviceLost, FrameDropped) arrive on
            // future subtasks. Ignored for now.
        }, *msg);
    });
}

} // namespace entity
