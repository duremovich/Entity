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
 *   [SNAPSHOT_BASE, TOTAL_SLOTS):                   Compose-target snapshots (read-back
 *                                                   for shader-blend modes; one per screen)
 */

#include <d3d12.h>
#include <cstdint>

namespace entity {

class DescriptorHeapLayout {
public:
    // Capacity limits. Must match IRenderer's public constants.
    static constexpr uint32_t MAX_VIDEO_TEXTURE_SLOTS = 16;
    static constexpr uint32_t MAX_COMPOSE_TARGETS     = 8;

    // Fixed layout — changing these shifts runtime descriptor references,
    // so validate carefully if you need to.
    static constexpr uint32_t IMGUI_SLOT           = 0;
    static constexpr uint32_t LEGACY_VIDEO_SLOT    = 1;
    static constexpr uint32_t COMPOSE_TARGET_BASE  = 2;
    static constexpr uint32_t VIDEO_TEXTURE_BASE   = COMPOSE_TARGET_BASE + MAX_COMPOSE_TARGETS;
    static constexpr uint32_t SNAPSHOT_BASE        = VIDEO_TEXTURE_BASE + MAX_VIDEO_TEXTURE_SLOTS;
    static constexpr uint32_t TOTAL_SLOTS          = SNAPSHOT_BASE + MAX_COMPOSE_TARGETS;

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

    // Bounds checks (useful in allocation paths)
    static constexpr bool isComposeIndexValid(uint32_t composeIndex) {
        return composeIndex < MAX_COMPOSE_TARGETS;
    }
    static constexpr bool isVideoIndexValid(uint32_t videoIndex) {
        return videoIndex < MAX_VIDEO_TEXTURE_SLOTS;
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
