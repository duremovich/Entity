#pragma once

/**
 * D3D12Device - thin wrapper around the ID3D12Device + ID3D12CommandQueue
 * pair created at D3D12Renderer startup.
 *
 * Ownership boundary for the backend-specific device state. A future
 * MetalDevice would be a peer class; code that cares about "the GPU device"
 * in a backend-agnostic way goes through IRenderer, not this.
 *
 * Scope is intentionally minimal for Phase B #14: only the device object,
 * the command queue, and debug-layer initialization live here. Command
 * list, command allocators, fence, and frame sync remain on D3D12Renderer
 * because they couple tightly to the swap chain's back buffer index, and
 * splitting that cleanly is larger than the scope of this task.
 */

#include "entity/core/Types.hpp"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

namespace entity {

using Microsoft::WRL::ComPtr;

class D3D12Device {
public:
    D3D12Device() = default;
    ~D3D12Device() = default;

    D3D12Device(const D3D12Device&) = delete;
    D3D12Device& operator=(const D3D12Device&) = delete;

    /**
     * Enable the D3D12 debug layer (debug builds only, best-effort) and
     * create the device + direct command queue.
     * @param enableDebugLayer If true, call EnableDebugLayer() before device
     *   creation. Ignored on release builds.
     */
    Result initialize(bool enableDebugLayer);

    /**
     * Release the device and command queue.
     */
    void shutdown();

    ID3D12Device*       device() const       { return m_device.Get(); }
    ID3D12CommandQueue* commandQueue() const { return m_commandQueue.Get(); }

    // Dedicated COPY queue for async texture uploads (Phase C.11). Lets
    // TextureUploader's CopyTextureRegion work overlap with composite/render on
    // the direct queue. Cross-queue ordering is enforced by an upload fence
    // owned by D3D12Renderer (copy queue Signals, direct queue Wait()s).
    ID3D12CommandQueue* copyQueue() const    { return m_copyQueue.Get(); }

    bool isInitialized() const { return m_device != nullptr; }

    // GPU adapter description captured at device-creation time.
    // VendorId / DeviceId / Description[128] (UTF-16).
    // All-zero if the adapter enumeration failed (e.g. WARP).
    const DXGI_ADAPTER_DESC1& adapterDesc() const { return m_adapterDesc; }

private:
    ComPtr<ID3D12Device>       m_device;
    ComPtr<ID3D12CommandQueue> m_commandQueue;
    ComPtr<ID3D12CommandQueue> m_copyQueue;
    DXGI_ADAPTER_DESC1         m_adapterDesc{};
};

} // namespace entity
