/**
 * D3D12Device implementation. See header for the ownership rationale.
 */

#include "entity/render/D3D12Device.hpp"
#include <cstdlib>
#include <dxgi1_4.h>
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

    // ENTITY_FORCE_WARP=1 selects the Windows Advanced Rasterization Platform
    // (software adapter). Used by integration tests on CI for cross-machine
    // determinism — hardware GPUs produce different bit patterns for the same
    // draw calls (filtering, blend rounding), which breaks pixel-exact golden
    // hashes. WARP is byte-deterministic across machines.
    ComPtr<IDXGIAdapter> adapter;
    const char* forceWarp = std::getenv("ENTITY_FORCE_WARP");
    if (forceWarp && forceWarp[0] != '0' && forceWarp[0] != '\0') {
        ComPtr<IDXGIFactory4> factory;
        if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
            if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) {
                std::cout << "D3D12 adapter: WARP (ENTITY_FORCE_WARP=" << forceWarp << ")" << std::endl;
            } else {
                std::cerr << "ENTITY_FORCE_WARP set but EnumWarpAdapter failed; using default adapter" << std::endl;
                adapter.Reset();
            }
        } else {
            std::cerr << "ENTITY_FORCE_WARP set but CreateDXGIFactory2 failed; using default adapter" << std::endl;
        }
    }

    // DRED — must be called before D3D12CreateDevice; settings are global.
    // Gated on _DEBUG: forcing DRED on in release builds causes AV crashes on
    // WARP adapters during compose-target staging-buffer readback (ctest
    // regression confirmed 2026-05-09: 6 tests fail with 0xC0000005 under
    // ENTITY_FORCE_WARP=1 when DRED is forced on in release).
#if defined(_DEBUG)
    if (enableDebugLayer) {
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dred;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            std::cout << "D3D12 DRED enabled (auto-breadcrumbs + page-fault + context)" << std::endl;
        } else {
            std::cerr << "[D3D12Device] DRED enablement failed — breadcrumbs unavailable" << std::endl;
        }
    }
#endif

    HRESULT hr = D3D12CreateDevice(
        adapter.Get(),              // null = default adapter; non-null = WARP
        D3D_FEATURE_LEVEL_11_0,     // Minimum feature level
        IID_PPV_ARGS(&m_device)
    );
    if (FAILED(hr)) {
        std::cerr << "Failed to create D3D12 device! HRESULT: 0x"
                  << std::hex << hr << std::dec << std::endl;
        return Result::Failure;
    }
    std::cout << "D3D12 device created" << std::endl;

    // Capture adapter description (VendorId / DeviceId / Description) for
    // forensic logging. Match by LUID so WARP and hardware adapters both work.
    {
        LUID luid = m_device->GetAdapterLuid();
        ComPtr<IDXGIFactory4> factory4;
        if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory4)))) {
            ComPtr<IDXGIAdapter1> a1;
            for (UINT i = 0; factory4->EnumAdapters1(i, &a1) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 d{};
                if (SUCCEEDED(a1->GetDesc1(&d)) &&
                    d.AdapterLuid.LowPart  == luid.LowPart &&
                    d.AdapterLuid.HighPart == luid.HighPart) {
                    m_adapterDesc = d;
                    break;
                }
                a1.Reset();
            }
        }
    }

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

    // Dedicated COPY queue (Phase C.11). Async texture uploads run here so
    // CopyTextureRegion overlaps with composite/render on the direct queue.
    D3D12_COMMAND_QUEUE_DESC copyQueueDesc = {};
    copyQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    copyQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    hr = m_device->CreateCommandQueue(&copyQueueDesc, IID_PPV_ARGS(&m_copyQueue));
    if (FAILED(hr)) {
        std::cerr << "Failed to create copy command queue! HRESULT: 0x"
                  << std::hex << hr << std::dec << std::endl;
        m_commandQueue.Reset();
        m_device.Reset();
        return Result::Failure;
    }
    std::cout << "Copy command queue created" << std::endl;

    return Result::Success;
}

void D3D12Device::shutdown() {
    m_copyQueue.Reset();
    m_commandQueue.Reset();
    m_device.Reset();
}

} // namespace entity
