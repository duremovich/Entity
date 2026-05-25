/**
 * TextureUploader - extracted from D3D12Renderer (Phase B #13).
 *
 * See header for the contract. This file contains the D3D12 mechanics:
 *  - Texture / upload-buffer resource creation (RGBA or BC-compressed)
 *  - SRV descriptor writes
 *  - Row-pitch-aware memcpy into the upload buffer (block-aware for BC)
 *  - CopyTextureRegion recording (no explicit barriers, see Phase C.11 below)
 *
 * Phase C.11 (async copy queue): textures live in D3D12_RESOURCE_STATE_COMMON
 * between uses. The cmdList passed to upload() lives on a COPY queue; implicit
 * promotion handles COMMON → COPY_DEST, implicit decay returns to COMMON at
 * the copy queue's ExecuteCommandLists boundary. The direct queue's draw call
 * later promotes COMMON → PIXEL_SHADER_RESOURCE implicitly and the read-only
 * state decays back to COMMON at that queue's ExecuteCommandLists boundary.
 * COPY queues cannot transition to PIXEL_SHADER_RESOURCE anyway, so the
 * implicit-state model is the only correct option here.
 *
 * All fence / waitForGpu coordination stays in D3D12Renderer — the caller
 * owns the command queues and their sync story.
 */

#include "entity/render/TextureUploader.hpp"
#include <cstring>
#include <iostream>

namespace entity {

namespace {

DXGI_FORMAT dxgiFormatFromTextureFormat(TextureFormat fmt) {
    switch (fmt) {
        case TextureFormat::RGBA8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case TextureFormat::BC1_UNORM:   return DXGI_FORMAT_BC1_UNORM;
        case TextureFormat::BC3_UNORM:   return DXGI_FORMAT_BC3_UNORM;
        case TextureFormat::BC4_UNORM:   return DXGI_FORMAT_BC4_UNORM;
        case TextureFormat::BC6H_UF16:   return DXGI_FORMAT_BC6H_UF16;
        case TextureFormat::BC6H_SF16:   return DXGI_FORMAT_BC6H_SF16;
        case TextureFormat::BC7_UNORM:   return DXGI_FORMAT_BC7_UNORM;
    }
    return DXGI_FORMAT_UNKNOWN;
}

} // anonymous namespace

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
        slot.format = TextureFormat::RGBA8_UNORM;
        slot.allocated = false;
    }
    m_device = nullptr;
    m_srvHeap = nullptr;
    m_srvDescriptorSize = 0;
}

uint32_t TextureUploader::allocateSlot() {
    std::lock_guard<std::mutex> lk(m_slotMutex);
    for (uint32_t i = 0; i < MAX_SLOTS; ++i) {
        if (!m_slots[i].allocated) {
            m_slots[i].allocated = true;
            m_slots[i].width = 0;
            m_slots[i].height = 0;
            m_slots[i].format = TextureFormat::RGBA8_UNORM;
            return i;
        }
    }
    return INVALID_SLOT;
}

void TextureUploader::freeSlot(uint32_t slot) {
    if (slot >= MAX_SLOTS) return;
    std::lock_guard<std::mutex> lk(m_slotMutex);
    auto& s = m_slots[slot];
    s.texture.Reset();
    s.uploadBuffer.Reset();
    s.gpuHandle = {};
    s.width = 0;
    s.height = 0;
    s.format = TextureFormat::RGBA8_UNORM;
    s.allocated = false;
}

bool TextureUploader::isAllocated(uint32_t slot) const {
    if (slot >= MAX_SLOTS) return false;
    return m_slots[slot].allocated;
}

uint32_t TextureUploader::allocatedSlotCount() const {
    std::lock_guard<std::mutex> lk(m_slotMutex);
    uint32_t count = 0;
    for (const auto& s : m_slots) {
        if (s.allocated) count++;
    }
    return count;
}

