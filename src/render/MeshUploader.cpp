/**
 * MeshUploader - vertex/index buffer slot pool for GPU mesh uploads.
 *
 * Upload model:
 *  - Vertex buffer: D3D12_HEAP_TYPE_DEFAULT, COMMON state (Phase C.11 implicit promotion)
 *  - Index buffer:  D3D12_HEAP_TYPE_DEFAULT, COMMON state
 *  - Each has a paired UPLOAD-heap staging buffer used once then retained
 *    until the slot is freed (matches TextureUploader's per-slot upload buffer).
 *  - No explicit barriers — copy queue implicit promotion handles COMMON →
 *    COPY_DEST; direct queue later promotes COMMON → VERTEX/INDEX_BUFFER.
 *
 * Deferred free:
 *  - scheduleSlotFree records {slot, editorFenceValue} in m_pendingFrees.
 *    The editor fence wraps both copy-queue upload and direct-queue draw work,
 *    so when its completed value passes the recorded value both are guaranteed
 *    done. Using the upload fence alone would be wrong: it only guarantees the
 *    CopyBufferRegion finished, not the subsequent DrawIndexedInstanced.
 *  - onFrameBegin(completedEditorFenceValue) reaps entries whose fence value
 *    has been passed by the GPU.
 */

#include "entity/render/MeshUploader.hpp"
#include <cstring>
#include <iostream>

namespace entity {

bool MeshUploader::initialize(ID3D12Device* device) {
    if (!device) return false;
    m_device = device;
    return true;
}

void MeshUploader::shutdown() {
    {
        std::lock_guard<std::mutex> lk(m_pendingMutex);
        m_pendingFrees.clear();
    }
    std::lock_guard<std::mutex> lk(m_slotMutex);
    for (uint32_t i = 0; i < MAX_MESH_SLOTS; ++i) {
        actuallyFreeSlot(i);
    }
    m_device = nullptr;
}

uint32_t MeshUploader::uploadMesh(ID3D12GraphicsCommandList* cmdList,
                                   const std::vector<MeshVertex>& vertices,
                                   const std::vector<uint32_t>& indices) {
    if (!m_device || !cmdList || vertices.empty() || indices.empty()) {
        return INVALID_SLOT;
    }

    uint32_t slotIdx = INVALID_SLOT;
    {
        std::lock_guard<std::mutex> lk(m_slotMutex);
        for (uint32_t i = 0; i < MAX_MESH_SLOTS; ++i) {
            if (!m_slots[i].allocated) {
                slotIdx = i;
                m_slots[i].allocated = true;
                break;
            }
        }
    }
    if (slotIdx == INVALID_SLOT) {
        std::cerr << "MeshUploader: slot pool exhausted (max " << MAX_MESH_SLOTS << ")\n";
        return INVALID_SLOT;
    }

    Slot& s = m_slots[slotIdx];

    const UINT64 vbBytes = static_cast<UINT64>(vertices.size()) * sizeof(MeshVertex);
    const UINT64 ibBytes = static_cast<UINT64>(indices.size())  * sizeof(uint32_t);

    auto makeBuffer = [&](UINT64 size, D3D12_HEAP_TYPE heapType,
                          D3D12_RESOURCE_STATES initialState,
                          ComPtr<ID3D12Resource>& out) -> bool {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = heapType;

        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width            = size;
        rd.Height           = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        HRESULT hr = m_device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &rd, initialState, nullptr,
            IID_PPV_ARGS(&out));
        if (FAILED(hr)) {
            std::cerr << "MeshUploader: CreateCommittedResource failed slot "
                      << slotIdx << " HRESULT 0x" << std::hex << hr << std::dec << "\n";
            return false;
        }
        return true;
    };

    // Default-heap buffers — implicit promotion COMMON → COPY_DEST on copy queue
    if (!makeBuffer(vbBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, s.vertexBuffer) ||
        !makeBuffer(ibBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, s.indexBuffer)  ||
        !makeBuffer(vbBytes, D3D12_HEAP_TYPE_UPLOAD,  D3D12_RESOURCE_STATE_GENERIC_READ, s.vertexUpload) ||
        !makeBuffer(ibBytes, D3D12_HEAP_TYPE_UPLOAD,  D3D12_RESOURCE_STATE_GENERIC_READ, s.indexUpload)) {
        std::lock_guard<std::mutex> lk(m_slotMutex);
        actuallyFreeSlot(slotIdx);
        return INVALID_SLOT;
    }

