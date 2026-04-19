/**
 * TextureUploader - extracted from D3D12Renderer (Phase B #13).
 *
 * See header for the contract. This file contains the D3D12 mechanics:
 *  - Texture / upload-buffer resource creation
 *  - SRV descriptor writes
 *  - Row-pitch-aware memcpy into the upload buffer
 *  - Command-list barrier + CopyTextureRegion recording
 *
 * All fence / waitForGpu coordination stays in D3D12Renderer — the caller
 * owns the command queue and its sync story.
 */

#include "entity/render/TextureUploader.hpp"
#include <cstring>
#include <iostream>

namespace entity {

Result TextureUploader::initialize(ID3D12Device* device,
                                    ID3D12DescriptorHeap* srvHeap,
                                    uint32_t srvDescriptorSize) {
    if (!device || !srvHeap || srvDescriptorSize == 0) {
        return Result::InvalidParameter;
    }
    m_device = device;
    m_srvHeap = srvHeap;
    m_srvDescriptorSize = srvDescriptorSize;
    return Result::Success;
}

void TextureUploader::shutdown() {
    for (auto& slot : m_slots) {
        slot.texture.Reset();
        slot.uploadBuffer.Reset();
        slot.gpuHandle = {};
        slot.width = 0;
        slot.height = 0;
        slot.allocated = false;
        slot.firstUpload = true;
    }
    m_device = nullptr;
    m_srvHeap = nullptr;
    m_srvDescriptorSize = 0;
}

uint32_t TextureUploader::allocateSlot() {
    for (uint32_t i = 0; i < MAX_SLOTS; ++i) {
        if (!m_slots[i].allocated) {
            m_slots[i].allocated = true;
            m_slots[i].firstUpload = true;
            m_slots[i].width = 0;
            m_slots[i].height = 0;
            return i;
        }
    }
    return INVALID_SLOT;
}

void TextureUploader::freeSlot(uint32_t slot) {
    if (slot >= MAX_SLOTS) return;
    auto& s = m_slots[slot];
    s.texture.Reset();
    s.uploadBuffer.Reset();
    s.gpuHandle = {};
    s.width = 0;
    s.height = 0;
    s.allocated = false;
    s.firstUpload = true;
}

bool TextureUploader::isAllocated(uint32_t slot) const {
    if (slot >= MAX_SLOTS) return false;
    return m_slots[slot].allocated;
}

bool TextureUploader::hasTexture(uint32_t slot) const {
    if (slot >= MAX_SLOTS) return false;
    return m_slots[slot].allocated && m_slots[slot].texture != nullptr;
}

D3D12_GPU_DESCRIPTOR_HANDLE TextureUploader::gpuHandle(uint32_t slot) const {
    if (!hasTexture(slot)) return {};
    return m_slots[slot].gpuHandle;
}

bool TextureUploader::uploadWouldResize(uint32_t slot, uint32_t width, uint32_t height) const {
    if (slot >= MAX_SLOTS) return false;
    const auto& s = m_slots[slot];
    if (!s.allocated) return false;
    if (!s.texture) return true;  // First upload allocates
    return s.width != width || s.height != height;
}

bool TextureUploader::ensureTexture(Slot& slot, uint32_t slotIndex,
                                     uint32_t width, uint32_t height) {
    // Already sized correctly — nothing to do
    if (slot.texture && slot.width == width && slot.height == height) {
        return true;
    }

    // Size changed — release old resources. Caller was responsible for GPU
    // synchronization (waitForGpu) before calling us. See uploadWouldResize().
    if (slot.texture) {
        slot.texture.Reset();
        slot.uploadBuffer.Reset();
        slot.width = 0;
        slot.height = 0;
        slot.firstUpload = true;
    }

    // Create the GPU texture (DEFAULT heap, COPY_DEST initial state)
    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT hr = m_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE,
        &textureDesc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&slot.texture));
    if (FAILED(hr)) {
        std::cerr << "TextureUploader: CreateCommittedResource (texture) failed for slot "
                  << slotIndex << ", HRESULT 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    // Create the staging upload buffer (UPLOAD heap, GENERIC_READ state)
    UINT64 uploadBufferSize = 0;
    m_device->GetCopyableFootprints(&textureDesc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadBufferSize);

    D3D12_HEAP_PROPERTIES uploadHeapProps = {};
    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC uploadBufferDesc = {};
    uploadBufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadBufferDesc.Width = uploadBufferSize;
    uploadBufferDesc.Height = 1;
    uploadBufferDesc.DepthOrArraySize = 1;
    uploadBufferDesc.MipLevels = 1;
    uploadBufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    uploadBufferDesc.SampleDesc.Count = 1;
    uploadBufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    hr = m_device->CreateCommittedResource(
        &uploadHeapProps, D3D12_HEAP_FLAG_NONE,
        &uploadBufferDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&slot.uploadBuffer));
    if (FAILED(hr)) {
        std::cerr << "TextureUploader: CreateCommittedResource (upload buffer) failed for slot "
                  << slotIndex << ", HRESULT 0x" << std::hex << hr << std::dec << std::endl;
        slot.texture.Reset();
        return false;
    }

    // Write the SRV descriptor at the heap index owned by DescriptorHeapLayout
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;

    const uint32_t descIdx = DescriptorHeapLayout::videoTextureSlot(slotIndex);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = DescriptorHeapLayout::cpuHandle(
        m_srvHeap, descIdx, m_srvDescriptorSize);
    m_device->CreateShaderResourceView(slot.texture.Get(), &srvDesc, cpu);

    slot.gpuHandle = DescriptorHeapLayout::gpuHandle(
        m_srvHeap, descIdx, m_srvDescriptorSize);
    slot.width = width;
    slot.height = height;

    std::cout << "TextureUploader: created slot " << slotIndex << " ("
              << width << "x" << height << ")" << std::endl;
    return true;
}