bool TextureUploader::prepareTexture(uint32_t slot, uint32_t width, uint32_t height,
                                      TextureFormat format) {
    if (slot >= MAX_SLOTS) {
        std::cerr << "TextureUploader::prepareTexture: slot " << slot
                  << " out of bounds (max " << MAX_SLOTS << ")" << std::endl;
        return false;
    }
    std::lock_guard<std::mutex> lk(m_slotMutex);
    Slot& s = m_slots[slot];
    if (!s.allocated) {
        std::cerr << "TextureUploader::prepareTexture: slot " << slot
                  << " not allocated" << std::endl;
        return false;
    }
    // ensureTexture is idempotent on matching dimensions/format: returns true
    // immediately. On mismatch it releases the old resources and re-creates
    // — which is exactly the same behavior the lazy first-upload path takes
    // when dimensions change mid-session. Caller is responsible for GPU
    // synchronization in that case; at project-load time (the intended call
    // site) the slot is brand-new and there's nothing in flight on the GPU.
    return ensureTexture(s, slot, width, height, format);
}

bool TextureUploader::hasTexture(uint32_t slot) const {
    if (slot >= MAX_SLOTS) return false;
    return m_slots[slot].allocated && m_slots[slot].texture != nullptr;
}

D3D12_GPU_DESCRIPTOR_HANDLE TextureUploader::gpuHandle(uint32_t slot) const {
    if (!hasTexture(slot)) return {};
    return m_slots[slot].gpuHandle;
}

bool TextureUploader::uploadWouldResize(uint32_t slot, uint32_t width, uint32_t height,
                                         TextureFormat format) const {
    if (slot >= MAX_SLOTS) return false;
    const auto& s = m_slots[slot];
    if (!s.allocated) return false;
    if (!s.texture) return true;  // First upload allocates
    return s.width != width || s.height != height || s.format != format;
}

bool TextureUploader::ensureTexture(Slot& slot, uint32_t slotIndex,
                                     uint32_t width, uint32_t height,
                                     TextureFormat format) {
    const DXGI_FORMAT dxgi = dxgiFormatFromTextureFormat(format);
    if (dxgi == DXGI_FORMAT_UNKNOWN) {
        std::cerr << "TextureUploader: unknown TextureFormat for slot " << slotIndex << std::endl;
        return false;
    }

    // Already correct — nothing to do
    if (slot.texture && slot.width == width && slot.height == height && slot.format == format) {
        return true;
    }

    // Dimensions or format changed — release old GPU resources. Caller was
    // responsible for GPU synchronization (waitForGpu) before calling us.
    if (slot.texture) {
        slot.texture.Reset();
        slot.uploadBuffer.Reset();
        slot.width = 0;
        slot.height = 0;
    }

    // Create the GPU texture in COMMON state (Phase C.11). The COPY queue
    // command list does NOT emit an explicit barrier — implicit promotion
    // takes COMMON → COPY_DEST and implicit decay returns to COMMON at
    // ExecuteCommandLists. The direct queue's later draw promotes to
    // PIXEL_SHADER_RESOURCE the same way.
    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = dxgi;
    textureDesc.SampleDesc.Count = 1;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT hr = m_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE,
        &textureDesc, D3D12_RESOURCE_STATE_COMMON,
        nullptr, IID_PPV_ARGS(&slot.texture));
    if (FAILED(hr)) {
        std::cerr << "TextureUploader: CreateCommittedResource (texture) failed for slot "
                  << slotIndex << ", HRESULT 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }

    // Create per-slot UPLOAD-heap staging buffer sized for the texture footprint.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;
    m_device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);

    D3D12_HEAP_PROPERTIES uploadHeapProps = {};
    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC uploadBufferDesc = {};
    uploadBufferDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadBufferDesc.Width            = totalBytes;
    uploadBufferDesc.Height           = 1;
    uploadBufferDesc.DepthOrArraySize = 1;
    uploadBufferDesc.MipLevels        = 1;
    uploadBufferDesc.Format           = DXGI_FORMAT_UNKNOWN;
    uploadBufferDesc.SampleDesc.Count = 1;
    uploadBufferDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

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
    srvDesc.Format = dxgi;
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
    slot.format = format;

    std::cout << "TextureUploader: created slot " << slotIndex << " ("
              << width << "x" << height << ", dxgi=" << static_cast<int>(dxgi) << ")" << std::endl;
    return true;
}

