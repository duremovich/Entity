#pragma once

#include "Types.hpp"
#include "entity/systems/System.hpp"
#include <entt/entt.hpp>
#include <memory>
#include <chrono>
#include <vector>

// Forward declarations
struct GLFWwindow;

namespace entity {

// Forward declarations
class D3D12Renderer;
class Timeline;
class WindowManager;
class Decoder;
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
     * @return Result::Success on success, error code otherwise
     */
    Result initialize(uint32_t windowWidth, uint32_t windowHeight, const char* windowTitle);

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
    D3D12Renderer* getRenderer() { return m_renderer.get(); }

    /**
     * Get the Timeline.
     */
    Timeline* getTimeline() { return m_timeline.get(); }

    // TODO: Implement this when class is ready
    // Transport* getTransport() { return m_transport.get(); }

    /**
     * Get the GLFW window.
     */
    GLFWwindow* getWindow() { return m_window; }

    /**
     * Get delta time for current frame (in seconds).
     */
    double getDeltaTime() const { return m_deltaTime; }

    /**
     * Get total elapsed time since engine start (in seconds).
     */
    double getElapsedTime() const { return m_elapsedTime; }

    /**
     * Get current frame number.
     */
    uint64_t getFrameCount() const { return m_frameCount; }

    /**
     * Get the current decoded video frame for display.
     * Returns nullptr if no frame is available.
     */
    const DecodedFrame* getCurrentVideoFrame() const;

    /**
     * Get list of loaded media files.
     */
    const std::vector<std::string>& getLoadedMediaFiles() const { return m_loadedMediaFiles; }

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
     * Update timing information.
     */
    void updateTiming();

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

private:
    // ECS registry
    entt::registry m_registry;

    // Core subsystems
    std::unique_ptr<D3D12Renderer> m_renderer;
    std::unique_ptr<Timeline> m_timeline;
    std::unique_ptr<WindowManager> m_windowManager;

    // Systems
    std::vector<std::unique_ptr<System>> m_systems;

    // TODO: implement when class is ready
    // std::unique_ptr<Transport> m_transport;

    // Window
    GLFWwindow* m_window{nullptr};
    uint32_t m_windowWidth{1920};
    uint32_t m_windowHeight{1080};

    // Timing
    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = std::chrono::time_point<Clock>;

    TimePoint m_startTime;
    TimePoint m_lastFrameTime;
    double m_deltaTime{0.0};
    double m_elapsedTime{0.0};
    uint64_t m_frameCount{0};

    // FPS tracking
    double m_fpsUpdateTimer{0.0};
    double m_fpsAccumulator{0.0};
    uint32_t m_fpsFrameCount{0};
    uint32_t m_currentFPS{0};

    // State
    bool m_initialized{false};
    bool m_running{false};
    bool m_resizePending{false};
    uint32_t m_pendingWidth{0};
    uint32_t m_pendingHeight{0};

    // Video playback
    std::unique_ptr<Decoder> m_decoder;
    std::unique_ptr<DecodedFrame> m_currentFrame;

    // Media library
    std::vector<std::string> m_loadedMediaFiles;
};

} // namespace entity
