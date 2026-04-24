#include "entity/media/Decoder.hpp"
#include "entity/media/ProResDecoder.hpp"
#include "entity/media/PNGSequenceDecoder.hpp"
#include "entity/media/HAPDecoder.hpp"
#include <algorithm>
#include <filesystem>
#include <iostream>

#ifdef HAVE_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}
#endif

namespace entity {

std::unique_ptr<Decoder> createDecoder(MediaType mediaType) {
    switch (mediaType) {
        case MediaType::VideoProRes4444:
            return std::make_unique<ProResDecoder>();
        case MediaType::VideoHAP:
        case MediaType::VideoHAPAlpha:
        case MediaType::VideoHAPQ:
        case MediaType::VideoHAPR:
            return std::make_unique<HAPDecoder>();
        case MediaType::PNGSequence:
            return std::make_unique<PNGSequenceDecoder>();
        default:
            return nullptr;
    }
}

#ifdef HAVE_FFMPEG

// Probe the container to pick the right MediaType. .mov/.mp4 holds both
// ProRes and HAP — you can't tell from the extension. Open with libavformat
// and inspect codec_id of the first video stream; if HAP, also look at
// codec_tag to distinguish the variant.
static MediaType probeContainer(const std::string& filepath) {
    AVFormatContext* ctx = nullptr;
    if (avformat_open_input(&ctx, filepath.c_str(), nullptr, nullptr) < 0) {
        std::cerr << "detectMediaType: avformat_open_input failed for " << filepath << std::endl;
        return MediaType::Unknown;
    }

    MediaType result = MediaType::Unknown;
    if (avformat_find_stream_info(ctx, nullptr) >= 0) {
        for (unsigned i = 0; i < ctx->nb_streams; ++i) {
            AVCodecParameters* cp = ctx->streams[i]->codecpar;
            if (cp->codec_type != AVMEDIA_TYPE_VIDEO) continue;

            if (cp->codec_id == AV_CODEC_ID_HAP) {
                // Dispatch to the right HAP MediaType based on FourCC.
                const char t0 = static_cast<char>(cp->codec_tag & 0xFF);
                const char t1 = static_cast<char>((cp->codec_tag >> 8) & 0xFF);
                const char t2 = static_cast<char>((cp->codec_tag >> 16) & 0xFF);
                const char t3 = static_cast<char>((cp->codec_tag >> 24) & 0xFF);
                (void)t0; (void)t1; (void)t2; // silence for simpler checks below

                if (t3 == '1')                                 result = MediaType::VideoHAP;
                else if (t3 == '5')                            result = MediaType::VideoHAPAlpha;
                else if (t3 == 'Y' || t3 == 'M')              result = MediaType::VideoHAPQ;
                else if (t3 == '7')                            result = MediaType::VideoHAPR;
                else                                           result = MediaType::VideoHAP; // unknown HAP*, handle as generic
            } else if (cp->codec_id == AV_CODEC_ID_PRORES) {
                result = MediaType::VideoProRes4444;
            } else {
                // Fall back — unknown codec in .mov, default to ProRes path
                // (the generic FFmpeg decoder will handle H.264 etc. even if
                // labeled ProRes — factory picks ProResDecoder which is
                // actually a generic FFmpeg decoder per class documentation).
                result = MediaType::VideoProRes4444;
            }
            break;
        }
    }
    avformat_close_input(&ctx);
    return result;
}

#endif

MediaType detectMediaType(const std::string& filepath) {
    std::filesystem::path path(filepath);
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    // Video codecs — .mov/.mp4 can carry either ProRes or HAP; probe to
    // distinguish (cheap — avformat only reads headers).
    if (ext == ".mov" || ext == ".mp4" || ext == ".m4v" || ext == ".avi") {
#ifdef HAVE_FFMPEG
        MediaType probed = probeContainer(filepath);
        if (probed != MediaType::Unknown) return probed;
#endif
        // Fallback heuristic if no FFmpeg or probe failed: .avi often ships
        // HAP in the wild; .mov defaults to ProRes.
        return (ext == ".avi") ? MediaType::VideoHAP : MediaType::VideoProRes4444;
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
