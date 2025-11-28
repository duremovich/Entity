#pragma once

#include "Types.hpp"
#include "entity/systems/System.hpp"
#include <entt/entt.hpp>
#include <memory>
#include <chrono>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <string>

// Forward declarations
struct GLFWwindow;

namespace entity {

// Forward declarations
class D3D12Renderer;
class Timeline;
class WindowManager;
class Decoder;
class DecodeSystem;
class AnimationSystem;
class CommandDispatcher;
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

    /**
     * Get the CommandDispatcher.
     */
    CommandDispatcher* getCommandDispatcher() { return m_commandDispatcher.get(); }

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

    /**
     * Save the current project to a file.
     * @param filepath Path to save to (empty = use current project path or prompt)
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
     * Get the current project file path.
     */
    const std::filesystem::path& getProjectPath() const { return m_projectPath; }

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

    /**
     * Handle media dropped onto timeline track.
     * Creates a clip at the specified track and position.
     */
    void onMediaDroppedOnTimeline(const std::string& filePath, int trackIndex, Timecode position);

    /**
     * Update all clip video textures based on current timeline position.
     * Decodes frames for active clips and uploads to GPU.
     */
    void updateClipVideos();

    /**
     * Check if a clip is active at the given frame.
     */
    bool isClipActiveAtFrame(const struct Clip& clip, FrameNumber frame) const;

    /**
     * Map timeline frame to media frame for a clip.
     */
    FrameNumber mapToMediaFrame(const struct Clip& clip, FrameNumber timelineFrame) const;

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

    // Systems
    std::vector<std::unique_ptr<System>> m_systems;
    DecodeSystem* m_decodeSystem{nullptr};  // Raw pointer for direct access (owned by m_systems)
    AnimationSystem* m_animationSystem{nullptr};  // Raw pointer for direct access (owned by m_systems)

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

    // Video playback (legacy single-clip for backwards compatibility)
    std::unique_ptr<Decoder> m_decoder;
    std::unique_ptr<DecodedFrame> m_currentFrame;

    // Multi-clip video playback state
    struct ClipDecodeState {
        std::unique_ptr<Decoder> decoder;
        std::unique_ptr<DecodedFrame> frame;
        FrameNumber lastDecodedFrame{UINT32_MAX};
    };
    std::unordered_map<entt::entity, ClipDecodeState> m_clipState;

    // Media library
    std::vector<std::string> m_loadedMediaFiles;

    // Project file
    std::filesystem::path m_projectPath;
};

} // namespace entity