bool TextureUploader::copyPixelsAndRecord(ID3D12GraphicsCommandList* cmdList,
                                           Slot& slot,
                                           const uint8_t* data,
                                           uint32_t width,
                                           uint32_t height,
                                           TextureFormat format) {
    (void)format; // pitch is derived from footprint below; format selected the texture desc earlier
    (void)width;
    (void)height;

    // Query upload buffer footprint (handles row-pitch alignment automatically
    // for BOTH RGBA and BC formats — rowSizeInBytes is the unpadded source
    // stride per row of pixels OR per row of 4x4 blocks).
    D3D12_RESOURCE_DESC textureDesc = slot.texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0;
    UINT64 totalBytes = 0;
    m_device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);

    // Map per-slot upload buffer and memcpy source with row-pitch fix-up.
    // Source stride is rowSizeInBytes (unpadded); destination stride is
    // footprint.RowPitch (aligned to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT=256).
    void* mappedData = nullptr;
    D3D12_RANGE readRange = { 0, 0 };
    HRESULT hr = slot.uploadBuffer->Map(0, &readRange, &mappedData);
    if (FAILED(hr)) {
        std::cerr << "TextureUploader: upload buffer Map failed, HRESULT 0x"
                  << std::hex << hr << std::dec << std::endl;
        return false;
    }

    uint8_t* dst = static_cast<uint8_t*>(mappedData) + footprint.Offset;
    const uint8_t* src = data;
    for (UINT row = 0; row < numRows; ++row) {
        std::memcpy(dst + row * footprint.Footprint.RowPitch,
                    src + row * rowSizeInBytes,
                    rowSizeInBytes);
    }
    slot.uploadBuffer->Unmap(0, nullptr);

    // No explicit barriers (Phase C.11). Texture rests in COMMON; implicit
    // promotion handles COMMON → COPY_DEST here, and the direct queue's draw
    // later promotes COMMON → PIXEL_SHADER_RESOURCE. Decay returns to COMMON
    // at each queue's ExecuteCommandLists boundary.
    D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
    srcLocation.pResource      = slot.uploadBuffer.Get();
    srcLocation.Type           = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLocation.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
    dstLocation.pResource        = slot.texture.Get();
    dstLocation.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLocation.SubresourceIndex = 0;

    cmdList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);

    return true;
}

bool TextureUploader::upload(ID3D12GraphicsCommandList* cmdList,
                              uint32_t slot,
                              const uint8_t* data,
                              uint32_t width,
                              uint32_t height,
                              TextureFormat format) {
    if (!m_device || !cmdList || !data || width == 0 || height == 0) {
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
    if (!ensureTexture(s, slot, width, height, format)) {
        return false;
    }
    return copyPixelsAndRecord(cmdList, s, data, width, height, format);
}


bool TextureUploader::recordDirectCopy(ID3D12GraphicsCommandList* cmdList,
                                        uint32_t slot,
                                        ID3D12Resource* uploadResource,
                                        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
                                        uint32_t width,
                                        uint32_t height,
                                        TextureFormat format) {
    if (!m_device || !cmdList || !uploadResource || width == 0 || height == 0) {
        return false;
    }
    if (slot >= MAX_SLOTS) {
        std::cerr << "TextureUploader: recordDirectCopy slot " << slot
                  << " out of bounds" << std::endl;
        return false;
    }
    Slot& s = m_slots[slot];
    if (!s.allocated) {
        std::cerr << "TextureUploader: recordDirectCopy slot " << slot
                  << " not allocated" << std::endl;
        return false;
    }

    // Ensure the GPU texture exists with the correct dimensions + format.
    // On a cold slot this creates the texture (matching prepareTexture).
    // On a dimension change it recreates — caller should have called
    // waitForGpu() before this (same contract as upload()).
    if (!ensureTexture(s, slot, width, height, format)) {
        return false;
    }

    // Source: the UploadHeapBuffer resource with the pre-computed footprint.
    // The footprint was produced by UploadHeapBufferPool::acquireForTexture
    // (stored in UploadHeapBuffer::footprint) and the decoder wrote data with
    // RowPitch stride — so this layout exactly matches the buffer contents.
    D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
    srcLocation.pResource        = uploadResource;
    srcLocation.Type             = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLocation.PlacedFootprint  = footprint;

    // Destination: the slot's DEFAULT-heap texture, subresource 0.
    D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
    dstLocation.pResource        = s.texture.Get();
    dstLocation.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLocation.SubresourceIndex = 0;

    // No explicit barriers needed. CopyTextureRegion runs on the DIRECT queue's
    // command list (the show list, not the COPY list). Implicit state promotion:
    // UPLOAD-heap source (COMMON → GENERIC_READ); DEFAULT-heap destination
    // (COMMON → COPY_DEST). Both decay back to COMMON at ExecuteCommandLists.
    cmdList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, nullptr);
    return true;
}

} // namespace entity
