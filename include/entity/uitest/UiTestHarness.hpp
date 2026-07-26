#pragma once

// UI test harness on Dear ImGui Test Engine. On builds without
// ENTITY_ENABLE_UI_TESTS the .cpp compiles link-safe no-op stubs and
// Engine never constructs the harness (pointer stays null).
//
// Model: one registered "session" test whose body drains a queue of
// UiActions. Ui* script commands enqueue an action, (re-)queue the session
// test if idle, and block the script drain via
// CommandDispatcher::setWaitPredicate until the action's done flag flips.
// The TestFunc coroutine runs interleaved with ImGui::NewFrame on the
// editor thread; the queue mutex only guards the action list.

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

struct ImGuiTestEngine;
struct ImGuiTest;

namespace entity {

class Engine;

struct UiAction {
    enum class Kind {
        SetRef,        // ref = window/path prefix for subsequent actions
        Click,         // ref = item path
        Check,         // ref = item path, checked = target state
        InputValue,    // ref = item path; one of numeric/text used per valueType
        MenuClick,     // ref = "//Window/Menu/Item" path
        FocusWindow,   // ref = window name
        AssertItemExists, // ref = item path
    };
    enum class ValueType { None, Int, Float, Text };

    Kind        kind{Kind::Click};
    std::string ref;
    bool        checked{true};
    ValueType   valueType{ValueType::None};
    double      numericValue{0.0};
    std::string textValue;

    // Completion signalling (shared with the issuing command's predicate).
    std::shared_ptr<std::atomic<bool>> done{std::make_shared<std::atomic<bool>>(false)};
    std::shared_ptr<std::atomic<bool>> succeeded{std::make_shared<std::atomic<bool>>(false)};
    std::shared_ptr<std::string>       error{std::make_shared<std::string>()};
};

class UiTestHarness {
public:
    UiTestHarness() = default;
    ~UiTestHarness();

    UiTestHarness(const UiTestHarness&) = delete;
    UiTestHarness& operator=(const UiTestHarness&) = delete;

    // Call on the editor thread AFTER ImGui context + backends exist
    // (Engine::initialize, right after the Renderer service comes up).
    bool initialize(Engine& engine);

    // Call on the editor thread BEFORE D3D12Renderer::shutdownImGui.
    // Stops the engine (aborts any running coroutine, failing pending
    // actions); ImGuiTestEngine_DestroyContext is deferred to the
    // destructor, which must run after ImGui::DestroyContext.
    void stop();

    // Enqueue an action and ensure the session test is queued to run.
    // Editor thread only (called from command execute()).
    void submit(UiAction action);

    // True when no session run is queued or winding down. Ui command wait
    // predicates gate on this IN ADDITION to the action's done flag: after
    // TestFunc returns, the engine runs two wind-down Yields before
    // clearing its run queue, and a QueueTest issued in that window is
    // silently dropped as "already running". Editor thread only.
    bool isSessionIdle() const;

    bool isInitialized() const { return m_engine != nullptr; }

private:
    void runSession(void* testContext);  // ImGuiTestContext*, kept void* to
                                         // spare non-UI translation units the
                                         // test-engine headers
    ImGuiTestEngine* m_engine{nullptr};
    ImGuiTest*       m_sessionTest{nullptr};  // owned by m_engine
    Engine*          m_app{nullptr};

    std::mutex m_queueMutex;
    std::deque<UiAction> m_queue;
    bool m_sessionQueued{false};   // guarded by m_queueMutex
    std::atomic<bool> m_stopping{false};

    // Last UiSetRef, re-applied at the start of every session run: each
    // run gets a fresh stack-local ImGuiTestContext inside the engine, and
    // the dispatcher drain means one Ui command per run — without this a
    // SetRef would be forgotten before the next command. Coroutine-side
    // only (editor thread is parked while the coroutine runs).
    std::string m_baseRef;
};

} // namespace entity
