#include "entity/uitest/UiTestHarness.hpp"

#if !defined(ENTITY_ENABLE_UI_TESTS)

// Link-safe stubs: this TU is always compiled so that Engine's unique_ptr
// destructor and UiActionCommand::execute resolve on OFF builds. Engine
// never constructs the harness there; commands see isInitialized()==false
// and fail with a clear error.
namespace entity {
UiTestHarness::~UiTestHarness() = default;
bool UiTestHarness::initialize(Engine&) { return false; }
void UiTestHarness::stop() {}
void UiTestHarness::submit(UiAction) {}
bool UiTestHarness::isSessionIdle() const { return true; }
void UiTestHarness::runSession(void*) {}
} // namespace entity

#else

#include "entity/core/Engine.hpp"
#include "entity/command/CommandDispatcher.hpp"

#include "imgui.h"
#include "imgui_test_engine/imgui_te_engine.h"
#include "imgui_test_engine/imgui_te_context.h"

#include <iostream>

namespace entity {

namespace {
UiTestHarness* g_harness = nullptr;  // single instance; the test's TestFunc is
                                     // a plain function pointer with no
                                     // user-data slot at v1.89.7
} // namespace

UiTestHarness::~UiTestHarness() {
    // Must run after ImGui::DestroyContext (test-engine ini data unbinds
    // lazily on context shutdown; see imgui_te_engine.h ordering notes).
    if (m_engine) {
        ImGuiTestEngine_DestroyContext(m_engine);
        m_engine = nullptr;
    }
    g_harness = nullptr;
}

bool UiTestHarness::initialize(Engine& engine) {
    if (m_engine) return true;
    m_app = &engine;
    g_harness = this;

    m_engine = ImGuiTestEngine_CreateContext();
    ImGuiTestEngineIO& io = ImGuiTestEngine_GetIO(m_engine);
    io.ConfigRunSpeed = ImGuiTestRunSpeed_Fast;   // teleport mouse, no delays
    io.ConfigVerboseLevel = ImGuiTestVerboseLevel_Info;
    io.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Debug;
    // No ScreenCaptureFunc is wired (screenshots go through CaptureBroker);
    // capture-on-error would also open the only yield window between
    // TestFunc returning and the run task clearing, during which a
    // submit()'s QueueTest would be silently dropped as "already running".
    io.ConfigCaptureOnError = false;

    ImGuiTestEngine_Start(m_engine, ImGui::GetCurrentContext());

    m_sessionTest = IM_REGISTER_TEST(m_engine, "harness", "session");
    m_sessionTest->TestFunc = [](ImGuiTestContext* ctx) {
        if (g_harness) g_harness->runSession(ctx);
    };

    std::cout << "[UiTestHarness] ImGui Test Engine started (run speed: fast)"
              << std::endl;
    return true;
}

void UiTestHarness::stop() {
    if (!m_engine) return;
    m_stopping.store(true);
    ImGuiTestEngine_Stop(m_engine);
    // Fail anything still pending so wait predicates resolve.
    std::lock_guard<std::mutex> lock(m_queueMutex);
    for (auto& a : m_queue) {
        *a.error = "UI test harness stopped";
        a.succeeded->store(false);
        a.done->store(true);
    }
    m_queue.clear();
    m_sessionQueued = false;
}

void UiTestHarness::submit(UiAction action) {
    bool needQueue = false;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.push_back(std::move(action));
        if (!m_sessionQueued) {
            m_sessionQueued = true;
            needQueue = true;
        }
    }
    if (needQueue) {
        ImGuiTestEngine_QueueTest(m_engine, m_sessionTest, ImGuiTestRunFlags_None);
    }
}

bool UiTestHarness::isSessionIdle() const {
    return !m_engine || ImGuiTestEngine_IsTestQueueEmpty(m_engine);
}

void UiTestHarness::runSession(void* testContext) {
    auto* ctx = static_cast<ImGuiTestContext*>(testContext);
    while (!m_stopping.load()) {
        UiAction action;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_queue.empty()) {
                // Drained everything: end this run. The next submit()
                // re-queues the session test.
                m_sessionQueued = false;
                return;
            }
            action = std::move(m_queue.front());
            m_queue.pop_front();
        }

        bool ok = true;
        std::string err;
        switch (action.kind) {
        case UiAction::Kind::SetRef:
            ctx->SetRef(action.ref.c_str());
            break;
        case UiAction::Kind::Click:
            ctx->ItemClick(action.ref.c_str());
            break;
        case UiAction::Kind::Check:
            if (action.checked) ctx->ItemCheck(action.ref.c_str());
            else                ctx->ItemUncheck(action.ref.c_str());
            break;
        case UiAction::Kind::InputValue:
            switch (action.valueType) {
            case UiAction::ValueType::Int:
                ctx->ItemInputValue(action.ref.c_str(),
                                    static_cast<int>(action.numericValue));
                break;
            case UiAction::ValueType::Float:
                ctx->ItemInputValue(action.ref.c_str(),
                                    static_cast<float>(action.numericValue));
                break;
            case UiAction::ValueType::Text:
                ctx->ItemInputValue(action.ref.c_str(), action.textValue.c_str());
                break;
            default:
                ok = false; err = "InputValue with no value type";
                break;
            }
            break;
        case UiAction::Kind::MenuClick:
            ctx->MenuClick(action.ref.c_str());
            break;
        case UiAction::Kind::FocusWindow:
            ctx->WindowFocus(action.ref.c_str());
            break;
        case UiAction::Kind::AssertItemExists: {
            ImGuiTestItemInfo* info =
                ctx->ItemInfo(action.ref.c_str(), ImGuiTestOpFlags_NoError);
            if (!info || info->ID == 0) {
                ok = false;
                err = "item not found: " + action.ref;
            }
            break;
        }
        }

        // Test-engine ops report failure by flagging the test as errored.
        // Surface that as this action's failure, then clear the flag so the
        // session can keep serving later commands. If the op instead
        // requested a hard abort, bail out -- stop()/the drained-queue path
        // fail whatever is still pending.
        if (ctx->IsError()) {
            ok = false;
            if (err.empty()) err = "UI op failed (see test engine log): " + action.ref;
            if (ctx->Abort) {
                *action.error = err;
                action.succeeded->store(false);
                action.done->store(true);
                if (auto* d = m_app->getCommandDispatcher()) {
                    d->addErrorToResults("UI action failed: " + err);
                }
                std::lock_guard<std::mutex> lock(m_queueMutex);
                for (auto& a : m_queue) {
                    *a.error = "UI session aborted by earlier failure";
                    a.succeeded->store(false);
                    a.done->store(true);
                }
                m_queue.clear();
                m_sessionQueued = false;
                return;
            }
            ctx->Test->Status = ImGuiTestStatus_Running;
            ctx->RecoverFromUiContextErrors();
        }

        // Failed actions must fail the *script*, not just end the wait:
        // the wait predicate completing doesn't record an error, so route
        // it into the script results here. The coroutine runs handshake-
        // interleaved with the editor thread (it only executes between
        // NewFrame yields while the editor blocks in RunOnce), so touching
        // the dispatcher is serialized with the editor's own use.
        if (!ok) {
            if (auto* d = m_app->getCommandDispatcher()) {
                d->addErrorToResults("UI action failed: " + err);
            }
        }

        *action.error = err;
        action.succeeded->store(ok);
        action.done->store(true);
    }
}

} // namespace entity

#endif // ENTITY_ENABLE_UI_TESTS
