#include "entity/media/HAPDecoder.hpp"
#include "entity/media/HapFormat.hpp"

#include <iostream>
#include <cstring>

#ifdef HAVE_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
}
#endif

namespace entity {

namespace {

#ifdef HAVE_FFMPEG

// Map HAP FourCC codec_tag → (MediaType, hasAlpha). The codec_tag is a
// LE-packed ASCII quad; compare via memcmp after unpacking. Per HAP spec,
// codec_tag values are 'Hap1', 'Hap5', 'HapY', 'Hap7', 'HapA', 'HapH',
// 'HapM' (multi-image YCoCg+Alpha).
struct VariantInfo {
    MediaType mediaType;
    bool hasAlpha;
    const char* name;
};

VariantInfo describeCodecTag(uint32_t codecTag) {
    char tag[5] = {0};
    tag[0] = static_cast<char>(codecTag & 0xFF);
    tag[1] = static_cast<char>((codecTag >> 8) & 0xFF);
    tag[2] = static_cast<char>((codecTag >> 16) & 0xFF);
    tag[3] = static_cast<char>((codecTag >> 24) & 0xFF);

    if (std::memcmp(tag, "Hap1", 4) == 0) return {MediaType::VideoHAP,      false, "Hap (BC1 RGB)"};
    if (std::memcmp(tag, "Hap5", 4) == 0) return {MediaType::VideoHAPAlpha, true,  "Hap Alpha (BC3 RGBA)"};
    if (std::memcmp(tag, "HapY", 4) == 0) return {MediaType::VideoHAPQ,    false, "Hap Q (YCoCg BC3)"};
    if (std::memcmp(tag, "HapM", 4) == 0) return {MediaType::VideoHAPQ,    true,  "Hap Q Alpha (YCoCg+A)"};
    if (std::memcmp(tag, "Hap7", 4) == 0) return {MediaType::VideoHAPR,    true,  "Hap R (BC7 RGBA)"};
    if (std::memcmp(tag, "HapA", 4) == 0) return {MediaType::VideoHAP,      false, "Hap Alpha-only (BC4)"};
    if (std::memcmp(tag, "HapH", 4) == 0) return {MediaType::VideoHAP,      false, "Hap HDR (BC6H)"};
    return {MediaType::VideoHAP, false, "unknown-HAP-variant"};
}

// Convert HapFormat's pixel format into the renderer-facing TextureFormat.
TextureFormat textureFormatFor(HapPixelFormat fmt) {
    switch (fmt) {
        case HapPixelFormat::BC1_UNORM: return TextureFormat::BC1_UNORM;
        case HapPixelFormat::BC3_UNORM: return TextureFormat::BC3_UNORM;
        case HapPixelFormat::BC4_UNORM: return TextureFormat::BC4_UNORM;
        case HapPixelFormat::BC6H_UF16: return TextureFormat::BC6H_UF16;
        case HapPixelFormat::BC6H_SF16: return TextureFormat::BC6H_SF16;
        case HapPixelFormat::BC7_UNORM: return TextureFormat::BC7_UNORM;
        default:                        return TextureFormat::RGBA8_UNORM;
    }
}

// Map the HAP-spec colour space onto the renderer-side hint. The PS branches
// on this and undoes HAP Q's scaled-YCoCg encoding before composition. HDR /
// alpha-only return Linear for now — the HDR FP16 path waits on Phase C.12,
// and HapA is single-channel, no colour conversion.
TextureColorSpace textureColorSpaceFor(HapColorSpace cs) {
    switch (cs) {
        case HapColorSpace::YCoCg_scaled: return TextureColorSpace::YCoCg_scaled;
        case HapColorSpace::RGB:
        case HapColorSpace::Alpha:
        case HapColorSpace::HDR_float:
        default:                          return TextureColorSpace::Linear;
    }
}

// Phase C.12 #6 — OCIO color-space name for the HAP variant. The HAP spec
// pins RGB content to Rec.709 primaries with sRGB-like piecewise EOTF, so
// "Linear Rec.709 (sRGB)" is the right OCIO source space for both Hap1 /
// Hap5 / Hap7 RGB *and* HapY post-YCoCg-decode (the in-shader YCoCg
// unscale produces linear Rec.709 RGB the OCIO input transform then
// promotes to ACEScg). HapA is a single-channel matte → tag "Raw" so the
// OCIO input transform is identity. HapH (BC6H_UF16) is HDR float;
// "Linear Rec.709 (sRGB)" is the closest sane default until C.12 #10
// validates the HDR FP16 path.
const char* ocioColorSpaceFor(HapColorSpace cs) {
    switch (cs) {
        case HapColorSpace::Alpha:        return "Raw";
        case HapColorSpace::YCoCg_scaled:
        case HapColorSpace::RGB:
        case HapColorSpace::HDR_float:
        default:                          return "Linear Rec.709 (sRGB)";
    }
}

#endif // HAVE_FFMPEG

} // anonymous namespace

HAPDecoder::HAPDecoder() = default;

HAPDecoder::~HAPDecoder() {
    close();
}

Result HAPDecoder::open(const std::string& filepath) {
    if (m_isOpen) {
        close();
    }
    m_filepath = filepath;

#ifdef HAVE_FFMPEG
    int ret = avformat_open_input(&m_formatContext, filepath.c_str(), nullptr, nullptr);
    if (ret < 0) {
        char buf[256]; av_strerror(ret, buf, sizeof(buf));
        std::cerr << "HAPDecoder: avformat_open_input failed: " << buf << std::endl;
        return Result::FileNotFound;
    }

    ret = avformat_find_stream_info(m_formatContext, nullptr);
    if (ret < 0) {
        std::cerr << "HAPDecoder: avformat_find_stream_info failed" << std::endl;
        close();
        return Result::DecoderError;
    }

    // Find the video stream (should be the only stream in a HAP .mov/.avi)
    m_videoStreamIndex = -1;
    for (unsigned i = 0; i < m_formatContext->nb_streams; ++i) {
        if (m_formatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            m_videoStreamIndex = static_cast<int>(i);
            break;
        }
    }
    if (m_videoStreamIndex < 0) {
        std::cerr << "HAPDecoder: no video stream in " << filepath << std::endl;
        close();
        return Result::UnsupportedFormat;
    }

    AVStream* stream = m_formatContext->streams[m_videoStreamIndex];
    AVCodecParameters* codecpar = stream->codecpar;

    // Sanity: verify this is actually a HAP stream. We refuse non-HAP to keep
    // the decoder-factory routing honest (the factory should dispatch HAP
    // files here; other codecs go to ProResDecoder/etc.).
    if (codecpar->codec_id != AV_CODEC_ID_HAP) {
        std::cerr << "HAPDecoder: stream codec_id is not HAP (got "
                  << codecpar->codec_id << ")" << std::endl;
        close();
        return Result::UnsupportedFormat;
    }

    // Metadata
    m_width  = static_cast<uint32_t>(codecpar->width);
    m_height = static_cast<uint32_t>(codecpar->height);

    if (stream->r_frame_rate.den > 0) {
        m_frameRate = av_q2d(stream->r_frame_rate);
    } else if (stream->avg_frame_rate.den > 0) {
        m_frameRate = av_q2d(stream->avg_frame_rate);
    } else {
        m_frameRate = 30.0;
    }

    if (stream->nb_frames > 0) {
        m_duration = static_cast<FrameNumber>(stream->nb_frames);
    } else if (stream->duration > 0) {
        AVRational fr = (stream->r_frame_rate.den > 0) ? stream->r_frame_rate
                                                        : stream->avg_frame_rate;
        if (fr.num <= 0 || fr.den <= 0) { fr.num = 30; fr.den = 1; }
        m_duration = static_cast<FrameNumber>(
            av_rescale_q(stream->duration, stream->time_base, av_inv_q(fr)));
    } else {
        m_duration = 0;
    }

    // Variant identification via FourCC codec_tag
    VariantInfo vi = describeCodecTag(codecpar->codec_tag);
    m_mediaType = vi.mediaType;
    m_hasAlpha = vi.hasAlpha;

    // Allocate reusable packet
    m_packet = av_packet_alloc();
    if (!m_packet) {
        std::cerr << "HAPDecoder: av_packet_alloc failed" << std::endl;
        close();
        return Result::OutOfMemory;
    }

    std::cout << "HAPDecoder opened " << filepath
              << " [" << vi.name << "] " << m_width << "x" << m_height
              << " @ " << m_frameRate << " fps, " << m_duration << " frames" << std::endl;

    m_isOpen = true;
    m_currentFrame = -1;
    return Result::Success;
#else
    (void)filepath;
    std::cerr << "HAPDecoder: FFmpeg not available" << std::endl;
    return Result::NotImplemented;
#endif
}

void HAPDecoder::close() {
#ifdef HAVE_FFMPEG
    if (m_packet) {
        av_packet_free(&m_packet);
        m_packet = nullptr;
    }
    if (m_codecContext) {
        // We don't actually open a codec (no avcodec_open2), but future code
        // paths might — cleanup is safe against never-opened contexts.
        avcodec_free_context(&m_codecContext);
        m_codecContext = nullptr;
    }
    if (m_formatContext) {
        avformat_close_input(&m_formatContext);
        m_formatContext = nullptr;
    }
    m_frame = nullptr; // never allocated; we bypass libavcodec's decode
#endif
    m_isOpen = false;
    m_width = 0;
    m_height = 0;
    m_duration = 0;
    m_hasAlpha = false;
    m_mediaType = MediaType::VideoHAP;
    m_currentFrame = -1;
    m_videoStreamIndex = -1;
}

Result HAPDecoder::seek(FrameNumber frameNumber) {
#ifdef HAVE_FFMPEG
    if (!m_isOpen || !m_formatContext) return Result::Failure;

    AVStream* stream = m_formatContext->streams[m_videoStreamIndex];
    AVRational fr = (stream->r_frame_rate.den > 0) ? stream->r_frame_rate
                                                   : stream->avg_frame_rate;
    if (fr.num <= 0 || fr.den <= 0) { fr.num = 30; fr.den = 1; }

    // Convert frame number → stream timebase timestamp. HAP is intra-frame
    // so every frame is a keyframe — AVSEEK_FLAG_BACKWARD lands exactly
    // on the target (no keyframe rounding to worry about).
    int64_t ts = av_rescale_q(static_cast<int64_t>(frameNumber),
                              av_inv_q(fr), stream->time_base);

    int ret = av_seek_frame(m_formatContext, m_videoStreamIndex, ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        char buf[256]; av_strerror(ret, buf, sizeof(buf));
        std::cerr << "HAPDecoder: seek failed: " << buf << std::endl;
        return Result::DecoderError;
    }
    m_currentFrame = frameNumber - 1;
    return Result::Success;
#else
    (void)frameNumber;
    return Result::NotImplemented;
#endif
}

Result HAPDecoder::decodeFrame(FrameNumber frameNumber, DecodedFrame& outFrame) {
#ifdef HAVE_FFMPEG
    if (!m_isOpen || !m_packet) {
        return Result::Failure;
    }

    // Seek if the target isn't the next sequential frame. For random access
    // this is free; for forward playback (nextFrame = current + 1) the seek
    // is skipped.
    if (frameNumber != m_currentFrame + 1) {
        Result sr = seek(frameNumber);
        if (sr != Result::Success) return sr;
    }

    // Read packets until we hit the target frame. In practice after a seek
    // the first packet is our target (HAP is all keyframes), but we stay
    // defensive against container-level padding / reordering.
    while (true) {
        av_packet_unref(m_packet);
        int ret = av_read_frame(m_formatContext, m_packet);
        if (ret < 0) {
            // EOF is expected when the reported duration overshoots the actual
            // packet count — common with FFmpeg-encoded HAP, where the muxer's
            // nb_frames sometimes claims one more than `av_read_frame` will
            // deliver. Caller treats EndOfStream as "stop, no error" while
            // still distinguishing it from a real demux/parser failure.
            return (ret == AVERROR_EOF) ? Result::EndOfStream : Result::DecoderError;
        }
        if (m_packet->stream_index != m_videoStreamIndex) {
            continue;
        }
        ++m_currentFrame;
        if (m_currentFrame < frameNumber) {
            continue; // keep reading toward target
        }
        break;
    }

    // Parse the raw HAP packet into BC block data + format metadata.
    // Reuse outFrame.data for the decompressed blocks (capacity is preserved
    // across ring-buffer push/pop, so steady state is allocation-free).
    HapFrame hap;
    hap.textureData = std::move(outFrame.data);
    hap.alphaData   = {};

    if (!parseHapPacket(m_packet->data, static_cast<size_t>(m_packet->size),
                        m_width, m_height, hap)) {
        outFrame.data = std::move(hap.textureData); // restore for retry
        outFrame.valid.store(false, std::memory_order_release);
        std::cerr << "HAPDecoder: parseHapPacket failed at frame " << frameNumber << std::endl;
        return Result::DecoderError;
    }

    // HapM multi-image (YCoCg + BC4 alpha) isn't fully wired through the
    // renderer yet — it needs a second texture slot for the alpha plane.
    // For now, play the color plane and drop alpha. TODO when any user
    // actually hits this path.
    if (hap.alphaFormat != HapPixelFormat::Unknown) {
        static bool warned = false;
        if (!warned) {
            std::cerr << "HAPDecoder: HapM second (alpha) plane not yet supported — "
                         "color plane only" << std::endl;
            warned = true;
        }
    }

    outFrame.data = std::move(hap.textureData);
    outFrame.frameNumber = frameNumber;
    outFrame.width = m_width;
    outFrame.height = m_height;
    outFrame.pts = m_packet->pts;
    outFrame.format = textureFormatFor(hap.format);
    outFrame.colorSpace = textureColorSpaceFor(hap.colorSpace);
    outFrame.ocioColorSpace = ocioColorSpaceFor(hap.colorSpace);
    outFrame.valid.store(true, std::memory_order_release);
    return Result::Success;
#else
    (void)frameNumber;
    (void)outFrame;
    return Result::NotImplemented;
#endif
}

Result HAPDecoder::detectHAPVariant() {
    // Variant is detected at open() via codec_tag; nothing to do here.
    // Kept for API compatibility with the original stub.
    return Result::Success;
}

Result HAPDecoder::convertToRGBA(AVFrame* srcFrame, DecodedFrame& outFrame) {
    // Not used — HAP decoding skips CPU decompression entirely. The parser
    // extracts pre-compressed BC blocks that go straight to a BC GPU
    // texture. Kept as a stub to satisfy the header, though we never call
    // it from this decoder. If a future path needs CPU RGBA (e.g. a
    // software fallback on systems without BC support), implement here by
    // running a block-decompression pass per format.
    (void)srcFrame;
    (void)outFrame;
    return Result::NotImplemented;
}

} // namespace entity
