#pragma once

/**
 * DescriptorHeapLayout - Compile-time layout for the D3D12 shader-visible
 * CBV/SRV/UAV heap used by D3D12Renderer.
 *
 * Consolidates the scattered slot-index math that was previously duplicated
 * inline as `2 + MAX_COMPOSE_TARGETS + slot` etc. No runtime state — the
 * D3D12DescriptorHeap resource and descriptor increment size still live on
 * D3D12Renderer. Helpers take the heap + descriptor size as parameters so
 * the layout class stays stateless and header-only.
 *
 * Layout:
 *   [0]:                                            ImGui fonts/textures
 *   [1]:                                            Legacy single-video path
 *   [COMPOSE_TARGET_BASE, VIDEO_TEXTURE_BASE):      Compose targets (one per screen)
 *   [VIDEO_TEXTURE_BASE, SNAPSHOT_BASE):            Video texture slots (per clip)
 *   [SNAPSHOT_BASE, OCIO_LUT_BASE):                 Compose-target snapshots (read-back
 *                                                   for shader-blend modes; one per screen)
 *   [OCIO_LUT_BASE, COMPOSE_TARGET_TRIPLE_BASE):    OCIO LUT SRVs — Phase C.12 #4. The
 *                                                   ACES Studio Config typically uses
 *                                                   6-12 LUTs across input + display
 *                                                   processors; 32 slots gives comfortable
 *                                                   headroom for custom .ocio configs.
 *   [COMPOSE_TARGET_TRIPLE_BASE, STAGE_TARGET_BASE): Triple-buffer extra slots for compose
 *                                                   targets (sub-indices 1 and 2).
 *   [STAGE_TARGET_BASE, TOTAL_SLOTS):               Editor stage 3D offscreen color SRVs
 *                                                   (FRAME_COUNT slots, double-buffered).
 */

#include <d3d12.h>
#include <cstdint>

namespace entity {

class DescriptorHeapLayout {
public:
    // Capacity limits. Must match IRenderer's public constants.
    static constexpr uint32_t MAX_VIDEO_TEXTURE_SLOTS = 16;
    static constexpr uint32_t MAX_COMPOSE_TARGETS     = 8;
    static constexpr uint32_t MAX_OCIO_LUT_SLOTS      = 32;
    static constexpr uint32_t MAX_STAGE_TARGETS       = 2; // FRAME_COUNT — double-buffered color SRVs

    // Fixed layout — changing these shifts runtime descriptor references,
    // so validate carefully if you need to.
    static constexpr uint32_t IMGUI_SLOT           = 0;
    static constexpr uint32_t LEGACY_VIDEO_SLOT    = 1;
    static constexpr uint32_t COMPOSE_TARGET_BASE  = 2;
    static constexpr uint32_t VIDEO_TEXTURE_BASE   = COMPOSE_TARGET_BASE + MAX_COMPOSE_TARGETS;
    static constexpr uint32_t SNAPSHOT_BASE        = VIDEO_TEXTURE_BASE + MAX_VIDEO_TEXTURE_SLOTS;
    static constexpr uint32_t OCIO_LUT_BASE        = SNAPSHOT_BASE + MAX_COMPOSE_TARGETS;
    // Stage 3d triple-buffer extra slots. Sub-index 0 reuses composeTargetSlot;
    // sub-indices 1 and 2 live in this region. Placed after OCIO LUTs so no
    // existing slot numbers shift.
    //   COMPOSE_TARGET_TRIPLE_BASE + composeIndex * 2 + (subIndex - 1)
    static constexpr uint32_t COMPOSE_TARGET_TRIPLE_BASE = OCIO_LUT_BASE + MAX_OCIO_LUT_SLOTS;
    // Stage render target color SRVs (one per frame, for the editor 3D preview).
    static constexpr uint32_t STAGE_TARGET_BASE    = COMPOSE_TARGET_TRIPLE_BASE + MAX_COMPOSE_TARGETS * 2;
    static constexpr uint32_t TOTAL_SLOTS          = STAGE_TARGET_BASE + MAX_STAGE_TARGETS;

    // Slot-index computation (the "2 + MAX_COMPOSE_TARGETS + slot" math)
    static constexpr uint32_t composeTargetSlot(uint32_t composeIndex) {
        return COMPOSE_TARGET_BASE + composeIndex;
    }
    static constexpr uint32_t videoTextureSlot(uint32_t videoIndex) {
        return VIDEO_TEXTURE_BASE + videoIndex;
    }
    // Snapshot SRV that holds a copy of compose target N taken just before each
    // shader-blend draw. Lets the blend pixel shader sample (fg, bg) when D3D12
    // fixed-function blend can't express the mode (Overlay, Difference, etc.).
    static constexpr uint32_t snapshotSlot(uint32_t composeIndex) {
        return SNAPSHOT_BASE + composeIndex;
    }
    // OCIO LUT SRV slot. lutIndex is allocated by OcioManager — there is no
    // fixed mapping from processor → slot, since LUTs are pooled across all
    // active processors (input transforms + display transforms).
    static constexpr uint32_t ocioLutSlot(uint32_t lutIndex) {
        return OCIO_LUT_BASE + lutIndex;
    }
    static constexpr uint32_t TOTAL_SLOTS_WITH_TRIPLE    = COMPOSE_TARGET_TRIPLE_BASE + MAX_COMPOSE_TARGETS * 2;
    static constexpr uint32_t composeTargetTripleSlot(uint32_t composeIndex, uint32_t subIndex) {
        // subIndex 0: use existing composeTargetSlot
        if (subIndex == 0) return composeTargetSlot(composeIndex);
        return COMPOSE_TARGET_TRIPLE_BASE + composeIndex * 2 + (subIndex - 1);
    }
    // Stage render target SRV slot for the editor 3D offscreen color buffer.
    static constexpr uint32_t stageTargetSlot(uint32_t frameIndex) {
        return STAGE_TARGET_BASE + frameIndex;
    }

    // Bounds checks (useful in allocation paths)
    static constexpr bool isComposeIndexValid(uint32_t composeIndex) {
        return composeIndex < MAX_COMPOSE_TARGETS;
    }
    static constexpr bool isVideoIndexValid(uint32_t videoIndex) {
        return videoIndex < MAX_VIDEO_TEXTURE_SLOTS;
    }
    static constexpr bool isOcioLutIndexValid(uint32_t lutIndex) {
        return lutIndex < MAX_OCIO_LUT_SLOTS;
    }

    // Handle-from-slot helpers. Caller supplies the live heap and the
    // descriptor increment size (D3D12 queries these on device creation).
    static D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle(ID3D12DescriptorHeap* heap,
                                                  uint32_t slotIndex,
                                                  uint32_t descriptorSize) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(slotIndex) * descriptorSize;
        return h;
    }
    static D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle(ID3D12DescriptorHeap* heap,
                                                  uint32_t slotIndex,
                                                  uint32_t descriptorSize) {
        D3D12_GPU_DESCRIPTOR_HANDLE h = heap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<UINT64>(slotIndex) * descriptorSize;
        return h;
    }
};

} // namespace entity