    // Copy vertex data into upload buffer
    {
        void* mapped = nullptr;
        D3D12_RANGE readRange{0, 0};
        if (FAILED(s.vertexUpload->Map(0, &readRange, &mapped))) {
            std::cerr << "MeshUploader: vertex upload Map failed slot " << slotIdx << "\n";
            std::lock_guard<std::mutex> lk(m_slotMutex);
            actuallyFreeSlot(slotIdx);
            return INVALID_SLOT;
        }
        std::memcpy(mapped, vertices.data(), static_cast<size_t>(vbBytes));
        s.vertexUpload->Unmap(0, nullptr);
    }

    // Copy index data into upload buffer
    {
        void* mapped = nullptr;
        D3D12_RANGE readRange{0, 0};
        if (FAILED(s.indexUpload->Map(0, &readRange, &mapped))) {
            std::cerr << "MeshUploader: index upload Map failed slot " << slotIdx << "\n";
            std::lock_guard<std::mutex> lk(m_slotMutex);
            actuallyFreeSlot(slotIdx);
            return INVALID_SLOT;
        }
        std::memcpy(mapped, indices.data(), static_cast<size_t>(ibBytes));
        s.indexUpload->Unmap(0, nullptr);
    }

    // Record copy commands — no explicit barriers (Phase C.11 implicit promotion)
    cmdList->CopyBufferRegion(s.vertexBuffer.Get(), 0, s.vertexUpload.Get(), 0, vbBytes);
    cmdList->CopyBufferRegion(s.indexBuffer.Get(),  0, s.indexUpload.Get(),  0, ibBytes);

    // Fill views (GPU address valid immediately after resource creation)
    s.vbv.BufferLocation = s.vertexBuffer->GetGPUVirtualAddress();
    s.vbv.SizeInBytes    = static_cast<UINT>(vbBytes);
    s.vbv.StrideInBytes  = sizeof(MeshVertex);

    s.ibv.BufferLocation = s.indexBuffer->GetGPUVirtualAddress();
    s.ibv.SizeInBytes    = static_cast<UINT>(ibBytes);
    s.ibv.Format         = DXGI_FORMAT_R32_UINT;

    s.indexCount = static_cast<uint32_t>(indices.size());

    std::cout << "MeshUploader: uploaded slot " << slotIdx
              << " (" << vertices.size() << " verts, " << indices.size() << " indices)\n";
    return slotIdx;
}

void MeshUploader::scheduleSlotFree(uint32_t slot, uint64_t currentFenceValue) {
    if (slot >= MAX_MESH_SLOTS) return;
    std::lock_guard<std::mutex> lk(m_pendingMutex);
    m_pendingFrees.push_back({slot, currentFenceValue});
}

void MeshUploader::onFrameBegin(uint64_t completedFenceValue) {
    std::lock_guard<std::mutex> lkPending(m_pendingMutex);
    auto it = m_pendingFrees.begin();
    while (it != m_pendingFrees.end()) {
        if (it->fenceValue <= completedFenceValue) {
            {
                std::lock_guard<std::mutex> lkSlot(m_slotMutex);
                actuallyFreeSlot(it->slot);
            }
            it = m_pendingFrees.erase(it);
        } else {
            ++it;
        }
    }
}

bool MeshUploader::getBindHandle(uint32_t slot, BindHandle& out) const {
    if (slot >= MAX_MESH_SLOTS) return false;
    std::lock_guard<std::mutex> lk(m_slotMutex);
    const Slot& s = m_slots[slot];
    if (!s.allocated || !s.vertexBuffer || !s.indexBuffer) return false;
    out.vbv        = s.vbv;
    out.ibv        = s.ibv;
    out.indexCount = s.indexCount;
    return true;
}

void MeshUploader::actuallyFreeSlot(uint32_t slot) {
    if (slot >= MAX_MESH_SLOTS) return;
    Slot& s = m_slots[slot];
    s.vertexBuffer.Reset();
    s.indexBuffer.Reset();
    s.vertexUpload.Reset();
    s.indexUpload.Reset();
    s.vbv        = {};
    s.ibv        = {};
    s.indexCount = 0;
    s.allocated  = false;
}

} // namespace entity
