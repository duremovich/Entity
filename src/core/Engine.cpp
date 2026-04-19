#include "entity/core/Engine.hpp"
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
#include "entity/command/CommandDispatcher.hpp"
#include "entity/command/Commands.hpp"
#include <imgui.h>
#include "entity/components/Transform.hpp"
#include "entity/components/MediaLayer.hpp"
#include "entity/components/Clip.hpp"
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

    // Set up video file callback
    m_windowManager->setVideoFileCallback([this](const std::string& filePath) {
        this->onVideoFileSelected(filePath);
    });

    // Set up project save/load callbacks
    m_windowManager->setSaveProjectCallback([this]() {
        this->saveProject();
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
    if (auto* widget = timelineWindow->getWidget()) {
        // Set up callback for media dropped onto timeline tracks
        widget->setMediaDropCallback([this](const std::string& filepath, int trackIndex, Timecode position) {
            this->onMediaDroppedOnTimeline(filepath, trackIndex, position);
        });
    }
    m_windowManager->registerWindow(std::move(timelineWindow));

    m_windowManager->registerWindow(std::make_unique<StageWindow>(this));
    m_windowManager->registerWindow(std::make_unique<PropertyWindow>(m_timeline.get()));
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
    m_startTime = Clock::now();
    m_lastFrameTime = m_startTime;
    m_deltaTime = 0.0;
    m_elapsedTime = 0.0;
    m_frameCount = 0;

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

    // Shutdown systems in reverse order
    for (auto it = m_systems.rbegin(); it != m_systems.rend(); ++it) {
        (*it)->shutdown(m_registry);
    }
    m_systems.clear();

    // Shutdown subsystems
    // TODO: Shutdown systems when they exist
    // m_transport.reset();
    // m_timeline.reset();
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
        updateTiming();

        // Detect potential freeze (frame took > 100ms)
        if (m_deltaTime > 0.1) {
            std::cout << "[FREEZE WARNING] Frame " << m_frameCount << " took "
                      << (m_deltaTime * 1000.0) << "ms (timeline frame: "
                      << (m_timeline ? m_timeline->getCurrentFrame() : 0) << ")" << std::endl;
        }

        // Update FPS counter
        m_fpsAccumulator += m_deltaTime;
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

        autoSaveTick(m_deltaTime);

        m_frameCount++;
    }

    std::cout << "Main loop exited." << std::endl;
}

void Engine::requestExit() {
    m_running = false;
}

