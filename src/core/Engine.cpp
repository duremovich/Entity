#include "entity/core/Engine.hpp"
#include "entity/profile/Tracy.hpp"
#include "entity/bus/InMemoryMessageTransport.hpp"
#include "entity/bus/Message.hpp"
#include "entity/bus/Serialization.hpp"
#include "entity/director/CaptureBroker.hpp"
#include "entity/director/Director.hpp"
#include "entity/effects/EffectKindRegistry.hpp"
#include "entity/renderer/Renderer.hpp"
#include "entity/director/PlaybackTimeAuthority.hpp"
#include "entity/director/SectionScheduler.hpp"
#include "entity/renderer/PlaybackPresenter.hpp"
#include "entity/render/IRenderer.hpp"
#include "entity/render/D3D12Renderer.hpp"
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
#include "entity/ui/PropsWindow.hpp"
#include "entity/ui/CueListWindow.hpp"
#include "entity/ui/LayersWindow.hpp"
#include "entity/systems/TimelineSystem.hpp"
#include "entity/systems/CompositorSystem.hpp"
#include "entity/systems/AnimationSystem.hpp"
#include "entity/systems/GenerativeSystem.hpp"
#include "entity/systems/DecodeSystem.hpp"
#include "entity/input/InputBus.hpp"
#include "entity/media/Decoder.hpp"
#include "entity/media/DecodedFrame.hpp"
#include "entity/color/OcioManager.hpp"
#include "entity/media/FrameCache.hpp"
#include "entity/project/ProjectSerializer.hpp"
#include "entity/project/ProjectManager.hpp"
#include "entity/project/ContentScanner.hpp"
#include "entity/media/MediaProbeWorker.hpp"
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
#include "entity/components/Layer.hpp"
#include "entity/components/ObjectAnimationLayer.hpp"
#include "entity/components/EffectChainRenderTargets.hpp"
#include "entity/components/GenerativeLayer.hpp"
#include "entity/components/MunchersGameState.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/components/Screen.hpp"
#include "entity/components/Model.hpp"
#include "entity/media/ObjLoader.hpp"
#include "entity/core/CrashLogger.hpp"
#include <GLFW/glfw3.h>
#include <cmath>

