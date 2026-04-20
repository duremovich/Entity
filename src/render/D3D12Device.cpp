/**
 * D3D12Device implementation. See header for the ownership rationale.
 */

#include "entity/render/D3D12Device.hpp"
#include <iostream>

namespace entity {

Result D3D12Device::initialize(bool enableDebugLayer) {
    if (m_device) {
        std::cerr << "D3D12Device already initialized" << std::endl;
        return Result::Failure;
    }

    // Debug layer — best-effort. If D3D12GetDebugInterface fails (e.g. debug
    // layer not installed) we continue without it rather than fail init.
#if defined(_DEBUG)
    if (enableDebugLayer) {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            std::cout << "D3D12 debug layer enabled" << std::endl;
        }
    }
#else
    (void)enableDebugLayer;
#endif

    HRESULT hr = D3D12CreateDevice(
        nullptr,                    // Use default adapter
        D3D_FEATURE_LEVEL_11_0,     // Minimum feature level
        IID_PPV_ARGS(&m_device)
    );
    if (FAILED(hr)) {
        std::cerr << "Failed to create D3D12 device! HRESULT: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return Result::Failure;
    }
    std::cout << "D3D12 device created" << std::endl;

    // Direct command queue — gfx + compute + copy combined. The one queue
    // matches the single-command-list rendering pattern in D3D12Renderer.
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    hr = m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue));
    if (FAILED(hr)) {
        std::cerr << "Failed to create command queue! HRESULT: 0x"
                  << std::hex << hr << std::dec << std::endl;
        m_device.Reset();
        return Result::Failure;
    }
    std::cout << "Command queue created" << std::endl;

    return Result::Success;
}

void D3D12Device::shutdown() {
    m_commandQueue.Reset();
    m_device.Reset();
}

} // namespace entity