void Engine::autoSaveTick(double deltaTime) {
    if (!m_timeline) return;
    m_autosaveAccumulator += deltaTime;
    if (m_autosaveAccumulator < m_autosaveIntervalSec) return;
    m_autosaveAccumulator = 0.0;

    std::filesystem::path autosavePath =
        m_projectPath.empty() ? std::filesystem::path("autosave.entity")
                              : m_projectPath;
    autosavePath += ".autosave";

    // Direct serializer call — we don't want to update m_projectPath to point at
    // the .autosave file. Operator still expects "Save" to write the real path.
    if (ProjectSerializer::save(*m_timeline, autosavePath)) {
        std::cout << "[Autosave] " << autosavePath.string() << std::endl;
    } else {
        std::cerr << "[Autosave] Failed: " << ProjectSerializer::getLastError() << std::endl;
    }
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

    // Update timeline
    if (m_timeline) {
        m_timeline->update(m_deltaTime);
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
        m_systems[i]->update(m_registry, static_cast<float>(m_deltaTime));
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

const DecodedFrame* Engine::getCurrentVideoFrame() const {
    // First check multi-clip frames (new system)
    if (m_timeline) {
        FrameNumber currentFrame = m_timeline->getCurrentFrame();

        // Find first active clip with a valid frame
        auto view = m_registry.view<Clip, VideoTexture>();
        for (auto [entity, clip, videoTex] : view.each()) {
            if (isClipActiveAtFrame(clip, currentFrame)) {
                auto stateIt = m_clipState.find(entity);
                if (stateIt != m_clipState.end() && stateIt->second.frame && stateIt->second.frame->valid) {
                    // Verify frame number is close to expected (avoid stale frame flash)
                    FrameNumber expectedMediaFrame = mapToMediaFrame(clip, currentFrame);
                    FrameNumber cachedFrameNum = stateIt->second.frame->frameNumber;

                    // Allow some tolerance for decode-ahead, but reject obviously stale frames
                    int64_t frameDelta = static_cast<int64_t>(cachedFrameNum) - static_cast<int64_t>(expectedMediaFrame);
                    if (std::abs(frameDelta) > 16) {
                        // Stale frame from a different position - don't display it
                        continue;
                    }

                    return stateIt->second.frame.get();
                }
            }
        }
    }

    // Fall back to legacy single frame
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
        updateClipVideos();
        auto t2 = std::chrono::high_resolution_clock::now();

        // Clear to a nice teal/cyan color (to warm your heart!)
        m_renderer->clear(0.0f, 0.5f, 0.6f, 1.0f);

        // Render all layers via CompositorSystem
        // CompositorSystem is first in m_systems, so we call it explicitly here
        if (!m_systems.empty()) {
            m_systems[0]->update(m_registry, static_cast<float>(m_deltaTime));
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

void Engine::updateTiming() {
    TimePoint currentTime = Clock::now();

    // Calculate delta time
    std::chrono::duration<double> delta = currentTime - m_lastFrameTime;
    m_deltaTime = delta.count();

    // Calculate elapsed time
    std::chrono::duration<double> elapsed = currentTime - m_startTime;
    m_elapsedTime = elapsed.count();

    m_lastFrameTime = currentTime;
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

    // Only handle key press events (not release or repeat)
    if (action != GLFW_PRESS) return;

    // Don't handle shortcuts if user is actively typing in a text field
    // WantTextInput is more specific than WantCaptureKeyboard - it's only true
    // when a text input widget is active and accepting input
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) {
        return;
    }

    // Check for Ctrl modifier
    bool ctrlPressed = (mods & GLFW_MOD_CONTROL) != 0;

    // Handle ESC key to quit
    if (key == GLFW_KEY_ESCAPE) {
        std::cout << "ESC pressed, requesting exit..." << std::endl;
        requestExit();
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
                break;

            case GLFW_KEY_LEFT:
                // Left arrow = Step back one frame
                {
                    Timecode currentTime = m_timeline->getCurrentTime();
                    Timecode frameTime = static_cast<Timecode>(1000000.0 / m_timeline->getFrameRate());
                    if (currentTime > frameTime) {
                        m_timeline->seek(currentTime - frameTime);
                    } else {
                        m_timeline->seek(0);
                    }
                }
                break;

            case GLFW_KEY_RIGHT:
                // Right arrow = Step forward one frame
                {
                    Timecode currentTime = m_timeline->getCurrentTime();
                    Timecode frameTime = static_cast<Timecode>(1000000.0 / m_timeline->getFrameRate());
                    Timecode newTime = currentTime + frameTime;
                    if (newTime < m_timeline->getDuration()) {
                        m_timeline->seek(newTime);
                    }
                }
                break;

            case GLFW_KEY_HOME:
                // Home = Go to start
                m_timeline->seek(0);
                break;

            case GLFW_KEY_END:
                // End = Go to end
                m_timeline->seek(m_timeline->getDuration());
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
            // Ctrl+S = Save project
            if (ctrlPressed) {
                if (m_projectPath.empty()) {
                    // Default to project.entity in current directory
                    m_projectPath = "project.entity";
                }
                saveProject(m_projectPath);
            }
            break;

        case GLFW_KEY_O:
            // Ctrl+O = Open project (use default path for now)
            if (ctrlPressed) {
                // TODO: Integrate with file dialog
                // For now, try to load project.entity if it exists
                std::filesystem::path defaultPath = "project.entity";
                if (std::filesystem::exists(defaultPath)) {
                    loadProject(defaultPath);
                } else {
                    std::cout << "[Engine] No project file found at " << defaultPath.string() << std::endl;
                }
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
    m_loadedMediaFiles.push_back(filePath);

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
    auto& state = m_clipState[clipEntity];
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

    // Detect media type from file extension
    MediaType mediaType = detectMediaType(filePath);
    if (mediaType == MediaType::Unknown) {
        std::cerr << "ERROR: Unsupported media type: " << filePath << std::endl;
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

    // Open the media file
    Result result = decoder->open(filePath);
    if (result != Result::Success) {
        std::cerr << "ERROR: Failed to open media file: " << filePath << std::endl;
        std::cerr << "========================================\n" << std::endl;
        return;
    }

    std::cout << "Media info:" << std::endl;
    std::cout << "  Resolution: " << decoder->getWidth() << "x" << decoder->getHeight() << std::endl;
    std::cout << "  Duration: " << decoder->getDuration() << " frames" << std::endl;
    std::cout << "  Frame rate: " << decoder->getFrameRate() << " fps" << std::endl;

    // Add to loaded media files list if not already there
    auto it = std::find(m_loadedMediaFiles.begin(), m_loadedMediaFiles.end(), filePath);
    if (it == m_loadedMediaFiles.end()) {
        m_loadedMediaFiles.push_back(filePath);
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
    auto& state = m_clipState[clipEntity];
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

bool Engine::isClipActiveAtFrame(const Clip& clip, FrameNumber frame) const {
    // Check if the timeline frame falls within the clip's range
    return frame >= clip.startFrame && frame < (clip.startFrame + clip.duration);
}

FrameNumber Engine::mapToMediaFrame(const Clip& clip, FrameNumber timelineFrame) const {
    // Calculate frame position relative to clip start (in timeline frames)
    FrameNumber localFrame = timelineFrame - clip.startFrame;

    // Convert timeline frames to source media frames using frame rate ratio
    // E.g., for 24fps video on 30fps timeline: sourceFrame = localFrame * (24/30) = localFrame * 0.8
    double timelineFrameRate = m_timeline ? m_timeline->getFrameRate() : 30.0;
    double frameRateRatio = clip.framerate / timelineFrameRate;
    FrameNumber sourceLocalFrame = static_cast<FrameNumber>(std::floor(localFrame * frameRateRatio));

    // Get source media length
    FrameNumber sourceLength = clip.totalMediaFrames > 0 ? clip.totalMediaFrames :
        static_cast<FrameNumber>(clip.duration * frameRateRatio);

    // If within source media range, direct mapping
    if (sourceLocalFrame < sourceLength) {
        return clip.mediaStartFrame + sourceLocalFrame;
    }

    // Clip extends beyond source media - apply playback mode
    switch (clip.playbackMode) {
        case PlaybackMode::Freeze:
            // Hold on last frame
            return clip.mediaStartFrame + sourceLength - 1;

        case PlaybackMode::Loop:
            // Restart from beginning
            return clip.mediaStartFrame + (sourceLocalFrame % sourceLength);

        case PlaybackMode::PingPong: {
            // Play forward then backward (palindrome)
            FrameNumber cycle = sourceLocalFrame / sourceLength;
            FrameNumber pos = sourceLocalFrame % sourceLength;
            // Even cycles play forward, odd cycles play backward
            if (cycle % 2 == 0) {
                return clip.mediaStartFrame + pos;
            } else {
                return clip.mediaStartFrame + (sourceLength - 1 - pos);
            }
        }
    }

    // Fallback (should never reach)
    return clip.mediaStartFrame + sourceLength - 1;
}

void Engine::updateClipVideos() {
    if (!m_renderer || !m_timeline) {
        return;
    }

    auto funcStart = std::chrono::high_resolution_clock::now();
    int clipCount = 0;
    int activeCount = 0;
    int processedCount = 0;
    int ringBufferCount = 0;
    int syncDecodeCount = 0;

    FrameNumber currentFrame = m_timeline->getCurrentFrame();
    PlaybackState playState = m_timeline->getPlaybackState();

    // Iterate over all entities with Clip and VideoTexture components
    auto view = m_registry.view<Clip, VideoTexture>();

    for (auto [entity, clip, videoTex] : view.each()) {
        clipCount++;
        // Skip if no texture slot allocated
        if (!videoTex.isAllocated()) {
            continue;
        }

        // Check if clip is active at current frame
        if (!isClipActiveAtFrame(clip, currentFrame)) {
            continue;
        }
        activeCount++;

        // Map timeline frame to media frame
        FrameNumber mediaFrame = mapToMediaFrame(clip, currentFrame);

        // Only process if frame number changed
        auto stateIt = m_clipState.find(entity);
        FrameNumber lastFrame = (stateIt != m_clipState.end()) ? stateIt->second.lastDecodedFrame : UINT32_MAX;

        if (mediaFrame == lastFrame) {
            continue;  // Frame hasn't changed, skip
        }

        // Try to get frame from FrameRingBuffer first (threaded decode path)
        auto* frameBuffer = m_registry.try_get<FrameBuffer>(entity);
        if (!frameBuffer) {
            // No FrameBuffer - this clip wasn't set up for threaded decode
            // During playback, skip this clip entirely to avoid blocking
            if (playState == PlaybackState::Playing) {
                std::cout << "[SKIP] No FrameBuffer for entity " << static_cast<uint32_t>(entity)
                          << " during playback - skipping" << std::endl;
                continue;
            }
        } else if (!frameBuffer->ringBuffer) {
            // FrameBuffer exists but no ring buffer
            if (playState == PlaybackState::Playing) {
                std::cout << "[SKIP] No ringBuffer for entity " << static_cast<uint32_t>(entity)
                          << " during playback - skipping" << std::endl;
                continue;
            }
        }
        if (frameBuffer && frameBuffer->ringBuffer) {
            // Check if seek is pending - buffer contains stale frames until decode thread processes seek
            const DecodeWorker* worker = m_decodeSystem->getWorker(entity);
            if (worker && worker->seekPending.load()) {
                // Skip frame retrieval - buffer has stale data
                // Keep showing previous frame until fresh frames arrive
                continue;
            }

            DecodedFrame ringFrame;
            bool gotFrame = false;

            // During playback, consume frames to keep buffer flowing
            // During scrubbing/paused, just peek without consuming
            // EXCEPTION: For ping-pong mode, NEVER consume frames - we need them for both directions
            auto ringStart = std::chrono::high_resolution_clock::now();

            // For ping-pong, don't consume frames at all - we need them in both directions
            bool isPingPong = (clip.playbackMode == PlaybackMode::PingPong);

            if (playState == PlaybackState::Playing && !isPingPong) {
                // Normal forward playback (non-ping-pong) - consume frames to keep buffer flowing
                gotFrame = frameBuffer->ringBuffer->consumeUpTo(mediaFrame, ringFrame);
            } else {
                // Scrubbing, paused, OR ping-pong mode - don't consume, just get
                // This preserves frames in buffer for ping-pong bidirectional access
                gotFrame = frameBuffer->ringBuffer->getFrame(mediaFrame, ringFrame);
            }

            auto ringMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - ringStart).count();
            if (ringMs > 50) {
                std::cout << "[SLOW RING] getFrame/consumeUpTo took " << ringMs << "ms for entity "
                          << static_cast<uint32_t>(entity) << " frame " << mediaFrame << std::endl;
            }
            ringBufferCount++;

            if (gotFrame) {
                // Got frame from ring buffer - upload to GPU
                D3D12_GPU_DESCRIPTOR_HANDLE srvHandle{};
                bool uploadSuccess = m_renderer->uploadVideoFrameToSlot(
                    videoTex.descriptorSlot,
                    ringFrame.data.data(),
                    ringFrame.width,
                    ringFrame.height,
                    &srvHandle
                );

                if (uploadSuccess) {
                    videoTex.srvHandle = srvHandle;
                    videoTex.width = ringFrame.width;
                    videoTex.height = ringFrame.height;
                    if (stateIt != m_clipState.end()) {
                        stateIt->second.lastDecodedFrame = mediaFrame;

                        // Also update frame so getCurrentVideoFrame() returns this frame
                        if (stateIt->second.frame) {
                            *stateIt->second.frame = ringFrame;
                        }
                    }
                }
                continue;  // Frame processed from ring buffer, skip legacy path
            }

            // Frame not in ring buffer - try to get nearest available frame
            // This prevents visual "freeze" when decode thread is catching up
            if (playState == PlaybackState::Playing) {
                // During playback, try to get any frame that's in the buffer
                // This is better than showing nothing (freeze)
                DecodedFrame nearestFrame;
                if (frameBuffer->ringBuffer->getNearestFrame(mediaFrame, nearestFrame)) {
                    // Got a frame (might not be exact target, but close enough)
                    D3D12_GPU_DESCRIPTOR_HANDLE srvHandle{};
                    bool uploadSuccess = m_renderer->uploadVideoFrameToSlot(
                        videoTex.descriptorSlot,
                        nearestFrame.data.data(),
                        nearestFrame.width,
                        nearestFrame.height,
                        &srvHandle
                    );
                    if (uploadSuccess) {
                        videoTex.srvHandle = srvHandle;
                        videoTex.width = nearestFrame.width;
                        videoTex.height = nearestFrame.height;
                    }
                }
                // Either way, don't fall through to sync decode during playback
                continue;
            }
            // During paused/scrubbing - fall through to legacy decode (blocking is OK)
        }

        // Legacy path: synchronous decode on main thread (only used when paused/scrubbing)
        // CRITICAL SAFETY CHECK: Never do sync decode during playback - it causes freezes!
        // If we somehow got here during playback, skip this frame.
        // Check CURRENT playState from timeline (not cached) to catch any state changes
        if (m_timeline->getPlaybackState() == PlaybackState::Playing) {
            // Should not happen - but if it does, skip to avoid freeze
            continue;
        }

        if (stateIt == m_clipState.end()) {
            continue;
        }

        Decoder* decoder = stateIt->second.decoder.get();
        DecodedFrame* frame = stateIt->second.frame.get();

        if (!decoder || !decoder->isOpen() || !frame) {
            continue;
        }

        syncDecodeCount++;
        // Log that we're doing synchronous decode
        auto decodeStart = std::chrono::high_resolution_clock::now();
        std::cout << "[SYNC DECODE] Entity " << static_cast<uint32_t>(entity)
                  << " frame " << mediaFrame << " playState=" << static_cast<int>(playState) << std::endl;

        // Decode the frame synchronously (blocking - only for scrub/pause)
        Result result = decoder->decodeFrame(mediaFrame, *frame);

        auto decodeEnd = std::chrono::high_resolution_clock::now();
        auto decodeMs = std::chrono::duration_cast<std::chrono::milliseconds>(decodeEnd - decodeStart).count();

        if (decodeMs > 50) {
            std::cout << "[SYNC DECODE] Took " << decodeMs << "ms for entity " << static_cast<uint32_t>(entity) << std::endl;
        }

        if (result == Result::Success) {
            frame->valid = true;
            stateIt->second.lastDecodedFrame = mediaFrame;

            // Upload frame to GPU texture slot
            D3D12_GPU_DESCRIPTOR_HANDLE srvHandle{};
            bool uploadSuccess = m_renderer->uploadVideoFrameToSlot(
                videoTex.descriptorSlot,
                frame->data.data(),
                frame->width,
                frame->height,
                &srvHandle
            );

            if (uploadSuccess) {
                videoTex.srvHandle = srvHandle;
                videoTex.width = frame->width;
                videoTex.height = frame->height;
            }
        } else {
            frame->valid = false;
        }
    }

    // Log summary if update took significant time
    auto funcMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - funcStart).count();
    if (funcMs > 100) {
        std::cout << "[CLIP UPDATE] Total=" << funcMs << "ms"
                  << " clips=" << clipCount
                  << " active=" << activeCount
                  << " ringBuf=" << ringBufferCount
                  << " syncDec=" << syncDecodeCount
                  << " frame=" << currentFrame
                  << " state=" << static_cast<int>(playState) << std::endl;
    }
}

bool Engine::saveProject(const std::filesystem::path& filepath) {
    std::filesystem::path savePath = filepath;

    // Use current project path if empty
    if (savePath.empty()) {
        if (m_projectPath.empty()) {
            savePath = "project.entity";
        } else {
            savePath = m_projectPath;
        }
    }

    std::cout << "[Engine] Saving project to " << savePath.string() << "..." << std::endl;

    if (!m_timeline) {
        std::cerr << "[Engine] Cannot save: No timeline!" << std::endl;
        return false;
    }

    bool success = ProjectSerializer::save(*m_timeline, savePath);

    if (success) {
        m_projectPath = savePath;
        std::cout << "[Engine] Project saved successfully!" << std::endl;
    } else {
        std::cerr << "[Engine] Save failed: " << ProjectSerializer::getLastError() << std::endl;
    }

    return success;
}

bool Engine::loadProject(const std::filesystem::path& filepath) {
    std::cout << "[Engine] Loading project from " << filepath.string() << "..." << std::endl;

    if (!m_timeline) {
        std::cerr << "[Engine] Cannot load: No timeline!" << std::endl;
        return false;
    }

    // Clear existing clip resources before loading
    for (auto& [entity, state] : m_clipState) {
        state.decoder.reset();
    }
    m_clipState.clear();
    m_loadedMediaFiles.clear();

    // Define media load callback to recreate decoders for loaded clips
    ProjectSerializer::MediaLoadCallback loadCallback = [this](entt::entity clipEntity, const std::string& mediaPath) {
        // Check if file exists
        if (!std::filesystem::exists(mediaPath)) {
            std::cerr << "[Engine] Media file not found: " << mediaPath << std::endl;
            return;
        }

        // Detect media type and create decoder
        MediaType mediaType = detectMediaType(mediaPath);
        if (mediaType == MediaType::Unknown) {
            std::cerr << "[Engine] Unsupported media type: " << mediaPath << std::endl;
            return;
        }

        auto decoder = createDecoder(mediaType);
        if (!decoder) {
            std::cerr << "[Engine] Failed to create decoder for: " << mediaPath << std::endl;
            return;
        }

        Result result = decoder->open(mediaPath);
        if (result != Result::Success) {
            std::cerr << "[Engine] Failed to open media: " << mediaPath << std::endl;
            return;
        }

        // Add VideoTexture if not present
        if (!m_registry.all_of<VideoTexture>(clipEntity)) {
            auto& videoTex = m_registry.emplace<VideoTexture>(clipEntity);
            videoTex.descriptorSlot = m_renderer->allocateVideoTextureSlot();
            videoTex.width = decoder->getWidth();
            videoTex.height = decoder->getHeight();
        }

        // Add FrameBuffer if not present
        if (!m_registry.all_of<FrameBuffer>(clipEntity)) {
            auto& frameBuffer = m_registry.emplace<FrameBuffer>(clipEntity);
            frameBuffer.ringBuffer = std::make_shared<FrameRingBuffer>();
            frameBuffer.isBuffering.store(true);
        }

        // Store decoder and create frame buffer
        auto& state = m_clipState[clipEntity];
        state.decoder = std::move(decoder);
        state.frame = std::make_unique<DecodedFrame>();

        auto& clip = m_registry.get<Clip>(clipEntity);
        state.frame->allocate(clip.width, clip.height);
        state.lastDecodedFrame = UINT32_MAX;

        // Add to loaded media files
        if (std::find(m_loadedMediaFiles.begin(), m_loadedMediaFiles.end(), mediaPath) == m_loadedMediaFiles.end()) {
            m_loadedMediaFiles.push_back(mediaPath);
        }

        std::cout << "[Engine] Loaded media: " << mediaPath << std::endl;
    };

    bool success = ProjectSerializer::load(*m_timeline, filepath, loadCallback);

    if (success) {
        m_projectPath = filepath;
        std::cout << "[Engine] Project loaded successfully!" << std::endl;
    } else {
        std::cerr << "[Engine] Load failed: " << ProjectSerializer::getLastError() << std::endl;
    }

    return success;
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
    auto& state = m_clipState[clipEntity];
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