#include <iostream>
#include <fstream>
#include <filesystem>
#include <cassert>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

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

    // Nudge the window size to force GLFW to fire its size/framebuffer
    // callbacks once before ImGui starts rendering. On Windows with display
    // scaling != 100%, the very first frame after window creation can land
    // with stale sizes — ImGui's DisplaySize hasn't yet caught up to the
    // DPI-scaled framebuffer, so mouse coords and rendering disagree until
    // the user manually resizes the window. The nudge below fires the
    // necessary callbacks immediately.
    if (!headless) {
        int nw, nh;
        glfwGetWindowSize(m_window, &nw, &nh);
        glfwSetWindowSize(m_window, nw + 1, nh);
        glfwSetWindowSize(m_window, nw, nh);
    }

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

    // GPU mesh lifecycle: deferred-free on model destruction.
    m_registry.on_destroy<Model>().connect<&Engine::onModelDestroyed>(this);

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
    m_generativeSystem  = m_director->getGenerativeSystem();
    m_timeAuthority     = m_director->getTimeAuthority();
    m_sectionScheduler  = m_director->getSectionScheduler();

    // Per-layer effects registry (issue #54). Built once at startup with
    // the engine-shipped effect catalog; pointers propagated to both the
    // editor-side bake (PlaybackTimeAuthority) and the show-side renderer
    // (D3D12Renderer PSO cache).
    m_effectKindRegistry = std::make_unique<effects::EffectKindRegistry>();
    m_effectKindRegistry->registerBuiltins();
    if (m_timeAuthority) {
        m_timeAuthority->setEffectKindRegistry(m_effectKindRegistry.get());
    }
    if (auto* d3d = m_rendererService ? m_rendererService->getD3D12Renderer() : nullptr) {
        d3d->setEffectKindRegistry(m_effectKindRegistry.get());
    }

    // Cross-system input channel bus. Constructed here (before any plugin
    // worker spins up that might write to it) and handed to systems +
    // plugins that need it. Plugins access it via EnginePluginContext
    // (future hook); the editor keyboard handler and SetInputChannel
    // command write to it directly.
    m_inputBus = std::make_unique<InputBus>();
    if (m_generativeSystem) {
        m_generativeSystem->setInputBus(m_inputBus.get());
    }
    std::cout << "  Director initialized (Timeline + ProjectManager + "
                 "TranscodeManager + CommandDispatcher + AnimationSystem + "
                 "PlaybackTimeAuthority)" << std::endl;

    if (auto* compositor = m_rendererService->getCompositorSystem()) {
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

    // Wire transport into CompositorSystem so it can post ScreenRenderTargetAllocated
    // R2D replies from the show thread instead of writing the registry directly.
    if (auto* compositor = m_rendererService->getCompositorSystem()) {
        compositor->setTransport(m_transport.get());
    }

    // Wire transport into OutputManager so assignDisplay routes swap-chain
    // teardown/rebuild through the show thread (SetOutputEnabled D2R), avoiding
    // TDR from calling destroyOutputWindow on the editor thread mid-frame.
    if (m_outputManager) {
        m_outputManager->setTransport(m_transport.get());
    }

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

    // Load named workspaces from disk and take over ImGui ini handling
    // before any window registration. ImGui auto-loads ini on the first
    // frame; loadWorkspaces() disables that and points us at workspaces.json
    // instead. Active-workspace activation runs further down once windows
    // are registered.
    m_windowManager->loadWorkspaces();
    m_windowManager->setActiveWorkspaceChangedCallback(
        [this](const std::string& name) {
            if (m_settings.activeWorkspace == name) return;
            m_settings.activeWorkspace = name;
            if (!saveSettings(m_settings)) {
                std::cerr << "[Engine] Could not persist activeWorkspace to settings.json"
                          << std::endl;
            }
        });

    // Lend OcioManager to the Preferences dialog so its Color section can
    // populate color-space / display / view dropdowns from the active OCIO
    // config. Renderer service owns OcioManager -- it outlives the
    // WindowManager.
    if (m_ocioManager) {
        m_windowManager->setOcioManager(m_ocioManager);
    }

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

    // ADR-0009 — File > Close Project. Disabled when no project is open
    // (PM has no path) so the menu item is honest about what it does.
    m_windowManager->setCanCloseProjectCallback([this]() {
        return m_projectManager && !m_projectManager->projectPath().empty();
    });
    m_windowManager->setCloseProjectCallback([this]() {
        this->closeProject();
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
        timelineWidget->setOALayerDropCallback([this](int trackIndex, FrameNumber startFrame, FrameNumber duration) {
            // Find the first Screen entity to use as default target (user can change in Properties)
            entt::entity target = entt::null;
            auto screenView = m_registry.view<Screen>();
            if (!screenView.empty()) target = *screenView.begin();
            entt::entity created = this->createObjectAnimationLayer(target, trackIndex, startFrame, duration);
            if (created != entt::null && m_timeline) {
                m_timeline->setSelectedClip(created);
            }
        });
        timelineWidget->setClipLayerDropCallback([this](int trackIndex, FrameNumber startFrame, FrameNumber duration) {
            entt::entity created = this->createEmptyClipLayer(trackIndex, startFrame, duration);
            if (created != entt::null && m_timeline) {
                m_timeline->setSelectedClip(created);
            }
        });
        timelineWidget->setGenerativeLayerDropCallback([this](int trackIndex, FrameNumber startFrame, FrameNumber duration) {
            // Resolve target screen: first Screen entity in the registry, same
            // policy as the OA drop callback. The user retargets via Properties
            // once the panel for generative layers exists.
            entt::entity target = entt::null;
            auto screenView = m_registry.view<Screen>();
            if (!screenView.empty()) target = *screenView.begin();
            entt::entity created = this->createMuncherLayer(target, trackIndex, startFrame, duration);
            if (created != entt::null && m_timeline) {
                m_timeline->setSelectedClip(created);
            }
        });
        // Phase A — ruler-menu cue ops route through the dispatcher.
        timelineWidget->setCommandDispatcher(m_commandDispatcher);
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
    m_windowManager->registerWindow(std::make_unique<PropsWindow>(this));
    m_windowManager->registerWindow(std::make_unique<CueListWindow>(this));
    m_windowManager->registerWindow(std::make_unique<LayersWindow>(this));

    // Apply the user's last-active workspace now that all windows are
    // registered. activateWorkspace() restores the saved ImGui ini, the
    // layout-lock state, and which windows were toggled off via the
    // Windows menu. Falls back to "Default" if the named workspace was
    // since deleted (WorkspaceStore guarantees Default always exists).
    m_windowManager->activateWorkspace(m_settings.activeWorkspace);

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

    // #27 — content scanner is constructed once and reused across projects.
    // Started in loadProject(), stopped in closeProject(). The launcher
    // path doesn't need it.
    m_contentScanner = std::make_unique<ContentScanner>();

    // Off-thread FFmpeg metadata probe. Resolver runs on the calling
    // thread (always main thread by contract), so the worker thread
    // never touches ProjectManager state. resolveMediaPath is a const
    // pass-through to ProjectManager::decoderPathFor.
    m_probeWorker = std::make_unique<MediaProbeWorker>(
        [this](const std::string& storedPath) -> std::string {
            return resolveMediaPath(storedPath);
        });

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

    // Join the show thread BEFORE any subsystem teardown. The show thread
    // holds raw pointers into m_renderer, m_outputManager, etc.; tearing
    // those down while the thread is still running causes use-after-free.
    m_showStopRequested.store(true, std::memory_order_release);
    if (m_showThread.joinable()) {
        m_showThread.join();
    }

    // Fire plugin shutdown hooks first, before any subsystem teardown. Plugins
    // that spawned worker threads use this to join them while the dispatcher
    // and bus are still alive (a worker that races a torn-down engine would
    // crash on use-after-free).
    for (auto* hook : m_pluginShutdownHooks) {
        if (hook) {
            try { hook(); }
            catch (...) { /* hooks are noexcept by contract; swallow anyway */ }
        }
    }
    m_pluginShutdownHooks.clear();

    // Cancel + join any in-flight transcode workers before we tear down
    // FFmpeg state or systems they might be touching.
    if (m_transcodeManager) {
        m_transcodeManager->joinAll();
    }

    // Join the metadata probe worker before any project state goes
    // away. The worker thread itself doesn't touch ProjectManager (the
    // resolver runs on main), but reset() blocks until the in-flight
    // probe finishes, so doing it early keeps shutdown ordered.
    m_probeWorker.reset();

    // Flush workspace state to disk while ImGui is still alive — the
    // Renderer service teardown below destroys the ImGui context, and
    // WindowManager::shutdown() captures the current dock layout via
    // ImGui::SaveIniSettingsToMemory. The destructor's repeat call later
    // is a no-op (saveActiveWorkspace clears the active-name guard).
    if (m_windowManager) {
        m_windowManager->shutdown();
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
    m_generativeSystem   = nullptr;
    m_commandDispatcher  = nullptr;
    m_inputBus.reset();
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

void Engine::paintLauncherLoadingOverlay(const std::filesystem::path& path) {
    const std::string projectName = path.filename().string();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowFocus();
    ImGui::SetNextWindowBgAlpha(0.95f);
    ImGui::Begin("##launcher_loading", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoNav);
    ImGui::TextUnformatted("Loading project...");
    ImGui::Spacing();
    ImGui::TextDisabled("%s", projectName.c_str());
    ImGui::End();
}

void Engine::requestProjectLoad(const std::filesystem::path& filepath) {
    // Stage the path; render() runs the two-frame handshake.
    m_pendingFileAssociationLoad = filepath;
    m_fileAssocPhase = FileAssocPhase::Idle;
}

void Engine::renderProjectLoadModal() {
    if (!m_showProjectLoadModal) return;
    if (!m_probeWorker) {
        m_showProjectLoadModal = false;
        return;
    }
    // Done — close on the same frame as we'd otherwise have opened.
    if (m_probeWorker->pendingCount() == 0) {
        m_showProjectLoadModal = false;
        if (ImGui::IsPopupOpen("Loading project")) {
            ImGui::CloseCurrentPopup();
        }
        return;
    }

    if (!ImGui::IsPopupOpen("Loading project")) {
        ImGui::OpenPopup("Loading project");
    }

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Loading project", nullptr,
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings)) {
        const std::filesystem::path& projPath = getProjectPath();
        const std::string projName = projPath.filename().string();
        if (!projName.empty()) {
            ImGui::TextDisabled("%s", projName.c_str());
            ImGui::Spacing();
        }
        ImGui::TextUnformatted("Indexing media...");
        ImGui::Spacing();

        const std::size_t total = m_probeWorker->totalEnqueuedSinceReset();
        const std::size_t done  = m_probeWorker->doneCount();
        const float frac = total > 0
            ? static_cast<float>(done) / static_cast<float>(total)
            : 0.0f;
        char label[64];
        std::snprintf(label, sizeof(label), "%zu / %zu", done, total);
        ImGui::ProgressBar(frac, ImVec2(300.0f, 0.0f), label);

        ImGui::EndPopup();
    }
}

void Engine::renderIndexingToast() {
    // Modal owns the screen during initial load; don't double up.
    if (m_showProjectLoadModal) return;
    if (!m_probeWorker) return;
    if (m_probeWorker->pendingCount() == 0) return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 padding(12.0f, 12.0f);
    const ImVec2 anchor(vp->WorkPos.x + vp->WorkSize.x - padding.x,
                        vp->WorkPos.y + vp->WorkSize.y - padding.y);
    ImGui::SetNextWindowPos(anchor, ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::Begin("##indexing_toast", nullptr,
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav);

    const std::size_t total = m_probeWorker->totalEnqueuedSinceReset();
    const std::size_t done  = m_probeWorker->doneCount();
    char text[96];
    std::snprintf(text, sizeof(text), "Indexing %zu / %zu media files...", done, total);
    ImGui::TextUnformatted(text);
    const float frac = total > 0
        ? static_cast<float>(done) / static_cast<float>(total)
        : 0.0f;
    ImGui::ProgressBar(frac, ImVec2(220.0f, 4.0f), "");

    ImGui::End();
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

    std::cout << "Starting main loop (show thread spawned)..." << std::endl;
    m_running = true;
    m_showStopRequested.store(false, std::memory_order_relaxed);
    m_showThread = std::thread([this]() {
        showThreadMain();
    });

    TracySetProgramName("Entity Media Server");
    tracy::SetThreadName("Editor");

    // Editor-side wall clock. Independent from PlaybackTimeAuthority's clock
    // (which the show thread owns) so the editor can compute its own
    // deltaTime — used by the FPS counter and SectionScheduler::tick — even
    // when the show thread is busy. The two clocks read the same QPC source
    // so they don't drift.
    using EditorClock = std::chrono::steady_clock;
    auto lastEditorTick = EditorClock::now();
    m_editorDeltaTime = 0.0;

    while (m_running && !glfwWindowShouldClose(m_window)) {
        ZoneScopedN("Engine::run iter");

        // Per-iteration editor delta. Clamped to 50ms so a Win32 modal-loop
        // wake-up (resize, title-bar drag) doesn't dump multiple seconds of
        // delta into Timeline::update at once — the show thread covered the
        // stall via its fallback tick.
        {
            const auto now = EditorClock::now();
            const std::chrono::duration<double> delta = now - lastEditorTick;
            m_editorDeltaTime = std::min(delta.count(), 0.050);
            lastEditorTick = now;
            m_lastEditorTickNs.store(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    now.time_since_epoch()).count(),
                std::memory_order_relaxed);
        }

        // FPS tracking (editor-side; measures editor frame rate).
        {
            m_fpsAccumulator += m_editorDeltaTime;
            m_fpsFrameCount++;
            if (m_fpsAccumulator >= 0.5) {
                m_currentFPS = static_cast<uint32_t>(m_fpsFrameCount / m_fpsAccumulator);
                m_fpsAccumulator = 0.0;
                m_fpsFrameCount  = 0;
                const std::string title =
                    "Entity Media Server - Editor | " + std::to_string(m_currentFPS) + " FPS";
                glfwSetWindowTitle(m_window, title.c_str());
                TracyPlot("FPS", static_cast<int64_t>(m_currentFPS));
            }
        }

        // Editor-only work: GLFW events, Editor-affinity commands, ImGui.
        processEvents();

        // Synthetic crash injection for testing CrashLogger.
        // Set ENTITY_FORCE_CRASH_AT_FRAME=N to AV on editor loop iteration N.
        // Runs before the launcher check so it fires in all modes (launcher + normal).
        {
            static int s_forceCrashFrame = -1;
            static bool s_forceCrashChecked = false;
            static uint64_t s_editorLoopIter = 0;
            if (!s_forceCrashChecked) {
                s_forceCrashChecked = true;
                if (const char* v = std::getenv("ENTITY_FORCE_CRASH_AT_FRAME")) {
                    s_forceCrashFrame = std::atoi(v);
                }
            }
            if (s_forceCrashFrame >= 0) {
                if (s_editorLoopIter++ == static_cast<uint64_t>(s_forceCrashFrame)) {
                    volatile int* p = nullptr;
                    *p = 0; // intentional AV — triggers SEH filter
                }
            }
        }

        // Synthetic device-lost injection for testing the relaunch path.
        // Set ENTITY_FORCE_DEVICE_LOST_AT_FRAME=N to simulate device removal
        // on show-thread frame N. Use N >= 10 so the show thread has presented
        // at least one frame (required for the cold-start gate to have passed).
        {
            static int s_forceDeviceLostFrame = -1;
            static bool s_forceDeviceLostChecked = false;
            if (!s_forceDeviceLostChecked) {
                s_forceDeviceLostChecked = true;
                if (const char* v = std::getenv("ENTITY_FORCE_DEVICE_LOST_AT_FRAME")) {
                    s_forceDeviceLostFrame = std::atoi(v);
                }
            }
            if (s_forceDeviceLostFrame >= 0 && m_renderer) {
                uint64_t sf = m_showFrameCount.load(std::memory_order_relaxed);
                if (sf >= static_cast<uint64_t>(s_forceDeviceLostFrame)) {
                    s_forceDeviceLostFrame = -1; // fire once
                    if (auto* d3d = m_rendererService ? m_rendererService->getD3D12Renderer() : nullptr) {
                        d3d->setDeviceLostForTesting();
                        std::cerr << "[Engine] ENTITY_FORCE_DEVICE_LOST_AT_FRAME: "
                                  << "synthetic device-lost set at show frame " << sf << std::endl;
                    }
                }
            }
        }

        // Stage 3c: condvar gate removed. Show thread paces itself via Present(1,0)
        // or QPC sleep_until; editor and show run at their own rates.

        // ADR-0009 — Launcher mode: editor renders the launcher only.
        if (m_showLauncher) {
            render();
            FrameMarkNamed("Editor");
            continue;
        }

        // Editor-affinity commands (undo/redo, UI mutations).
        // Editor thread is sole registry writer; no lock needed.
        if (m_commandDispatcher) {
            m_commandDispatcher->processQueue(*this, Affinity::Editor);
        }
        // Editor-side simulation work: timeline, section scheduler, decode.
        update();

        // Editor-half of scene snapshot: snapshot Screen/Surface/Projector/
        // Output registry views and publish latest-wins on D2R.
        if (m_transport && m_timeAuthority) {
            ZoneScopedN("buildSceneSnapshot");
            bus::SceneSnapshot snap;
            m_timeAuthority->buildSceneSnapshot(snap);
            m_transport->send(bus::Direction::D2R, bus::serialize(bus::Message{snap}));
        }

        // Editor-side render: back buffer, ImGui, editor swap chain.
        render();

        // Drain R2D replies from the show thread (CaptureCompleted, etc).
        drainRendererToDirector();

        if (m_commandDispatcher && m_commandDispatcher->scriptReadyToFinish()) {
            auto* broker = m_director ? m_director->getCaptureBroker() : nullptr;
            if (!broker || !broker->hasPending()) {
                m_commandDispatcher->finishScript();
            }
        }

        // Bail on GPU device-lost.
        // Send bus::DeviceLost, then drain immediately so the autosave +
        // relaunch-flag fire before we leave the loop. A second drain here is
        // safe — drainRendererToDirector() is idempotent and was already called
        // above for the show-thread's normal R2D replies.
        if (m_renderer && m_renderer->isDeviceLost()) {
            std::cerr << "[Engine] D3D12 device lost — exiting main loop." << std::endl;
            if (m_transport && !m_deviceLostPosted) {
                m_deviceLostPosted = true;
                bus::DeviceLost msg{};
                msg.hresult = static_cast<int>(m_renderer->getDeviceLostReason());
                msg.reason  = "D3D12 device removed or hung — see stderr for details";
                m_transport->send(bus::Direction::R2D,
                                  bus::serialize(bus::Message{msg}));
                drainRendererToDirector(); // process DeviceLost → autosave + relaunch flag
            }
            m_running = false;
        }

        autoSaveTick(m_timeAuthority ? m_timeAuthority->getDeltaTime() : 0.0);
        pollTranscodes();
        pollProbeCompletions();

        FrameMarkNamed("Editor");
    }

    // Stop the show thread.
    m_showStopRequested.store(true, std::memory_order_release);
    if (m_showThread.joinable()) {
        m_showThread.join();
    }

    std::cout << "Main loop exited." << std::endl;
}

void Engine::showThreadMain() {
    tracy::SetThreadName("Show");

    // ShowClock: QPC-based 60 Hz pacing used when no physical output provides
    // vsync via Present(1,0). Keeps the show loop from spinning flat-out when
    // idling in editor-preview-only mode.
    using Clock = std::chrono::steady_clock;
    constexpr auto kShowPeriod = std::chrono::nanoseconds(16'666'667); // ~60 Hz
    auto nextTick = Clock::now() + kShowPeriod;

    while (!m_showStopRequested.load(std::memory_order_acquire)) {
        ZoneScopedN("Show iter");
        const auto showIterStart = Clock::now();
        // Stop spinning on a dead device. The editor thread owns the autosave +
        // relaunch wiring; let it proceed without interference from the show thread
        // issuing more D3D12 calls on a lost device.
        if (m_renderer && m_renderer->isDeviceLost()) {
            break;
        }

        // Stage 3c: show thread paces itself. No condvar gate with the editor.
        // Show-side timing update.
        if (m_timeAuthority) {
            m_timeAuthority->updateTiming();
        }

        // Timeline now ticks on the show thread under all conditions
        // (Phase D move, May 2026). Show thread owns Timeline.m_currentTime
        // advancement so playback pace stays decoupled from editor health —
        // Win32 modal drag, slow ImGui frame, heavy project load all leave
        // playback advancing at a stable 60 Hz. Editor still owns scrub /
        // seek / play / pause / project load via Timeline's setters and the
        // Director command path. Timeline::update no-ops when not Playing,
        // so it's safe to call unconditionally.
        if (m_timeline && m_timeAuthority) {
            m_timeline->update(m_timeAuthority->getDeltaTime());
        }

        // DecodeSystem still ticks on the editor thread (Engine::update).
        // Show-thread fallback for full editor stalls: when the editor is
        // pinned (>50ms heartbeat staleness — modal resize loops, hard
        // ImGui block), tick DecodeSystem here too so workers' targetFrame
        // keeps advancing. Without this the FrameRingBuffer stops filling
        // and PlaybackPresenter re-uploads the same media frame.
        if (m_decodeSystem && m_timeAuthority) {
            const int64_t lastEditor = m_lastEditorTickNs.load(std::memory_order_relaxed);
            const int64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            constexpr int64_t kEditorStaleNs = 50'000'000; // 50 ms
            if (lastEditor != 0 && (nowNs - lastEditor) > kEditorStaleNs) {
                m_decodeSystem->update(m_registry,
                    static_cast<float>(m_timeAuthority->getDeltaTime()));
            }
        }

        // Consume any SceneSnapshot published by the editor half. Other D2R
        // messages (SetOutputEnabled, ApplySettings, ProvisionClipResources, ...)
        // are deferred and re-published so the post-beginShowFrame drain picks
        // them up — drain() empties the queue, so anything we don't requeue here
        // is silently dropped.
        if (m_transport) {
            ZoneScopedN("D2R drain (pre-frame)");
            std::vector<std::vector<std::uint8_t>> deferred;
            m_transport->drain(bus::Direction::D2R, [this, &deferred](std::vector<std::uint8_t>&& bytes) {
                auto msg = bus::deserialize(bytes);
                if (!msg) {
                    // Malformed payload — push back as-is rather than dropping.
                    deferred.push_back(std::move(bytes));
                    return;
                }
                bool isSceneSnapshot = false;
                std::visit([this, &isSceneSnapshot](auto& body) {
                    using T = std::decay_t<decltype(body)>;
                    if constexpr (std::is_same_v<T, bus::SceneSnapshot>) {
                        m_cachedSceneSnapshot = std::move(body);
                        isSceneSnapshot = true;
                    }
                }, *msg);
                if (!isSceneSnapshot) deferred.push_back(std::move(bytes));
            });
            for (auto& bytes : deferred) {
                m_transport->send(bus::Direction::D2R, std::move(bytes));
            }
        }

        // Launcher mode: show thread idles at 60 Hz with an empty RenderFrame.
        if (m_showLauncher) {
            m_showFrameCount.fetch_add(1, std::memory_order_release);
            FrameMarkNamed("Show");
            std::this_thread::sleep_until(nextTick);
            nextTick += kShowPeriod;
            continue;
        }

        // Drain Show-affinity commands (transport, playback control).
        // Editor-affinity commands are skipped here and remain in the queue
        // for the editor thread to consume on its next tick.
        if (m_commandDispatcher) {
            m_commandDispatcher->processQueue(*this, Affinity::Show);
        }

        // Build per-tick RenderFrame. No registry lock needed here — all clip/
        // layer/phase data comes from m_cachedSceneSnapshot::clipCatalog, populated
        // by buildSceneSnapshot on the editor thread. Zero registry reads.
        bus::RenderFrame lastRF;
        if (m_transport && m_timeAuthority && m_playbackPresenter) {
            ZoneScopedN("buildRenderFrame");
            m_timeAuthority->buildRenderFrame(lastRF, m_cachedSceneSnapshot);
            m_transport->send(bus::Direction::D2R, bus::serialize(bus::Message{lastRF}));
        }

        // Show frame: copy uploads + compositor + output draws + output Present.
        if (m_renderer) {
            drainCaptureRequestsPreFrame();
            m_renderer->beginShowFrame();

            // Drain D2R *after* beginShowFrame: PlaybackPresenter::present records
            // CopyTextureRegion onto m_copyCommandList, which beginShowFrame just
            // Reset() and reopened. Draining before beginShowFrame would record
            // into a closed command list and the Reset would wipe the recordings.
            if (m_transport) {
                m_transport->drain(bus::Direction::D2R, [this, &lastRF](std::vector<std::uint8_t>&& bytes) {
                    auto msg = bus::deserialize(bytes);
                    if (!msg) return;
                    std::visit([this, &lastRF](auto& body) {
                        using T = std::decay_t<decltype(body)>;
                        if constexpr (std::is_same_v<T, bus::RenderFrame>) {
                            if (m_playbackPresenter) m_playbackPresenter->present(body);
                            lastRF = body;
                        } else if constexpr (std::is_same_v<T, bus::SceneSnapshot>) {
                            m_cachedSceneSnapshot = std::move(body);
                        } else if constexpr (std::is_same_v<T, bus::SetOutputEnabled>) {
                            if (m_outputManager) {
                                m_outputManager->setOutputEnabled(
                                    static_cast<entt::entity>(body.entity), body.enabled);
                            }
                        } else if constexpr (std::is_same_v<T, bus::ApplySettings>) {
                            if (m_frameCache) {
                                m_frameCache->setMaxBytes(static_cast<size_t>(body.frameCacheBytes));
                            }
                        } else if constexpr (std::is_same_v<T, bus::ProvisionClipResources>) {
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
                    }, *msg);
                });
            }

            // Stage 4: compositor->update no longer writes registry.
            // ensureScreenRenderTarget posts ScreenRenderTargetAllocated R2D
            // so the editor drains and writes Screen::renderTargetSlot itself.
            // No registry lock needed here.
            if (auto* compositor = m_rendererService ? m_rendererService->getCompositorSystem() : nullptr) {
                double deltaTime = m_timeAuthority ? m_timeAuthority->getDeltaTime() : 0.0;
                compositor->update(lastRF, m_registry, static_cast<float>(deltaTime));
            }
            if (m_outputManager) {
                m_outputManager->renderOutputs(lastRF);
            }

            m_renderer->endShowFrame();
        }

        if (m_timeAuthority) {
            m_timeAuthority->incrementFrameCount();
        }

        m_showFrameCount.fetch_add(1, std::memory_order_release);

        {
            const auto showIterEnd = Clock::now();
            [[maybe_unused]] const double showMs = std::chrono::duration<double, std::milli>(
                showIterEnd - showIterStart).count();
            TracyPlot("Show frame ms", showMs);
        }

        if (m_frameCache) {
            auto [hits, misses] = m_frameCache->consumeAccessCounters();
            [[maybe_unused]] const uint64_t total = hits + misses;
            [[maybe_unused]] const double hitRate = total > 0 ? (100.0 * hits / total) : 0.0;
            TracyPlot("FrameCache bytes used", static_cast<int64_t>(m_frameCache->bytesUsed()));
            TracyPlot("FrameCache hit rate %", hitRate);
            TracyPlot("FrameCache entries", static_cast<int64_t>(m_frameCache->entryCount()));
        }

        FrameMarkNamed("Show");

        // QPC pacing: when the show frame finished before the next 60 Hz tick
        // (i.e. no physical output's Present(1,0) consumed the time), sleep
        // until the next boundary so we don't spin. If the frame took longer
        // than one period we skip the sleep and let the next tick run immediately.
        const auto now = Clock::now();
        if (now < nextTick) {
            std::this_thread::sleep_until(nextTick);
        }
        nextTick += kShowPeriod;
    }
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

void Engine::pollProbeCompletions() {
    if (!m_probeWorker || !m_projectManager) return;
    auto fresh = m_probeWorker->drainCompletedFreshProbes();
    if (fresh.empty()) return;
    for (auto& cp : fresh) {
        auto* entry = m_projectManager->findEntry(cp.storedPath);
        if (!entry) continue;
        // Worker captures size+mtime at probe time. Treat (0, 0) as
        // "stat failed" — skip the writeback so we re-probe next
        // session rather than persist a useless sentinel.
        if (cp.sizeBytes <= 0) continue;
        entry->lastProbeSizeBytes = cp.sizeBytes;
        entry->lastProbeMtimeUnix = cp.mtimeUnix;
        entry->cachedProbe        = std::move(cp.info);
    }
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

std::string Engine::resolveMediaPath(const std::string& storedPath) const {
    if (!m_projectManager) return storedPath;
    return m_projectManager->decoderPathFor(storedPath);
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

void Engine::resolvePendingImport(ImportMode mode,
                                   const std::string& subfolder,
                                   bool transcode,
                                   bool rememberStorage,
                                   bool rememberTranscode) {
    if (!m_pendingImport) return;
    PendingImport p = *m_pendingImport;   // copy before clearing
    m_pendingImport.reset();

    // Persist user preferences before doing the work — even if the
    // import itself fails, the "don't ask again" choice should stick.
    if (rememberStorage) {
        m_settings.importStoragePolicy =
            (mode == ImportMode::Copy)
                ? Settings::ImportStoragePolicy::AlwaysCopy
                : Settings::ImportStoragePolicy::AlwaysLink;
        publishActiveSettings(m_settings);
        saveSettings(m_settings);  // best-effort; failure already logs
    }
    if (rememberTranscode && p.transcodeEligible && m_projectManager) {
        m_projectManager->setNonHapImportPolicy(
            transcode ? ProjectManager::NonHapImportPolicy::AlwaysTranscode
                      : ProjectManager::NonHapImportPolicy::NeverTranscode);
    }

    // Now that storage is resolved, actually copy/link the file. Up to
    // this point p.sourceFilePath is still the user-picked source.
    ProjectManager::PathKind importedKind = ProjectManager::PathKind::Linked;
    const std::string canonicalPath =
        applyImportMode(p.sourceFilePath, mode, subfolder, &importedKind);
    if (canonicalPath.empty()) {
        std::cerr << "[Engine] resolvePendingImport: import failed for "
                  << p.sourceFilePath << std::endl;
        return;
    }

    if (m_projectManager) {
        m_projectManager->addMediaFile(canonicalPath, importedKind);
    }
    if (m_probeWorker) m_probeWorker->enqueue(canonicalPath);

    if (p.transcodeEligible && transcode) {
        scheduleTranscode(canonicalPath, p.mediaType);
    } else {
        ingestVideoClip(canonicalPath, p.mediaType);
    }
}

const std::filesystem::path& Engine::getProjectPath() const {
    static const std::filesystem::path kEmpty;
    return m_projectManager ? m_projectManager->projectPath() : kEmpty;
}

void Engine::processEvents() {
    ZoneScopedN("processEvents");
    glfwPollEvents();

    // TODO: Handle input events
}

void Engine::update() {
    ZoneScopedN("Engine::update");
    auto t0 = std::chrono::high_resolution_clock::now();
    // Timeline.m_currentTime advancement moved to the show thread (see
    // Engine::showThreadMain). The editor used to advance Timeline by
    // m_timeAuthority->getDeltaTime() per editor tick, which coupled
    // playback pace to editor health and produced half-speed playback
    // during Win32 modal drags. Phase D move: show thread owns the per-
    // tick advance under all conditions.
    //
    // The editor still uses deltaTime to drive systems whose state lives
    // in the registry (SectionScheduler, AnimationSystem, DecodeSystem).
    // Those tick on the editor thread so registry writes stay on the sole
    // writer. They read Timeline's atomic m_currentTime fresh each tick.
    //
    // Use the editor-thread wall-clock delta, NOT m_timeAuthority->getDeltaTime()
    // (which is the show-thread's per-show-frame delta, refreshed only ~60Hz).
    // SectionScheduler::advanceContinuation integrates dt over editor ticks,
    // so feeding it the show-thread's cached dt makes phase advance at
    // editorTicksPerSecond × ~16.67ms × clip.fps instead of wallSeconds ×
    // clip.fps — section-break continuation runs at editor-loop speed.
    double deltaTime = m_editorDeltaTime;

    // Section break detection runs against the (show-thread-advanced)
    // timeline frame so AnimationSystem and DecodeSystem (below) see the
    // snapped frame when the playhead just crossed a break.
    if (m_sectionScheduler) {
        m_sectionScheduler->tick(deltaTime);
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    // NOTE: updateClipVideos() moved to render() - must be called AFTER beginShowFrame()
    // because beginShowFrame() resets the show command list

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
    // beginShowFrame/endShowFrame so its draw calls land on a recording command
    // list. Order: Animation (Director) -> Decode (Renderer). DecodeSystem
    // doesn't depend on Animation; the order is "Director-side first,
    // Renderer-side second" to match the eventual director.tick() ->
    // renderer.tick() split (subtask 8).
    if (m_animationSystem) {
        m_animationSystem->update(m_registry, static_cast<float>(deltaTime));
    }
    // Local input sources → Muncher input bus. Joystick takes priority
    // over keyboard; both share the same "only WRITE while active"
    // guard so scripts / OSC plugins can drive the channels when no
    // local input source is engaged. Single namespace (muncher.input.x/.y)
    // means a gameplay system never cares which source produced the
    // value.
    if (m_inputBus) {
        // 1) Joystick. Polled every tick on slot 1 only (every-slot scans
        //    every tick triggered enough per-tick overhead to destabilize
        //    the screenshot pipeline under heavy compositor load). GLFW's
        //    stick-Y is -1=up / +1=down, matching the image-coord
        //    convention used by the show-side render — no flip needed.
        //    Small deadzone on top of the gameplay-side deadzone so
        //    noisy sticks don't dribble input.
        float jx = 0.0f, jy = 0.0f;
        bool  joystickActive = false;
        if (glfwJoystickPresent(GLFW_JOYSTICK_1)) {
            int axesCount = 0;
            const float* axes = glfwGetJoystickAxes(GLFW_JOYSTICK_1, &axesCount);
            if (axes && axesCount >= 2) {
                constexpr float kJoyDeadzone = 0.15f;
                jx = (std::fabs(axes[0]) > kJoyDeadzone) ? axes[0] : 0.0f;
                jy = (std::fabs(axes[1]) > kJoyDeadzone) ? axes[1] : 0.0f;
                joystickActive = (jx != 0.0f || jy != 0.0f);
            }
        }

        // 2) Keyboard. Gated on ImGui WantTextInput so typing in fields
        //    doesn't smear into movement.
        float kx = 0.0f, ky = 0.0f;
        bool  keyboardActive = false;
        if (m_window && !ImGui::GetIO().WantTextInput) {
            const bool w     = glfwGetKey(m_window, GLFW_KEY_W)     == GLFW_PRESS;
            const bool s     = glfwGetKey(m_window, GLFW_KEY_S)     == GLFW_PRESS;
            const bool a     = glfwGetKey(m_window, GLFW_KEY_A)     == GLFW_PRESS;
            const bool d     = glfwGetKey(m_window, GLFW_KEY_D)     == GLFW_PRESS;
            const bool up    = glfwGetKey(m_window, GLFW_KEY_UP)    == GLFW_PRESS;
            const bool down  = glfwGetKey(m_window, GLFW_KEY_DOWN)  == GLFW_PRESS;
            const bool left  = glfwGetKey(m_window, GLFW_KEY_LEFT)  == GLFW_PRESS;
            const bool right = glfwGetKey(m_window, GLFW_KEY_RIGHT) == GLFW_PRESS;
            kx = ((d || right) ? 1.0f : 0.0f) - ((a || left) ? 1.0f : 0.0f);
            ky = ((s || down)  ? 1.0f : 0.0f) - ((w || up)   ? 1.0f : 0.0f);
            keyboardActive = (kx != 0.0f) || (ky != 0.0f);
        }

        // 3) Merge + write. Joystick > keyboard. While neither is active,
        //    issue one trailing zero-write (so the player stops cleanly
        //    when the local source releases) and then stop writing — leaves
        //    the channel free for scripts / OSC to drive.
        if (joystickActive) {
            m_inputBus->setFloat("muncher.input.x", jx);
            m_inputBus->setFloat("muncher.input.y", jy);
            m_muncherKeyboardActive = true;  // re-used to track "any local source active"
        } else if (keyboardActive) {
            m_inputBus->setFloat("muncher.input.x", kx);
            m_inputBus->setFloat("muncher.input.y", ky);
            m_muncherKeyboardActive = true;
        } else if (m_muncherKeyboardActive) {
            m_inputBus->setFloat("muncher.input.x", 0.0f);
            m_inputBus->setFloat("muncher.input.y", 0.0f);
            m_muncherKeyboardActive = false;
        }
    }
    // GenerativeSystem ticks after AnimationSystem so any animated parameter
    // on a generative layer (e.g. a future opacity track) is settled before
    // the sim reads it. Same threading constraint as AnimationSystem — editor
    // thread only; show thread reads the baked snapshot (Phase 2 wiring).
    if (m_generativeSystem) {
        m_generativeSystem->update(m_registry, static_cast<float>(deltaTime));
    }
    // #27 — drain ContentScanner deltas just before DecodeSystem so any
    // freshly-discovered file is decoder-ready this same frame.
    drainContentScannerDeltas();
    if (m_decodeSystem) {
        m_decodeSystem->update(m_registry, static_cast<float>(deltaTime));
    }

    // Upload any Model meshes that don't yet have GPU resources.
    // Pacing rules:
    //   1. Cold-start gate: skip until the show thread has presented at least
    //      one frame. This prevents the first-frame TDR where a large mesh
    //      upload races the show thread's first compositor present.
    //   2. Steady-state cap: at most one successful upload per editor tick,
    //      spreading N dirty Models across N ticks instead of one burst.
    //      Failed-upload marker writes still complete this tick.
    if (m_showFrameCount.load(std::memory_order_acquire) < 1) {
        // Show thread hasn't presented yet; defer all mesh uploads.
    } else if (auto* d3d = m_rendererService ? m_rendererService->getD3D12Renderer() : nullptr) {
        auto modelView = m_registry.view<Model>();
        for (auto [e, m] : modelView.each()) {
            if (!m.gpuResourcesValid && !m.gpuUploadFailed && m.mesh.isValid()) {
                uint32_t slot = d3d->uploadMeshImmediate(m.mesh.vertices, m.mesh.indices);
                if (slot != UINT32_MAX) {
                    m.vertexBufferSlot = slot;
                    m.indexBufferSlot  = slot;
                    m.gpuResourcesValid = true;
                    break;  // one successful upload per tick — pace the GPU
                } else {
                    std::cerr << "[Engine] MeshUploader slot pool exhausted for model '"
                              << m.name << "' — mesh will not render" << std::endl;
                    m.gpuUploadFailed = true;
                    // Don't break on failure — just a marker write.
                }
            }
        }
    }

    auto t3 = std::chrono::high_resolution_clock::now();

    // Log timing if any stage took > 50ms
    auto timelineMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    auto legacyDecodeMs = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count();
    auto systemsMs = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();
    auto totalMs = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t0).count();
    const double totalMsD = std::chrono::duration<double, std::milli>(t3 - t0).count();
    TracyPlot("Editor frame ms", totalMsD);

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

uint64_t Engine::getMeshUploadCount() const {
    if (auto* d3d = m_rendererService ? m_rendererService->getD3D12Renderer() : nullptr) {
        return d3d->getMeshUploadCount();
    }
    return 0;
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
    ZoneScopedN("Engine::render");
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
        // File-association entry point (apps/editor/main.cpp -> opens
        // .entity directly via OS double-click): same two-frame
        // handshake as the launcher's Open action, but with no
        // launcher behind it. Frame N: paint the "Loading..." overlay
        // alone and present so the user sees something instead of a
        // blank window during the deserialize freeze. Frame N+1: run
        // the blocking load with that overlay still on the front
        // buffer. On failure, fall back to the launcher (recoverable).
        if (!m_pendingFileAssociationLoad.empty() && !m_showLauncher) {
            m_renderer->beginEditorFrame();
            m_renderer->clear(0.05f, 0.06f, 0.08f, 1.0f);
            m_renderer->beginImGuiFrame();
            paintLauncherLoadingOverlay(m_pendingFileAssociationLoad);
            m_renderer->endImGuiFrame();
            m_renderer->endEditorFrame();

            if (m_fileAssocPhase == FileAssocPhase::Idle) {
                // Frame N — overlay presented, defer the load to N+1
                // so the user gets a dedicated frame of feedback.
                m_fileAssocPhase = FileAssocPhase::OverlayStaged;
                return;
            }
            // Frame N+1 — overlay is on the front buffer. Run the load.
            auto path = std::move(m_pendingFileAssociationLoad);
            m_pendingFileAssociationLoad.clear();
            m_fileAssocPhase = FileAssocPhase::Idle;
            if (!loadProject(path)) {
                std::cerr << "[Engine] file-association load failed for "
                          << path << " — falling back to launcher." << std::endl;
                showLauncher();
            }
            return;
        }

        // ADR-0009 — Launcher mode: skip the compositor + outputs pass
        // entirely; just clear, run an ImGui frame containing the
        // launcher, and present. Avoids touching scene-state-dependent
        // systems (CompositorSystem, OutputManager) before a project
        // exists.
        if (m_showLauncher) {
            m_renderer->beginEditorFrame();
            m_renderer->clear(0.05f, 0.06f, 0.08f, 1.0f);
            m_renderer->beginImGuiFrame();

            // Frame N+1 of the open handshake: a pending load was
            // staged last frame. Paint the overlay alone (no launcher
            // UI — the user already chose), present, *then* run the
            // blocking load. The just-presented frame stays on the
            // swap chain's front buffer for the entire freeze, so the
            // user keeps seeing "Loading..." while the main thread is
            // stalled in loadProject().
            if (!m_pendingLauncherLoad.empty()) {
                paintLauncherLoadingOverlay(m_pendingLauncherLoad);
                m_renderer->endImGuiFrame();
                m_renderer->endEditorFrame();
                auto path = std::move(m_pendingLauncherLoad);
                m_pendingLauncherLoad.clear();
                onLauncherOpenProject(path);
                return;
            }

            if (m_launcher) {
                auto result = m_launcher->render();
                switch (result.action) {
                    case ProjectLauncher::Action::Open:
                        // Frame N: stage the load and paint the overlay
                        // on top of the launcher. The actual load runs
                        // on frame N+1 (above) so the user gets a full
                        // dedicated frame of "Loading..." before the
                        // freeze begins.
                        m_pendingLauncherLoad = result.path;
                        paintLauncherLoadingOverlay(result.path);
                        m_renderer->endImGuiFrame();
                        m_renderer->endEditorFrame();
                        return;
                    case ProjectLauncher::Action::Quit:
                        requestExit();
                        break;
                    case ProjectLauncher::Action::None:
                        break;
                }
            }
            m_renderer->endImGuiFrame();
            m_renderer->endEditorFrame();
            return;
        }

        // Editor frame: back-buffer clear, ImGui, editor Present.
        // Show-side work (copy uploads, compositor, output draws, endShowFrame)
        // now runs in showThreadMain().
        m_renderer->beginEditorFrame();

        // Clear back buffer to the editor background color.
        m_renderer->clear(0.0f, 0.5f, 0.6f, 1.0f);

        // Begin ImGui frame for UI rendering
        m_renderer->beginImGuiFrame();

        // Render window manager (DockSpace and all windows)
        if (m_windowManager) {
            m_windowManager->render();
        }

        // Indexing UI: modal during the post-load probe sweep, toast for
        // live drops once the editor is interactive. The two are mutually
        // exclusive (renderIndexingToast bails if the modal is active).
        renderProjectLoadModal();
        renderIndexingToast();

        // End ImGui frame and render UI
        m_renderer->endImGuiFrame();

        m_renderer->endEditorFrame();
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

    // ESC = panic stop for live performances. If any Physical output is
    // currently driving a display, ESC disables them all so the editor
    // becomes visible again. Otherwise ESC is a no-op — closing the app
    // is reserved for the OS close button / File menu / Alt+F4 so an
    // accidental ESC mid-show can't kill the whole program.
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
        }
        return;
    }

    // Timeline keyboard shortcuts (only if timeline exists)
    if (m_timeline) {
        switch (key) {
            case GLFW_KEY_SPACE:
                // Hybrid GO / play-pause. When the SectionScheduler has parked
                // the playhead at a break, spacebar is GO (advance one frame
                // past the break and resume play). Otherwise it's the standard
                // toggle. Both paths route through CommandDispatcher for
                // recording / scriptability.
                if (m_commandDispatcher) {
                    if (m_timeline && m_timeline->sectionAtBreak()) {
                        m_commandDispatcher->enqueue(std::make_unique<SectionGoCommand>());
                    } else {
                        m_commandDispatcher->enqueue(std::make_unique<TogglePlayPauseCommand>());
                    }
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
                // Delete = Delete selected clip. Routed through the command
                // dispatcher so it lands on the undo stack (Ctrl+Z restores).
                {
                    entt::entity selectedClip = m_timeline->getSelectedClip();
                    if (selectedClip != entt::null && m_commandDispatcher) {
                        m_commandDispatcher->enqueue(std::make_unique<DeleteClipCommand>(
                            static_cast<uint32_t>(selectedClip)));
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

        track.addLayer(clip1);
        track.addLayer(clip2);
        track.addLayer(clip3);

        assert(track.getLayerCount() == 3);
        assert(track.isEmpty() == false);

        track.removeLayer(clip2);
        assert(track.getLayerCount() == 2);

        std::cout << "  ✓ TimelineTrack layer management works" << std::endl;
        std::cout << "  ✓ Layers in track: " << track.getLayerCount() << std::endl;
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

    // #32 — empty subfolder means "drop into content/ root". The legacy
    // unsorted/ default is gone; users organize via the modal's subfolder
    // field (or after-the-fact via Explorer / drag — both pick up via
    // ContentScanner).
    const fs::path targetDir = subfolder.empty() ? contentDir
                                                  : (contentDir / subfolder);
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

    // #32 — collect both policies and decide whether to ask the user.
    // Storage policy lives on Settings (per-machine). Transcode policy
    // lives on ProjectManager (per-project). The unified modal asks
    // whichever decisions aren't already pinned.

    // Storage decision.
    const auto storagePolicy = m_settings.importStoragePolicy;
    bool storageDecided = (storagePolicy != Settings::ImportStoragePolicy::Ask);
    ImportMode resolvedMode = ImportMode::Link;
    if (storagePolicy == Settings::ImportStoragePolicy::AlwaysCopy) {
        resolvedMode = ImportMode::Copy;
    }

    // Transcode decision (only meaningful for non-HAP video that we have
    // a transcoder for; PNG sequences stay on the CPU path).
    const bool transcodeEligible =
        !isHapMediaType(mediaType) &&
        mediaType == MediaType::VideoProRes4444 &&
        m_projectManager && m_transcodeManager;
    bool transcodeDecided = !transcodeEligible;
    bool resolvedTranscode = false;
    if (transcodeEligible && m_projectManager) {
        const auto txPolicy = m_projectManager->nonHapImportPolicy();
        if (txPolicy == ProjectManager::NonHapImportPolicy::AlwaysTranscode) {
            transcodeDecided = true;
            resolvedTranscode = true;
        } else if (txPolicy == ProjectManager::NonHapImportPolicy::NeverTranscode) {
            transcodeDecided = true;
            resolvedTranscode = false;
        }
    }

    // If anything is undecided, defer to the unified modal.
    if (!storageDecided || !transcodeDecided) {
        if (m_pendingImport) {
            std::cerr << "[Engine] Import already awaiting user decision ("
                      << m_pendingImport->sourceFilePath
                      << "); dropping " << filePath << std::endl;
            return;
        }
        PendingImport p;
        p.sourceFilePath    = filePath;
        p.mediaType         = mediaType;
        p.transcodeEligible = transcodeEligible;
        p.storageDecided    = storageDecided;
        p.resolvedMode      = resolvedMode;
        p.transcodeDecided  = transcodeDecided;
        p.resolvedTranscode = resolvedTranscode;
        m_pendingImport = std::move(p);
        std::cout << "[Engine] Awaiting user decision on import: " << filePath << std::endl;
        std::cout << "========================================\n" << std::endl;
        return;
    }

    // Both decisions are pinned — apply silently.
    ProjectManager::PathKind importedKind = ProjectManager::PathKind::Linked;
    // Subfolder defaults to "" (root). The modal is the only path that
    // can specify a subfolder; AlwaysCopy without a modal lands at root
    // by design (user organizes via Explorer / drag, scanner picks up).
    const std::string canonicalPath = applyImportMode(
        filePath, resolvedMode, /*subfolder*/ "", &importedKind);
    if (canonicalPath.empty()) {
        std::cerr << "ERROR: Import failed for " << filePath << std::endl;
        std::cerr << "========================================\n" << std::endl;
        return;
    }
    if (m_projectManager) {
        m_projectManager->addMediaFile(canonicalPath, importedKind);
    }
    if (m_probeWorker) m_probeWorker->enqueue(canonicalPath);
    if (transcodeEligible && resolvedTranscode) {
        scheduleTranscode(canonicalPath, mediaType);
    } else {
        ingestVideoClip(canonicalPath, mediaType);
    }
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
    clip.mediaOutFrame = clip.totalMediaFrames > 0     // Default out-point: last frame of source (inclusive)
        ? clip.totalMediaFrames - 1 : -1;
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
        for (entt::entity existingClipEntity : checkTrack.layers) {
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
    {
        auto& lay = m_registry.emplace<Layer>(clipEntity);
        lay.kind       = Layer::Kind::Clip;
        lay.startFrame = clip.startFrame;
        lay.duration   = clip.duration;
        lay.trackIndex = static_cast<uint32_t>(finalTrackIndex);
    }
    auto& finalTrack = m_registry.get<TimelineTrack>(m_timeline->getTracks()[finalTrackIndex]);
    finalTrack.addLayer(clipEntity);
    finalTrack.sortLayers(m_registry); // Maintain sorted order by start frame
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

entt::entity Engine::createObjectAnimationLayer(entt::entity targetScreen,
                                                int trackIndex,
                                                FrameNumber startFrame,
                                                FrameNumber duration) {
    if (!m_timeline) return entt::null;
    const auto& tracks = m_timeline->getTracks();
    if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracks.size()) {
        std::cerr << "[Engine::createObjectAnimationLayer] trackIndex "
                  << trackIndex << " out of range" << std::endl;
        return entt::null;
    }

    auto* track = m_registry.try_get<TimelineTrack>(tracks[trackIndex]);
    if (!track) return entt::null;

    entt::entity layerEntity = m_registry.create();

    auto& lay = m_registry.emplace<Layer>(layerEntity);
    lay.kind       = Layer::Kind::ObjectAnimation;
    lay.startFrame = startFrame;
    lay.duration   = duration;
    lay.trackIndex = static_cast<uint32_t>(trackIndex);
    lay.color      = {0.55f, 0.35f, 0.70f, 1.0f};

    auto& oal = m_registry.emplace<ObjectAnimationLayer>(layerEntity);
    oal.target          = targetScreen;
    oal.sectionBehavior = SectionBehavior::Normal;

    m_registry.emplace<AnimatedProperties>(layerEntity);

    track->addLayer(layerEntity);
    track->sortLayers(m_registry);

    std::cout << "[Engine] Created OA layer entity="
              << static_cast<uint32_t>(layerEntity)
              << " track=" << trackIndex
              << " start=" << startFrame
              << " duration=" << duration << std::endl;
    return layerEntity;
}

entt::entity Engine::createEmptyClipLayer(int trackIndex,
                                          FrameNumber startFrame,
                                          FrameNumber duration) {
    if (!m_timeline) return entt::null;
    const auto& tracks = m_timeline->getTracks();
    if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracks.size()) {
        std::cerr << "[Engine::createEmptyClipLayer] trackIndex "
                  << trackIndex << " out of range" << std::endl;
        return entt::null;
    }

    auto* track = m_registry.try_get<TimelineTrack>(tracks[trackIndex]);
    if (!track) return entt::null;

    entt::entity layerEntity = m_registry.create();

    auto& clip = m_registry.emplace<Clip>(layerEntity);
    clip.startFrame = startFrame;
    clip.duration   = duration;
    // All other Clip fields left at defaults (filepath empty, mediaType Unknown,
    // etc.). User assigns media via the Properties panel or drag-and-drop.

    auto& lay = m_registry.emplace<Layer>(layerEntity);
    lay.kind       = Layer::Kind::Clip;
    lay.startFrame = startFrame;
    lay.duration   = duration;
    lay.trackIndex = static_cast<uint32_t>(trackIndex);
    lay.color      = {0.35f, 0.55f, 0.85f, 1.0f};
    lay.name       = "Empty Clip";

    m_registry.emplace<Transform>(layerEntity);
    m_registry.emplace<MediaLayer>(layerEntity);

    track->addLayer(layerEntity);
    track->sortLayers(m_registry);

    std::cout << "[Engine] Created empty clip layer entity="
              << static_cast<uint32_t>(layerEntity)
              << " track=" << trackIndex
              << " start=" << startFrame
              << " duration=" << duration << std::endl;
    return layerEntity;
}

entt::entity Engine::createMuncherLayer(entt::entity targetScreen,
                                        int trackIndex,
                                        FrameNumber startFrame,
                                        FrameNumber duration) {
    if (!m_timeline) return entt::null;
    const auto& tracks = m_timeline->getTracks();
    if (trackIndex < 0 || static_cast<size_t>(trackIndex) >= tracks.size()) {
        std::cerr << "[Engine::createMuncherLayer] trackIndex "
                  << trackIndex << " out of range" << std::endl;
        return entt::null;
    }

    auto* track = m_registry.try_get<TimelineTrack>(tracks[trackIndex]);
    if (!track) return entt::null;

    entt::entity layerEntity = m_registry.create();

    auto& lay = m_registry.emplace<Layer>(layerEntity);
    lay.kind       = Layer::Kind::Generative;
    lay.startFrame = startFrame;
    lay.duration   = duration;
    lay.trackIndex = static_cast<uint32_t>(trackIndex);
    // Yellow-green: nods to the lower-case-"e" Muncher mascot and reads
    // distinctly against the Clip (blue) and OA (purple) timeline colors.
    lay.color      = {0.85f, 0.78f, 0.20f, 1.0f};
    lay.name       = "Muncher";

    auto& gl = m_registry.emplace<GenerativeLayer>(layerEntity);
    gl.targetScreen = targetScreen;
    // renderWidth / renderHeight default 1920x1080; show thread will allocate
    // the matching render target on first use.

    m_registry.emplace<MunchersGameState>(layerEntity);
    m_registry.emplace<MediaLayer>(layerEntity);

    // UV-space transform — same semantics as Clip. Position in compose-target
    // NDC, scale=1 fills the screen. Default identity = playfield fills the
    // target screen, matching pre-Transform Muncher behavior. CompositorSystem
    // pre-multiplies this onto every procedural cell-quad in drawMuncherPlayfield.
    m_registry.emplace<Transform>(layerEntity);

    track->addLayer(layerEntity);
    track->sortLayers(m_registry);

    std::cout << "[Engine] Created Muncher generative layer entity="
              << static_cast<uint32_t>(layerEntity)
              << " track=" << trackIndex
              << " start=" << startFrame
              << " duration=" << duration << std::endl;
    return layerEntity;
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
    if (m_probeWorker) m_probeWorker->enqueue(filePath);

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
    clip.mediaOutFrame = clip.totalMediaFrames > 0   // Default out-point: last frame of source (inclusive)
        ? clip.totalMediaFrames - 1 : -1;
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
        for (entt::entity existingClipEntity : checkTrack.layers) {
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
    {
        auto& lay = m_registry.emplace<Layer>(clipEntity);
        lay.kind       = Layer::Kind::Clip;
        lay.startFrame = clip.startFrame;
        lay.duration   = clip.duration;
        lay.trackIndex = static_cast<uint32_t>(finalTrackIndex);
    }
    auto& finalTrack = m_registry.get<TimelineTrack>(m_timeline->getTracks()[finalTrackIndex]);
    finalTrack.addLayer(clipEntity);
    finalTrack.sortLayers(m_registry); // Maintain sorted order by start frame
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

    CrashLogger::setProjectPath(m_projectManager->projectPath());

    // Drop undo/redo history — the previous project's commands point at
    // entities that the load just replaced. Without this, a Ctrl+Z
    // post-load would replay against a stale entity ID.
    if (m_commandDispatcher) {
        m_commandDispatcher->clearHistory();
    }

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
    //
    // Routed through the D2R bus so the show thread creates the output
    // window. Per ADR-0014 the show thread owns output windows, swap chains,
    // and Present; calling setOutputEnabled() directly here would create the
    // window on the editor thread and violate that contract.
    if (m_outputManager) {
        m_outputManager->syncCounterFromRegistry();

        auto view = m_registry.view<OutputDisplay>();
        for (auto [entity, out] : view.each()) {
            if (out.enabled && out.isPhysical() && out.physicalDisplayIndex >= 0) {
                publishSetOutputEnabled(bus::SetOutputEnabled{
                    static_cast<std::uint64_t>(entity), true});
            }
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

    // Async metadata probe: every library entry feeds the
    // MediaProbeWorker so MediaBin can render rows immediately and
    // fill in codec/resolution/framerate as the worker drains. Reset
    // counters first so the modal's progress bar starts at 0/N for
    // this load. Cached probes from a previous in-session load are
    // preserved (resetCounters doesn't drop m_results), so already-
    // probed paths return instantly via tryGet.
    if (m_probeWorker) {
        m_probeWorker->resetCounters();
        for (const auto& entry : m_projectManager->loadedMediaFiles()) {
            // v12 — seed from cache when the on-disk file matches the
            // last-saved size+mtime; otherwise enqueue a fresh probe.
            // Cache misses (modified files, fresh entries) fall through
            // and re-probe silently.
            m_probeWorker->enqueueOrSeed(entry.originalPath,
                                         entry.lastProbeSizeBytes,
                                         entry.lastProbeMtimeUnix,
                                         entry.cachedProbe);
        }
    }

    // Initial sync scan of <projectDir>/content/ — picks up files the
    // user dropped in while the app was closed. Bypasses the polling
    // scanner's 2-tick stability gate (those files have been on disk
    // for a while; they're not mid-write) so the bin is fully populated
    // by the time the editor renders. ContentScanner still starts below
    // and handles drops that happen later.
    if (!filepath.empty() && m_projectManager) {
        const std::filesystem::path projectRoot = filepath.parent_path();
        const std::filesystem::path contentDir = projectRoot / "content";
        std::error_code ec;
        if (std::filesystem::exists(contentDir, ec) &&
            std::filesystem::is_directory(contentDir, ec)) {
            std::filesystem::recursive_directory_iterator it(
                contentDir,
                std::filesystem::directory_options::skip_permission_denied,
                ec);
            for (; !ec && it != std::filesystem::recursive_directory_iterator{};
                 it.increment(ec)) {
                const auto& entry = *it;
                if (entry.is_directory(ec)) {
                    if (ContentScanner::shouldSkipDirStatic(entry.path())) {
                        it.disable_recursion_pending();
                    }
                    continue;
                }
                if (!entry.is_regular_file(ec)) continue;
                if (ContentScanner::shouldSkipFileStatic(entry.path())) continue;
                if (!ContentScanner::isMediaExtensionStatic(entry.path().extension())) continue;

                std::error_code relEc;
                std::filesystem::path rel =
                    std::filesystem::relative(entry.path(), projectRoot, relEc);
                if (relEc) continue;
                const std::string relStr = rel.generic_string();  // forward slashes

                if (m_projectManager->findEntry(relStr) != nullptr) continue;
                auto& libEntry = m_projectManager->addMediaFile(
                    relStr, ProjectManager::PathKind::Managed);
                libEntry.missingOnDisk = false;
                // Brand-new entry (no probe yet) — enqueueOrSeed falls
                // through to the enqueue branch since lastProbeSizeBytes
                // is 0. Using the same call site here keeps the gating
                // logic in one place.
                if (m_probeWorker) {
                    m_probeWorker->enqueueOrSeed(relStr,
                                                 libEntry.lastProbeSizeBytes,
                                                 libEntry.lastProbeMtimeUnix,
                                                 libEntry.cachedProbe);
                }
                std::cout << "[Engine] initial scan discovered " << relStr << std::endl;
            }
        }
    }

    // Show the project-load modal while probes drain. When the user
    // opened a tiny project (or one whose entries are all already
    // cached) pendingCount() will be 0 by the time render() polls
    // this and the modal will skip the open. Larger projects keep
    // the modal until the worker finishes.
    m_showProjectLoadModal = m_probeWorker && m_probeWorker->pendingCount() > 0;

    // #27 — kick off the content/ poller now that we know the project
    // root. Files that already exist in content/ but aren't in the
    // library (because the user copied them in via Explorer / sync
    // tools while the app was closed) will appear within a few ticks.
    if (m_contentScanner && !filepath.empty()) {
        m_contentScanner->start(filepath.parent_path());
    }
    return true;
}

void Engine::closeProject() {
    // Idempotent: each step below tolerates an already-clear state, so
    // calling closeProject() with nothing open just lands the user in
    // the launcher.

    // 1. Disable physical output windows. Same prelude as loadProject —
    //    OutputDisplay components hold renderer slot IDs; clearing the
    //    components without first releasing the slots would leak them
    //    for the rest of the session.
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

    // 2. Join any in-flight transcode workers. requestCancel() + join
    //    via destructors. Partial outputs on disk are harmless — the
    //    worker writes to a deterministic path and a future re-import
    //    will overwrite or skip via the existing dedupe.
    if (m_transcodeManager) {
        m_transcodeManager->joinAll();
    }

    // 3. Wipe the timeline (clips + tracks + sections + selection +
    //    play/stop state).
    if (m_timeline) {
        m_timeline->clear();
    }

    // 4. Destroy the project-scoped entity types ProjectSerializer::load
    //    would otherwise wipe on its way to loading new state.
    {
        std::vector<entt::entity> toDestroy;
        auto mappingView = m_registry.view<MappingSurface>();
        for (auto e : mappingView) toDestroy.push_back(e);
        auto outputView  = m_registry.view<OutputDisplay>();
        for (auto e : outputView)  toDestroy.push_back(e);
        auto modelView   = m_registry.view<Model>();
        for (auto [e, m] : modelView.each()) toDestroy.push_back(e);
        for (auto e : toDestroy) {
            if (m_registry.valid(e)) m_registry.destroy(e);
        }
    }
    m_registry.clear<ClipDecodeState>();

    // 5. Drop undo/redo history — the entities those commands reference
    //    are gone. Without this, a Ctrl+Z after re-opening a different
    //    project would replay an op against a stale entity ID.
    if (m_commandDispatcher) {
        m_commandDispatcher->clearHistory();
    }

    // 6. Stop the content scanner BEFORE clearing PM state — its worker
    //    thread reads m_contentDir which won't be valid post-close.
    if (m_contentScanner) {
        m_contentScanner->stop();
    }

    // 7. Clear PM state (m_projectPath, mediaLibrary, policy).
    if (m_projectManager) {
        m_projectManager->closeProject();
    }

    // 8. Re-show the launcher. The run loop already gates editor render
    //    + simulation work on m_showLauncher (see Engine::run /
    //    Engine::render), so this is the only flag flip required.
    showLauncher();

    std::cout << "[Engine] Project closed; launcher re-shown." << std::endl;
}

void Engine::drainContentScannerDeltas() {
    ZoneScopedN("ContentScanner::drainDeltas");
    if (!m_contentScanner || !m_projectManager) return;
    auto deltas = m_contentScanner->drain();
    if (deltas.empty()) return;

    for (const auto& d : deltas) {
        switch (d.kind) {
            case ContentScanner::DeltaKind::Added: {
                // Discovered files are Managed by definition (they live
                // under the project's content/ tree). addMediaFile is
                // idempotent so it's safe even if a Linked entry happens
                // to point at the same file — the dedupe is via
                // findEntry by string equality of `originalPath`.
                if (m_projectManager->findEntry(d.relativePath) != nullptr) {
                    // Already known (manual import got here first, or
                    // we're seeing a file already in the loaded library).
                    // Re-clear missingOnDisk in case the file came back.
                    if (auto* e = m_projectManager->findEntry(d.relativePath); e) {
                        e->missingOnDisk = false;
                    }
                    continue;
                }
                auto& entry = m_projectManager->addMediaFile(
                    d.relativePath, ProjectManager::PathKind::Managed);
                entry.missingOnDisk = false;
                if (m_probeWorker) m_probeWorker->enqueue(d.relativePath);
                std::cout << "[ContentScanner] discovered " << d.relativePath << std::endl;
                break;
            }
            case ContentScanner::DeltaKind::Removed: {
                // Soft-mark missing — never erase. Brief unlink-then-rename
                // windows during external sync would otherwise nuke user
                // state.
                if (auto* e = m_projectManager->findEntry(d.relativePath); e) {
                    e->missingOnDisk = true;
                    std::cout << "[ContentScanner] missing " << d.relativePath << std::endl;
                }
                break;
            }
        }
    }
}

void Engine::saveProjectInteractive() {
    if (!m_projectManager) return;
    const auto& currentPath = m_projectManager->projectPath();
    if (currentPath.empty()) {
        saveProjectAsInteractive();
        return;
    }
    saveProject(currentPath);
}

void Engine::saveProjectAsInteractive() {
    if (!m_windowManager || !m_projectManager) return;
    std::string suggested = m_projectManager->projectPath().empty()
                                ? std::string{}
                                : m_projectManager->projectPath().string();
    m_windowManager->saveProjectFileDialog(
        suggested,
        [this](const std::string& chosen) {
            if (chosen.empty()) return;  // Cancelled
            saveProject(chosen);
        });
}

void Engine::openProjectInteractive() {
    if (!m_windowManager) return;
    m_windowManager->openProjectFileDialog(
        [this](const std::string& chosen) {
            if (chosen.empty()) return;  // Cancelled
            loadProject(chosen);
        });
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
    // Default Main Screen renders as a 4m × 2.25m flat panel using the
    // default 16:9 plane mesh. Size in real-world meters per Phase 2 spec.
    screen.size = {4.0f, 2.25f, 0.0f};
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
    // the post-beginShowFrame drain below picks it up in the original order.
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
    ZoneScopedN("drainR2D");
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
            } else if constexpr (std::is_same_v<T, bus::ScreenRenderTargetAllocated>) {
                // Show thread allocated a compose-target slot for a Screen.
                // Write it back into the Screen component on the editor thread
                // so the next SceneSnapshot carries the valid slot.
                const auto entity = static_cast<entt::entity>(body.entity);
                if (auto* screen = m_registry.try_get<Screen>(entity)) {
                    screen->renderTargetSlot  = body.slot;
                    screen->renderTargetValid = (body.slot != UINT32_MAX);
                }
            } else if constexpr (std::is_same_v<T, bus::GenerativeLayerRenderTargetAllocated>) {
                // Mirror of ScreenRenderTargetAllocated for generative
                // layers. The PASS 1 compositor draws into this slot;
                // PASS 2 blits it onto the target screen via the unified
                // ContentLayerSnapshot path.
                const auto entity = static_cast<entt::entity>(body.entity);
                if (auto* gen = m_registry.try_get<GenerativeLayer>(entity)) {
                    gen->renderTargetSlot = static_cast<int>(body.slot);
                }
            } else if constexpr (std::is_same_v<T, bus::EffectChainRenderTargetAllocated>) {
                // PASS 1.5 of the compositor allocated one of the two
                // ping-pong slots for a layer's effect chain (issue #54).
                // Write the slot back into EffectChainRenderTargets so the
                // next SceneSnapshot carries it; show-side PASS 1.5 reuses
                // the slot until the layer/chain goes away.
                const auto entity = static_cast<entt::entity>(body.entity);
                if (!m_registry.valid(entity)) return;
                auto& rts = m_registry.get_or_emplace<EffectChainRenderTargets>(entity);
                if (body.side == 0) rts.slotA = static_cast<std::int32_t>(body.slot);
                else                rts.slotB = static_cast<std::int32_t>(body.slot);
                rts.width  = body.width;
                rts.height = body.height;
            } else if constexpr (std::is_same_v<T, bus::EffectCompileFailed>) {
                // Phase 6 — user-authored HLSL compile failure. Editor-side
                // surface (red badge on the affected node in the graph
                // editor) is wired up in Phase 6; Phase 2 just logs.
                std::cerr << "[Engine] EffectCompileFailed kind=0x"
                          << std::hex << body.kindIdHash << std::dec
                          << ": " << body.errorMessage << std::endl;
            } else if constexpr (std::is_same_v<T, bus::DeviceLost>) {
                // Autosave-on-device-loss. The Engine already observed
                // isDeviceLost() in the main loop and set m_running=false.
                // Write a .autosave beside the project file, then set the
                // relaunch flag so main.cpp can spawn a recovery process.
                // Single-relaunch latch: skip if this process is already a
                // recovery launch (m_isRecoveryRelaunch) to prevent storms.
                std::cerr << "[Engine] bus::DeviceLost received on R2D drain "
                          << "(HRESULT=0x" << std::hex << body.hresult << std::dec
                          << "): " << body.reason << std::endl;
                if (!m_isRecoveryRelaunch && m_projectManager &&
                    !m_projectManager->projectPath().empty() && m_timeline) {
                    std::filesystem::path projectPath = m_projectManager->projectPath();
                    // Name: <stem>.autosave.entity so ProjectSerializer's
                    // extension check passes without double-appending.
                    std::filesystem::path autosavePath =
                        projectPath.parent_path() /
                        (projectPath.stem().string() + ".autosave.entity");
                    if (ProjectSerializer::save(*m_timeline, autosavePath, m_projectManager)) {
                        std::cerr << "[Engine] Device-lost autosave: " << autosavePath.string() << std::endl;
                        m_relaunchProjectPath  = autosavePath;
                        m_relaunchRequested    = true;
                    } else {
                        std::cerr << "[Engine] Device-lost autosave failed: "
                                  << ProjectSerializer::getLastError() << std::endl;
                    }
                }
            }
            // FrameDropped, CreateOutputWindowRequest, OutputWindowReady:
            // informational or deferred — no action needed in this path.
        }, *msg);
    });
}

void Engine::onModelDestroyed(entt::registry& reg, entt::entity e) {
    const Model& m = reg.get<Model>(e);
    if (!m.gpuResourcesValid) return;
    if (auto* d3d = m_rendererService ? m_rendererService->getD3D12Renderer() : nullptr)
        d3d->scheduleMeshSlotFree(m.vertexBufferSlot);
}

} // namespace entity