bool TextureUploader::copyPixelsAndRecord(ID3D12GraphicsCommandList* cmdList,
                                           Slot& slot,
                                           const uint8_t* rgba,
                                           uint32_t width,
                                           uint32_t height) {
    // Query upload buffer footprint (row pitch alignment)
    D3D12_RESOURCE_DESC textureDesc = slot.texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;
    m_device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);

    // Map upload buffer and memcpy rgba with row-pitch fix-up
    void* mappedData = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    HRESULT hr = slot.uploadBuffer->Map(0, &readRange, &mappedData);
    if (FAILED(hr)) {
        std::cerr << "TextureUploader: upload buffer Map failed, HRESULT 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    uint8_t* dst = static_cast<uint8_t*>(mappedData) + footprint.Offset;
    const uint8_t* src = rgba;
    const UINT srcRowPitch = width * 4;  // RGBA tightly packed

    for (UINT row = 0; row < numRows; ++row) {
        std::memcpy(dst + row * footprint.Footprint.RowPitch,
                    src + row * srcRowPitch,
                    srcRowPitch);
    }
    slot.uploadBuffer->Unmap(0, nullptr);

    // Record barriers + copy onto the command list
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = slot.texture.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    // On first upload the texture is already in COPY_DEST state (from creation),
    // so we skip the PIXEL_SHADER_RESOURCE → COPY_DEST transition.
    if (!slot.firstUpload) {
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        cmdList->ResourceBarrier(1, &barrier);
    }
    slot.firstUpload = false;

    D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
    srcLocation.pResource = slot.uploadBuffer.Get();
    srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLocation.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
    dstLocation.pResource = slot.texture.Get();
    dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLocation.SubresourceIndex = 0;

    cmdList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);

    // Transition back to shader resource state
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    cmdList->ResourceBarrier(1, &barrier);

    return true;
}

bool TextureUploader::upload(ID3D12GraphicsCommandList* cmdList,
                              uint32_t slot,
                              const uint8_t* rgba,
                              uint32_t width,
                              uint32_t height) {
    if (!m_device || !cmdList || !rgba || width == 0 || height == 0) {
        return false;
    }
    if (slot >= MAX_SLOTS) {
        std::cerr << "TextureUploader: slot " << slot << " out of bounds (max "
                  << MAX_SLOTS << ")" << std::endl;
        return false;
    }
    Slot& s = m_slots[slot];
    if (!s.allocated) {
        std::cerr << "TextureUploader: slot " << slot << " not allocated" << std::endl;
        return false;
    }

    if (!ensureTexture(s, slot, width, height)) {
        return false;
    }
    return copyPixelsAndRecord(cmdList, s, rgba, width, height);
}

} // namespace entity
