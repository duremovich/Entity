#include "entity/media/DecodedFrame.hpp"
#include "entity/render/UploadHeapBufferPool.hpp"

// The accessor implementations live here rather than as inlines in the header
// because UploadHeapBuffer is defined in UploadHeapBufferPool.hpp, which pulls
// in <d3d12.h> and <wrl/client.h>. Keeping those out of DecodedFrame.hpp lets
// CPU-only consumers (unit tests, server-side code) include the frame type
// without dragging in D3D12 headers. Decoders that only use the CpuHeap path
// don't pay the UploadHeapBuffer include either.

namespace entity {

const uint8_t* DecodedFrame::data() const {
    if (storage == Storage::UploadHeap) {
        return uploadBuf ? uploadBuf->mappedPtr : nullptr;
    }
    return cpuData.data();
}

uint8_t* DecodedFrame::mutableData() {
    if (storage == Storage::UploadHeap) {
        return uploadBuf ? uploadBuf->mappedPtr : nullptr;
    }
    return cpuData.data();
}

size_t DecodedFrame::size() const {
    if (storage == Storage::UploadHeap) {
        return uploadBuf ? uploadBuf->size : 0;
    }
    return cpuData.size();
}

uint32_t DecodedFrame::uploadRowPitch() const {
    if (storage == Storage::UploadHeap && uploadBuf) {
        return uploadBuf->footprint.Footprint.RowPitch;
    }
    // CpuHeap path: tight-packed (no padding). Callers use width*bpp directly.
    return 0;
}

} // namespace entity
