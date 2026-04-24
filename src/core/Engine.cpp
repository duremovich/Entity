#include "entity/core/Engine.hpp"
#include "entity/core/PlaybackController.hpp"
#include "entity/render/D3D12Renderer.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/ui/WindowManager.hpp"
#include "entity/ui/TimelineWindow.hpp"
#include "entity/ui/StageWindow.hpp"
#include "entity/ui/MediaBinWindow.hpp"
#include "entity/ui/PropertyWindow.hpp"
#include "entity/ui/MappingWindow.hpp"
#include "entity/ui/ModelBinWindow.hpp"
#include "entity/ui/ScreensWindow.hpp"
#include "entity/systems/TestSystem.hpp"
#include "entity/systems/TimelineSystem.hpp"
#include "entity/systems/BufferSystem.hpp"
#include "entity/systems/CompositorSystem.hpp"
#include "entity/systems/AnimationSystem.hpp"
#include "entity/systems/DecodeSystem.hpp"
#include "entity/media/Decoder.hpp"
#include "entity/media/FrameRingBuffer.hpp"
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

    // Initialize D3D12 renderer
    m_renderer = std::make_unique<D3D12Renderer>();
    Result rendererResult = m_renderer->initialize(m_window, windowWidth, windowHeight);
    if (rendererResult != Result::Success) {
        std::cerr << "Failed to initialize D3D12 renderer!" << std::endl;
        return rendererResult;
    }

    // Output manager — enumerates displays and owns physical-output windows.
    // Constructed before systems so any scripted output setup can work.
    m_outputManager = std::make_unique<OutputManager>(m_renderer.get(), m_registry);
    if (m_outputManager->initialize() != Result::Success) {
        std::cerr << "Failed to initialize OutputManager!" << std::endl;
        return Result::Failure;
    }

    // Register compositor system (renders layers to screen)
    registerSystem(std::make_unique<CompositorSystem>(m_renderer.get()));

    // Register test system to validate infrastructure
    registerSystem(std::make_unique<TestSystem>());

    // Initialize all registered systems
    for (auto& system : m_systems) {
        system->initialize(m_registry);
    }

    // Initialize timeline
    m_timeline = std::make_unique<Timeline>(m_registry);

    // Initialize playback controller (owns frame timing + per-frame seek-aware updates).
    // DecodeSystem is injected below after registration.
    m_playbackController = std::make_unique<PlaybackController>(m_registry, m_timeline.get(), m_renderer.get());

    // Initialize project manager (owns project path, media library, autosave)
    m_projectManager = std::make_unique<ProjectManager>();
    m_projectManager->initialize(m_timeline.get(), &m_registry, m_renderer.get());

    // Background HAP transcoder — cache dir gets set lazily via
    // updateTranscodeCacheDir() when a project path is established.
    m_transcodeManager = std::make_unique<TranscodeManager>();
    updateTranscodeCacheDir();

    // Set up callback for when new clips are created (split, duplicate)
    m_timeline->setClipCreatedCallback([this](entt::entity clipEntity, const std::string& filepath) {
        this->onClipCreated(clipEntity, filepath);
    });

    // Set timeline on CompositorSystem (system 0) for frame-accurate rendering
    if (!m_systems.empty()) {
        if (auto* compositor = dynamic_cast<CompositorSystem*>(m_systems[0].get())) {
            compositor->setTimeline(m_timeline.get());
            compositor->setDebugLogging(false);  // Debug logging disabled
        }
    }

    // Register decode system (needs timeline for frame position)
    auto decodeSystem = std::make_unique<DecodeSystem>();
    decodeSystem->setTimeline(m_timeline.get());
    m_decodeSystem = decodeSystem.get();  // Keep raw pointer for direct access
    m_playbackController->setDecodeSystem(m_decodeSystem);
    registerSystem(std::move(decodeSystem));

    // Register animation system (needs timeline for keyframe evaluation)
    auto animSystem = std::make_unique<AnimationSystem>();
    animSystem->setTimeline(m_timeline.get());
    m_animationSystem = animSystem.get();  // Keep raw pointer for direct access
    registerSystem(std::move(animSystem));

    // Initialize command dispatcher
    m_commandDispatcher = std::make_unique<CommandDispatcher>();
    std::cout << "  Command dispatcher initialized" << std::endl;

    // Initialize window manager
    m_windowManager = std::make_unique<WindowManager>();
    m_windowManager->initialize();

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

    // Register windows with window manager
    m_windowManager->registerWindow(std::make_unique<MediaBinWindow>(this));

    // Create and configure TimelineWindow
    auto timelineWindow = std::make_unique<TimelineWindow>(m_timeline.get());
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

    m_windowManager->registerWindow(std::make_unique<StageWindow>(this));
    {
        auto propertyWindow = std::make_unique<PropertyWindow>(m_timeline.get());
        propertyWindow->setCommandDispatcher(m_commandDispatcher.get());
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

    // Initialize timing
    m_playbackController->startTiming();

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

    // Shutdown systems in reverse order
    for (auto it = m_systems.rbegin(); it != m_systems.rend(); ++it) {
        (*it)->shutdown(m_registry);
    }
    m_systems.clear();

    // Shutdown subsystems
    // TODO: Shutdown systems when they exist
    // m_transport.reset();
    // m_timeline.reset();
    // OutputManager must release its output windows BEFORE the renderer
    // shuts down — it holds slot IDs into the renderer's swap-chain pool.
    if (m_outputManager) {
        m_outputManager->shutdown();
        m_outputManager.reset();
    }
    m_renderer.reset();

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

void Engine::run() {
    if (!m_initialized) {
        std::cerr << "Cannot run: Engine not initialized!" << std::endl;
        return;
    }

    std::cout << "Starting main loop..." << std::endl;
    m_running = true;

    while (m_running && !glfwWindowShouldClose(m_window)) {
        // Update timing
        m_playbackController->updateTiming();
        double deltaTime = m_playbackController->getDeltaTime();

        // Detect potential freeze (frame took > 100ms)
        if (deltaTime > 0.1) {
            std::cout << "[FREEZE WARNING] Frame " << m_playbackController->getFrameCount() << " took "
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

        // Process command queue
        if (m_commandDispatcher) {
            m_commandDispatcher->processQueue(*this);
        }

        // Update systems
        update();

        // Render
        render();

        // Bail on GPU device-lost. Keep rendering into a dead device and you
        // get silent hangs, not useful for a live show. Shut down cleanly so
        // the operator knows they need to restart.
        if (m_renderer && m_renderer->isDeviceLost()) {
            std::cerr << "[Engine] D3D12 device lost — exiting main loop." << std::endl;
            m_running = false;
        }

        autoSaveTick(deltaTime);
        pollTranscodes();

        m_playbackController->incrementFrameCount();
    }

    std::cout << "Main loop exited." << std::endl;
}

void Engine::requestExit() {
    m_running = false;
}

IRenderer* Engine::getRenderer() {
    // Upcast from the concrete unique_ptr; external callers see only the interface.
    return m_renderer.get();
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

    // Snapshot the library so we don't hold a mutable reference across
    // setTranscodedPath calls (which could in principle grow the vector).
    std::vector<std::string> originals;
    originals.reserve(m_projectManager->loadedMediaFiles().size());
    for (const auto& e : m_projectManager->loadedMediaFiles()) {
        if (e.transcodedPath.empty()) originals.push_back(e.originalPath);
    }

    for (const auto& src : originals) {
        auto st = m_transcodeManager->statusOf(src);
        if (!st) continue;
        if (st->state == TranscodeState::Done) {
            m_projectManager->setTranscodedPath(src, st->outputPath, st->variant);
            std::cout << "[Engine] Transcode done: " << src
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

bool Engine::autoTranscodeOnImport() const {
    return m_projectManager && m_projectManager->autoTranscodeOnImport();
}
void Engine::setAutoTranscodeOnImport(bool enable) {
    if (m_projectManager) m_projectManager->setAutoTranscodeOnImport(enable);
}

const std::filesystem::path& Engine::getProjectPath() const {
    static const std::filesystem::path kEmpty;
    return m_projectManager ? m_projectManager->projectPath() : kEmpty;
}

void Engine::registerSystem(std::unique_ptr<System> system) {
    std::cout << "Registering system: " << system->getName() << std::endl;
    m_systems.push_back(std::move(system));
}

void Engine::processEvents() {
    glfwPollEvents();

    // TODO: Handle input events
}

void Engine::update() {
    auto t0 = std::chrono::high_resolution_clock::now();
    double deltaTime = m_playbackController ? m_playbackController->getDeltaTime() : 0.0;

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

    // Update all registered systems except CompositorSystem (index 0)
    // CompositorSystem is called in render() between beginFrame/endFrame
    for (size_t i = 1; i < m_systems.size(); ++i) {
        m_systems[i]->update(m_registry, static_cast<float>(deltaTime));
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
    return m_playbackController ? m_playbackController->getDeltaTime() : 0.0;
}

double Engine::getElapsedTime() const {
    return m_playbackController ? m_playbackController->getElapsedTime() : 0.0;
}

uint64_t Engine::getFrameCount() const {
    return m_playbackController ? m_playbackController->getFrameCount() : 0;
}

const DecodedFrame* Engine::getCurrentVideoFrame() const {
    if (m_playbackController) {
        if (const DecodedFrame* frame = m_playbackController->getCurrentVideoFrame()) {
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
        auto t0 = std::chrono::high_resolution_clock::now();

        m_renderer->beginFrame();
        auto t1 = std::chrono::high_resolution_clock::now();

        // Upload video textures to GPU - MUST be after beginFrame() or commands get discarded!
        if (m_playbackController) {
            m_playbackController->updateClipVideos();
        }
        auto t2 = std::chrono::high_resolution_clock::now();

        // Clear to a nice teal/cyan color (to warm your heart!)
        m_renderer->clear(0.0f, 0.5f, 0.6f, 1.0f);

        // Render all layers via CompositorSystem
        // CompositorSystem is first in m_systems, so we call it explicitly here
        if (!m_systems.empty()) {
            double deltaTime = m_playbackController ? m_playbackController->getDeltaTime() : 0.0;
            m_systems[0]->update(m_registry, static_cast<float>(deltaTime));
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
                    m_outputManager->setOutputEnabled(entity, false);
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
                // Toggle play/pause
                if (m_timeline->getPlaybackState() == PlaybackState::Playing) {
                    m_timeline->pause();
                } else {
                    m_timeline->play();
                }
                break;

            case GLFW_KEY_K:
                // K = Pause (industry standard)
                m_timeline->pause();
                break;

            case GLFW_KEY_J:
                // J = Step backward (simplified - full J/K/L would need speed control)
                if (m_timeline->getPlaybackState() == PlaybackState::Playing) {
                    m_timeline->pause();
                }
                {
                    Timecode currentTime = m_timeline->getCurrentTime();
                    Timecode frameTime = static_cast<Timecode>(1000000.0 / m_timeline->getFrameRate());
                    if (currentTime > frameTime) {
                        m_timeline->seek(currentTime - frameTime);
                    } else {
                        m_timeline->seek(0);
                    }
                }
                if (m_timelineWidget) m_timelineWidget->ensurePlayheadVisible();
                break;

            case GLFW_KEY_L:
                // L = Step forward (simplified - full J/K/L would need speed control)
                if (m_timeline->getPlaybackState() == PlaybackState::Playing) {
                    m_timeline->pause();
                }
                {
                    Timecode currentTime = m_timeline->getCurrentTime();
                    Timecode frameTime = static_cast<Timecode>(1000000.0 / m_timeline->getFrameRate());
                    Timecode newTime = currentTime + frameTime;
                    if (newTime < m_timeline->getDuration()) {
                        m_timeline->seek(newTime);
                    }
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

    // Test 5: FrameBuffer Component
    std::cout << "\nTest 5: FrameBuffer Component" << std::endl;
    {
        // Create BufferSystem for testing buffer logic
        BufferSystem bufferSystem;

        auto entity = m_registry.create();
        testEntities.push_back(entity);
        auto& frameBuffer = m_registry.emplace<FrameBuffer>(entity);

        // Note: Full ring buffer functionality requires FrameRingBuffer class
        // For now, just test basic state
        frameBuffer.currentPTS = 1000000; // 1 second in microseconds
        frameBuffer.targetFrame = 30;
        frameBuffer.bufferedFrames = 16;

        // Use BufferSystem instead of component methods
        assert(bufferSystem.hasFrames(frameBuffer) == true);
        assert(bufferSystem.isReady(frameBuffer) == true); // >= 8 frames

        float fillPercentage = bufferSystem.getFillPercentage(frameBuffer);
        assert(fillPercentage == 16.0f / 32.0f); // 50% full

        std::cout << "  ✓ FrameBuffer state tracking works (via BufferSystem)" << std::endl;
        std::cout << "  ✓ Buffer fill: " << (fillPercentage * 100.0f) << "%" << std::endl;
    }

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

void Engine::onVideoFileSelected(const std::string& filePath) {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Video File Selected" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "File: " << filePath << std::endl;

    // Detect media type from file extension
    MediaType mediaType = detectMediaType(filePath);
    if (mediaType == MediaType::Unknown) {
        std::cerr << "ERROR: Unsupported media type: " << filePath << std::endl;
        std::cerr << "========================================\n" << std::endl;
        return;
    }

    std::cout << "Detected media type: " << MediaTypeToString(mediaType) << std::endl;

    // If auto-transcode is on and the source is a non-HAP container the
    // HAP encoder can actually handle (i.e. ProRes / H264 / HEVC — not PNG
    // sequences, since vcpkg FFmpeg lacks a PNG decoder), register in the
    // media library and queue a background transcode. No clip gets created
    // until the user drags it out of the MediaBin once Done.
    if (m_projectManager && m_transcodeManager &&
        m_projectManager->autoTranscodeOnImport() &&
        !isHapMediaType(mediaType) &&
        mediaType == MediaType::VideoProRes4444) {
        m_projectManager->addMediaFile(filePath);
        m_transcodeManager->enqueue(filePath, "hap_alpha", 0.0);
        std::cout << "[Engine] Queued transcode for " << filePath << std::endl;
        std::cout << "========================================\n" << std::endl;
        return;
    }

    // Create decoder for the media type
    m_decoder = createDecoder(mediaType);
    if (!m_decoder) {
        std::cerr << "ERROR: Failed to create decoder for media type: "
                  << MediaTypeToString(mediaType) << std::endl;
        std::cerr << "========================================\n" << std::endl;
        return;
    }

    std::cout << "Created decoder: " << MediaTypeToString(mediaType) << std::endl;

    // Open the media file
    Result result = m_decoder->open(filePath);
    if (result != Result::Success) {
        std::cerr << "ERROR: Failed to open media file: " << filePath << std::endl;
        m_decoder.reset();
        std::cerr << "========================================\n" << std::endl;
        return;
    }

    std::cout << "Media opened successfully:" << std::endl;
    std::cout << "  Resolution: " << m_decoder->getWidth() << "x" << m_decoder->getHeight() << std::endl;
    std::cout << "  Duration: " << m_decoder->getDuration() << " frames" << std::endl;
    std::cout << "  Frame rate: " << m_decoder->getFrameRate() << " fps" << std::endl;
    std::cout << "  Has alpha: " << (m_decoder->hasAlpha() ? "yes" : "no") << std::endl;

    // Add to loaded media files list
    if (m_projectManager) m_projectManager->addMediaFile(filePath);

    // Create clip entity with all required components for rendering
    entt::entity clipEntity = m_registry.create();

    // Add Clip component with metadata
    auto& clip = m_registry.emplace<Clip>(clipEntity);
    clip.filepath = filePath;
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

    // Add VideoTexture component and allocate texture slot
    auto& videoTex = m_registry.emplace<VideoTexture>(clipEntity);
    videoTex.descriptorSlot = m_renderer->allocateVideoTextureSlot();
    if (videoTex.descriptorSlot == UINT32_MAX) {
        std::cerr << "ERROR: Failed to allocate video texture slot!" << std::endl;
    } else {
        std::cout << "Allocated video texture slot: " << videoTex.descriptorSlot << std::endl;
    }

    // Add FrameBuffer component for threaded decoding
    auto& frameBuffer = m_registry.emplace<FrameBuffer>(clipEntity);
    frameBuffer.ringBuffer = std::make_shared<FrameRingBuffer>();
    frameBuffer.isBuffering.store(true);
    std::cout << "Created FrameBuffer with ring buffer for threaded decoding" << std::endl;

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

    // Add to loaded media files list (idempotent)
    if (m_projectManager) m_projectManager->addMediaFile(filePath);

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

    // Add VideoTexture component and allocate texture slot
    auto& videoTex = m_registry.emplace<VideoTexture>(clipEntity);
    videoTex.descriptorSlot = m_renderer->allocateVideoTextureSlot();
    if (videoTex.descriptorSlot == UINT32_MAX) {
        std::cerr << "ERROR: Failed to allocate video texture slot!" << std::endl;
    }

    // Add FrameBuffer component for threaded decoding
    auto& frameBuffer = m_registry.emplace<FrameBuffer>(clipEntity);
    frameBuffer.ringBuffer = std::make_shared<FrameRingBuffer>();
    frameBuffer.isBuffering.store(true);

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

    // Re-queue library entries whose transcoded side is missing — either
    // because auto-transcode ran mid-session before save, or the cache
    // was cleared. Only entries whose source is a non-HAP media type and
    // the project still wants auto-transcode get re-enqueued.
    if (m_transcodeManager) {
        for (const auto& entry : m_projectManager->loadedMediaFiles()) {
            if (!entry.transcodedPath.empty()) continue;
            if (!m_projectManager->autoTranscodeOnImport()) break;
            const MediaType mt = detectMediaType(entry.originalPath);
            if (isHapMediaType(mt)) continue;
            if (mt == MediaType::Unknown) continue;
            const double srcFps = (mt == MediaType::PNGSequence) ? 30.0 : 0.0;
            m_transcodeManager->enqueue(entry.originalPath, "hap_alpha", srcFps);
            std::cout << "[Engine] Re-enqueued transcode for " << entry.originalPath << std::endl;
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

    // Allocate video texture slot if VideoTexture component exists
    auto* videoTex = m_registry.try_get<VideoTexture>(clipEntity);
    if (videoTex && videoTex->descriptorSlot == 0) {
        videoTex->descriptorSlot = m_renderer->allocateVideoTextureSlot();
        if (videoTex->descriptorSlot == UINT32_MAX) {
            std::cerr << "[Engine] Failed to allocate video texture slot!" << std::endl;
        } else {
            std::cout << "[Engine] Allocated video texture slot: " << videoTex->descriptorSlot << std::endl;
        }
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

bool Engine::captureHash(const std::string& hashFilepath,
                          const std::string& goldenFilepath,
                          uint32_t composeSlot) {
    if (!m_renderer) {
        std::cerr << "[Engine] captureHash failed: renderer not initialized" << std::endl;
        return false;
    }

    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;
    if (!m_renderer->readComposeTargetPixels(composeSlot, width, height, pixels)) {
        return false;
    }

    // FNV-1a 64-bit — deterministic, dependency-free, good enough for exact-match
    // pixel comparisons in tests. Not cryptographic.
    constexpr uint64_t FNV_OFFSET = 0xcbf29ce484222325ULL;
    constexpr uint64_t FNV_PRIME  = 0x00000100000001b3ULL;
    uint64_t hash = FNV_OFFSET;
    for (uint8_t b : pixels) {
        hash ^= static_cast<uint64_t>(b);
        hash *= FNV_PRIME;
    }

    // Format: "<16-hex-digits> <width>x<height>\n"
    char buf[64];
    int n = std::snprintf(buf, sizeof(buf), "%016llx %ux%u\n",
                          static_cast<unsigned long long>(hash), width, height);
    if (n <= 0) {
        return false;
    }
    std::string line(buf, static_cast<size_t>(n));

    // Write hash file
    std::filesystem::path outPath(hashFilepath);
    if (outPath.has_parent_path()) {
        std::filesystem::create_directories(outPath.parent_path());
    }
    std::ofstream out(outPath, std::ios::binary);
    if (!out) {
        std::cerr << "[Engine] captureHash: failed to open for write: " << hashFilepath << std::endl;
        return false;
    }
    out.write(line.data(), static_cast<std::streamsize>(line.size()));
    out.close();

    std::cout << "[CaptureHash] " << hashFilepath << " -> " << line;

    // Compare to golden if provided
    if (!goldenFilepath.empty()) {
        std::ifstream gf(goldenFilepath, std::ios::binary);
        if (!gf) {
            std::cerr << "[CaptureHash] MISS: golden not found: " << goldenFilepath << std::endl;
            return false;
        }
        std::string golden((std::istreambuf_iterator<char>(gf)),
                            std::istreambuf_iterator<char>());
        if (golden != line) {
            std::cerr << "[CaptureHash] FAIL: mismatch\n  got:    " << line
                      << "  golden: " << golden << std::endl;
            return false;
        }
        std::cout << "[CaptureHash] PASS against " << goldenFilepath << std::endl;
    }

    return true;
}

bool Engine::captureScreenshot(const std::string& filepath) {
    if (!m_renderer) {
        std::cerr << "[Engine] Screenshot failed: renderer not initialized" << std::endl;
        return false;
    }

    return m_renderer->captureComposeTargetToPNG(filepath);
}

bool Engine::captureWindowScreenshot(const std::string& filepath) {
    if (!m_renderer) {
        std::cerr << "[Engine] Screenshot failed: renderer not initialized" << std::endl;
        return false;
    }

    return m_renderer->captureBackBufferToPNG(filepath);
}

bool Engine::runScript(const std::string& filepath) {
    if (!m_commandDispatcher) {
        std::cerr << "[Engine] Script failed: command dispatcher not initialized" << std::endl;
        return false;
    }

    return m_commandDispatcher->loadScript(filepath);
}

} // namespace entity
