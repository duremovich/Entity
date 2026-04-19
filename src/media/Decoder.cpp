#include "entity/media/Decoder.hpp"
#include "entity/media/ProResDecoder.hpp"
#include "entity/media/PNGSequenceDecoder.hpp"
#include <algorithm>
#include <filesystem>
#include <iostream>

namespace entity {

std::unique_ptr<Decoder> createDecoder(MediaType mediaType) {
    switch (mediaType) {
        case MediaType::VideoProRes4444:
            return std::make_unique<ProResDecoder>();
        case MediaType::VideoHAP:
        case MediaType::VideoHAPAlpha:
        case MediaType::VideoHAPQ:
            // HAP decoder is not yet implemented. Returning nullptr so callers
            // surface a clear error instead of constructing a stub that silently
            // fails on every operation. See docs/reference/CODE_ISSUES.md (HIGH-13).
            std::cerr << "createDecoder: HAP codec family is not yet implemented" << std::endl;
            return nullptr;
        case MediaType::PNGSequence:
            return std::make_unique<PNGSequenceDecoder>();
        default:
            return nullptr;
    }
}

MediaType detectMediaType(const std::string& filepath) {
    std::filesystem::path path(filepath);
    std::string ext = path.extension().string();

    // Convert to lowercase for case-insensitive comparison
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    // Video codecs
    if (ext == ".mov" || ext == ".mp4" || ext == ".m4v") {
        // Need to inspect file to determine if ProRes or HAP
        // For now, default to ProRes
        return MediaType::VideoProRes4444;
    }
    else if (ext == ".avi") {
        // HAP codec typically uses AVI container
        return MediaType::VideoHAP;
    }
    // Image sequences
    else if (ext == ".png") {
        return MediaType::PNGSequence;
    }
    else if (ext == ".dpx") {
        return MediaType::DPXSequence;
    }

    return MediaType::Unknown;
}

} // namespace entity
