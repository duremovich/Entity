#include "entity/core/Engine.hpp"
#include "entity/render/D3D12Renderer.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/timeline/TimelineWidget.hpp"
#include "entity/systems/TestSystem.hpp"
#include "entity/systems/TimelineSystem.hpp"
#include "entity/systems/BufferSystem.hpp"
#include "entity/systems/CompositorSystem.hpp"
#include <imgui.h>
#include "entity/components/Transform.hpp"
#include "entity/components/MediaLayer.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/VideoTexture.hpp"
#include "entity/components/FrameBuffer.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/components/OutputMapping.hpp"
#include <GLFW/glfw3.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

#include <iostream>
#include <cassert>

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

Result Engine::initialize(uint32_t windowWidth, uint32_t windowHeight, const char* windowTitle) {
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

    // Initialize timeline widget
    m_timelineWidget = std::make_unique<TimelineWidget>(m_timeline.get());

    // Create test tracks and clips for timeline
    entt::entity track1 = m_timeline->createTrack("Video Track 1");
    entt::entity track2 = m_timeline->createTrack("Video Track 2");

    // Add test clips to tracks
    {
        auto& trackComponent = m_registry.get<TimelineTrack>(track1);

        // Clip 1: 0-5 seconds
        entt::entity clip1 = m_registry.create();
        auto& clip1Data = m_registry.emplace<Clip>(clip1);
        clip1Data.filepath = "test_video_1.mov";
        clip1Data.startFrame = 0;
        clip1Data.duration = 150;  // 5 seconds at 30fps
        clip1Data.framerate = 30.0;
        trackComponent.addClip(clip1);

        // Clip 2: 6-10 seconds
        entt::entity clip2 = m_registry.create();
        auto& clip2Data = m_registry.emplace<Clip>(clip2);
        clip2Data.filepath = "test_video_2.mov";
        clip2Data.startFrame = 180;  // Start at 6 seconds
        clip2Data.duration = 120;    // 4 seconds at 30fps
        clip2Data.framerate = 30.0;
        trackComponent.addClip(clip2);
    }

    {
        auto& trackComponent = m_registry.get<TimelineTrack>(track2);

        // Clip 3: 2-8 seconds on second track
        entt::entity clip3 = m_registry.create();
        auto& clip3Data = m_registry.emplace<Clip>(clip3);
        clip3Data.filepath = "overlay_video.mov";
        clip3Data.startFrame = 60;   // Start at 2 seconds
        clip3Data.duration = 180;    // 6 seconds at 30fps
        clip3Data.framerate = 30.0;
        trackComponent.addClip(clip3);
    }

    std::cout << "Created test timeline with 2 tracks and 3 clips" << std::endl;

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
    createTestEntities();

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

        // Update systems
        update();

        // Render
        render();

        m_frameCount++;
    }

    std::cout << "Main loop exited." << std::endl;
}

void Engine::requestExit() {
    m_running = false;
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
    // Update timeline
    if (m_timeline) {
        m_timeline->update(m_deltaTime);
    }

    // Update all registered systems except CompositorSystem (index 0)
    // CompositorSystem is called in render() between beginFrame/endFrame
    for (size_t i = 1; i < m_systems.size(); ++i) {
        m_systems[i]->update(m_registry, static_cast<float>(m_deltaTime));
    }
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
        m_renderer->beginFrame();

        // Clear to a nice teal/cyan color (to warm your heart!)
        m_renderer->clear(0.0f, 0.5f, 0.6f, 1.0f);

        // Render all layers via CompositorSystem
        // CompositorSystem is first in m_systems, so we call it explicitly here
        if (!m_systems.empty()) {
            m_systems[0]->update(m_registry, static_cast<float>(m_deltaTime));
        }

        // Begin ImGui frame for UI rendering
        m_renderer->beginImGuiFrame();

        // Render timeline widget
        if (m_timelineWidget) {
            m_timelineWidget->render();
        }

        // End ImGui frame and render UI
        m_renderer->endImGuiFrame();

        m_renderer->endFrame();
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
    (void)mods;

    // Handle ESC key to quit
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
        std::cout << "ESC pressed, requesting exit..." << std::endl;
        requestExit();
    }

    // TODO: Add more input handling as needed
    // For now, log other keys for debugging
    if (action == GLFW_PRESS) {
        std::cout << "Key pressed: " << key << std::endl;
    }
}

void Engine::testComponents() {
    std::cout << "\n========================================" << std::endl;
    std::cout << "Phase 3: Testing ECS Components" << std::endl;
    std::cout << "========================================\n" << std::endl;

    // Test 1: Transform Component
    std::cout << "Test 1: Transform Component" << std::endl;
    {
        auto entity = m_registry.create();
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
        auto& track = m_registry.emplace<TimelineTrack>(trackEntity);

        track.trackIndex = 0;

        // Create some clip entities and add them to track
        auto clip1 = m_registry.create();
        auto clip2 = m_registry.create();
        auto clip3 = m_registry.create();

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

} // namespace entity
