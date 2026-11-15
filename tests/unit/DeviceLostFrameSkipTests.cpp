/**
 * begin*Frame must report a lost device so the caller skips the tick.
 *
 * When beginShowFrame / beginEditorFrame cannot record (device lost,
 * latched before the call or by the show-fence wait inside it), no command
 * list is active on the calling thread. Callers used to ignore that and run
 * the compositor / output / stage draws anyway, dereferencing the null
 * thread-local list (an access violation that pre-empted the device-lost
 * autosave + relaunch). The begin calls now return false and the caller skips.
 *
 * Like WaitForGpuEarlyOutTests, these run against a default-constructed
 * D3D12Renderer: no device, so they cover the begin-frame contract and the
 * test hook, not the null-list guards in the draw entry points (those need
 * live compose targets / output windows; integration_device_lost_show_frame
 * covers them end to end on WARP).
 */

#include <gtest/gtest.h>

#include "entity/render/D3D12Renderer.hpp"

using namespace entity;

namespace {

TEST(DeviceLostFrameSkip, BeginShowFrameReportsConfirmedLoss) {
    D3D12Renderer renderer;
    renderer.setDeviceLostForTesting(DXGI_ERROR_DEVICE_HUNG);
    EXPECT_FALSE(renderer.beginShowFrame());
    // endShowFrame stays callable after a failed begin (bookkeeping only).
    renderer.endShowFrame();
}

TEST(DeviceLostFrameSkip, BeginShowFrameReportsUnconfirmedLoss) {
    // A fence wait that exhausts its budget on a live device latches with
    // reason S_OK; the frame still cannot be recorded.
    D3D12Renderer renderer;
    renderer.setDeviceLostForTesting(S_OK);
    EXPECT_FALSE(renderer.beginShowFrame());
    EXPECT_EQ(renderer.getDeviceLostReason(), static_cast<int32_t>(S_OK))
        << "skipping the frame must not upgrade the latch to a confirmed removal";
    renderer.endShowFrame();
}

TEST(DeviceLostFrameSkip, BeginEditorFrameReportsLoss) {
    D3D12Renderer renderer;
    renderer.setDeviceLostForTesting(DXGI_ERROR_DEVICE_REMOVED);
    EXPECT_FALSE(renderer.beginEditorFrame());
    renderer.endEditorFrame();
}

TEST(DeviceLostFrameSkip, FailNextShowFrameHookLatchesUnconfirmedLoss) {
    D3D12Renderer renderer;
    ASSERT_FALSE(renderer.isDeviceLost());
    renderer.failNextShowFrameForTesting();
    EXPECT_FALSE(renderer.beginShowFrame());
    EXPECT_TRUE(renderer.isDeviceLost());
    EXPECT_EQ(renderer.getDeviceLostReason(), static_cast<int32_t>(S_OK));
    renderer.endShowFrame();
    // Still lost on the next call; the hook fired once but the latch holds.
    EXPECT_FALSE(renderer.beginShowFrame());
    renderer.endShowFrame();
}

} // namespace
