#include "entity/media/ProResDecoder.hpp"
#include <iostream>
#include <cstring>

#ifdef HAVE_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
}
#endif

namespace entity {

ProResDecoder::ProResDecoder() = default;

ProResDecoder::~ProResDecoder() {
    close();
}

Result ProResDecoder::open(const std::string& filepath) {
    if (m_isOpen) {
        close();
    }

    m_filepath = filepath;

#ifdef HAVE_FFMPEG
    // 1. Open input file
    int ret = avformat_open_input(&m_formatContext, filepath.c_str(), nullptr, nullptr);
    if (ret < 0) {
        char errBuf[256];
        av_strerror(ret, errBuf, sizeof(errBuf));
        std::cerr << "ProResDecoder: Failed to open file: " << errBuf << std::endl;
        return Result::FileNotFound;
    }

    // 2. Find stream info
    ret = avformat_find_stream_info(m_formatContext, nullptr);
    if (ret < 0) {
        char errBuf[256];
        av_strerror(ret, errBuf, sizeof(errBuf));
        std::cerr << "ProResDecoder: Failed to find stream info: " << errBuf << std::endl;
        cleanupFFmpeg();
        return Result::DecoderError;
    }

    // 3. Find video stream
    m_videoStreamIndex = -1;
    AVCodecParameters* codecpar = nullptr;
    for (unsigned int i = 0; i < m_formatContext->nb_streams; ++i) {
        if (m_formatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            m_videoStreamIndex = static_cast<int>(i);
            codecpar = m_formatContext->streams[i]->codecpar;
            break;
        }
    }

    if (m_videoStreamIndex < 0 || !codecpar) {
        std::cerr << "ProResDecoder: No video stream found in file" << std::endl;
        cleanupFFmpeg();
        return Result::DecoderError;
    }

    // 4. Find decoder
    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id);
    if (!codec) {
        std::cerr << "ProResDecoder: Unsupported codec" << std::endl;
        cleanupFFmpeg();
        return Result::DecoderError;
    }

    std::cout << "ProResDecoder: Using codec: " << codec->long_name << std::endl;

    // 5. Create codec context
    m_codecContext = avcodec_alloc_context3(codec);
    if (!m_codecContext) {
        std::cerr << "ProResDecoder: Failed to allocate codec context" << std::endl;
        cleanupFFmpeg();
        return Result::DecoderError;
    }

    ret = avcodec_parameters_to_context(m_codecContext, codecpar);
    if (ret < 0) {
        std::cerr << "ProResDecoder: Failed to copy codec parameters" << std::endl;
        cleanupFFmpeg();
        return Result::DecoderError;
    }

    // Enable multi-threaded decoding for better performance
    m_codecContext->thread_count = 0; // Auto-detect thread count
    m_codecContext->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

    // 6. Open codec
    ret = avcodec_open2(m_codecContext, codec, nullptr);
    if (ret < 0) {
        char errBuf[256];
        av_strerror(ret, errBuf, sizeof(errBuf));
        std::cerr << "ProResDecoder: Failed to open codec: " << errBuf << std::endl;
        cleanupFFmpeg();
        return Result::DecoderError;
    }

    // 7. Extract media properties
    m_width = static_cast<uint32_t>(m_codecContext->width);
    m_height = static_cast<uint32_t>(m_codecContext->height);

    AVStream* stream = m_formatContext->streams[m_videoStreamIndex];
    if (stream->r_frame_rate.den > 0) {
        m_frameRate = av_q2d(stream->r_frame_rate);
    } else if (stream->avg_frame_rate.den > 0) {
        m_frameRate = av_q2d(stream->avg_frame_rate);
    } else {
        m_frameRate = 30.0; // Default fallback
    }

    // Calculate duration in frames using integer arithmetic for precision
    if (stream->nb_frames > 0) {
        // Use nb_frames if available (most reliable)
        m_duration = static_cast<FrameNumber>(stream->nb_frames);
    } else if (stream->duration > 0) {
        // Convert stream duration to frames using av_rescale_q for exact integer arithmetic
        AVRational frameRate;
        if (stream->r_frame_rate.den > 0) {
            frameRate = stream->r_frame_rate;
        } else if (stream->avg_frame_rate.den > 0) {
            frameRate = stream->avg_frame_rate;
        } else {
            frameRate.num = 30;
            frameRate.den = 1;
        }

        // Rescale duration from stream timebase to frame count
        int64_t totalFrames = av_rescale_q(stream->duration, stream->time_base, av_inv_q(frameRate));
        m_duration = static_cast<FrameNumber>(totalFrames);
    } else if (m_formatContext->duration > 0) {
        // Convert container duration to frames using integer arithmetic
        AVRational frameRate;
        if (stream->r_frame_rate.den > 0) {
            frameRate = stream->r_frame_rate;
        } else if (stream->avg_frame_rate.den > 0) {
            frameRate = stream->avg_frame_rate;
        } else {
            frameRate.num = 30;
            frameRate.den = 1;
        }

        // Container duration is in AV_TIME_BASE units
        AVRational containerTimebase = {1, AV_TIME_BASE};
        int64_t totalFrames = av_rescale_q(m_formatContext->duration, containerTimebase, av_inv_q(frameRate));
        m_duration = static_cast<FrameNumber>(totalFrames);
    } else {
        m_duration = 0;
    }

    // 8. Check for alpha channel
    // ProRes 4444 with alpha uses YUVA formats
    AVPixelFormat pixFmt = m_codecContext->pix_fmt;
    m_hasAlpha = (pixFmt == AV_PIX_FMT_YUVA444P ||
                  pixFmt == AV_PIX_FMT_YUVA444P10LE ||
                  pixFmt == AV_PIX_FMT_YUVA444P10BE ||
                  pixFmt == AV_PIX_FMT_YUVA444P12LE ||
                  pixFmt == AV_PIX_FMT_YUVA444P12BE);

    std::cout << "ProResDecoder: Pixel format: " << av_get_pix_fmt_name(pixFmt)
              << " (alpha=" << (m_hasAlpha ? "yes" : "no") << ")" << std::endl;

    // 9. Allocate frame and packet
    m_frame = av_frame_alloc();
    m_packet = av_packet_alloc();
    if (!m_frame || !m_packet) {
        std::cerr << "ProResDecoder: Failed to allocate frame/packet" << std::endl;
        cleanupFFmpeg();
        return Result::DecoderError;
    }

    m_isOpen = true;
    m_currentFrame = -1;

    std::cout << "ProResDecoder: Opened successfully" << std::endl;
    std::cout << "  Resolution: " << m_width << "x" << m_height << std::endl;
    std::cout << "  Frame rate: " << m_frameRate << " fps" << std::endl;
    std::cout << "  Duration: " << m_duration << " frames" << std::endl;

    return Result::Success;
#else
    std::cerr << "ProResDecoder: FFmpeg not available (HAVE_FFMPEG not defined)" << std::endl;
    std::cerr << "  File: " << filepath << std::endl;
    return Result::NotImplemented;
#endif
}

void ProResDecoder::close() {
    if (!m_isOpen) {
        return;
    }

    cleanupFFmpeg();

    // Reset state
    m_isOpen = false;
    m_filepath.clear();
    m_width = 0;
    m_height = 0;
    m_frameRate = 30.0;
    m_duration = 0;
    m_hasAlpha = false;
    m_videoStreamIndex = -1;
    m_currentFrame = -1;
}

Result ProResDecoder::decodeFrame(FrameNumber frameNumber, DecodedFrame& outFrame) {
    if (!m_isOpen) {
        std::cerr << "ProResDecoder: Attempted to decode from closed decoder" << std::endl;
        return Result::Failure;
    }

#ifdef HAVE_FFMPEG
    // Clamp frame number to valid range
    if (frameNumber < 0) frameNumber = 0;
    if (m_duration > 0 && frameNumber >= m_duration) {
        frameNumber = m_duration - 1;
    }

    // If not sequential, seek to the target frame
    if (frameNumber != m_currentFrame + 1) {
        Result seekResult = seek(frameNumber);
        if (seekResult != Result::Success) {
            return seekResult;
        }
    }

    // Decode frames until we reach the target frame
    while (m_currentFrame < frameNumber) {
        Result decodeResult = decodeNextPacket();
        if (decodeResult != Result::Success) {
            return decodeResult;
        }
    }

    // Convert decoded frame to RGBA
    return convertToRGBA(m_frame, outFrame);
#else
    (void)frameNumber;
    (void)outFrame;
    std::cerr << "ProResDecoder: decodeFrame - FFmpeg not available" << std::endl;
    return Result::NotImplemented;
#endif
}

Result ProResDecoder::seek(FrameNumber frameNumber) {
    if (!m_isOpen) {
        std::cerr << "ProResDecoder: Attempted to seek in closed decoder" << std::endl;
        return Result::Failure;
    }

    // Validate frame number
    if (frameNumber < 0) {
        std::cerr << "ProResDecoder: Seek frame number cannot be negative: " << frameNumber << std::endl;
        return Result::InvalidParameter;
    }
    if (m_duration > 0 && frameNumber >= m_duration) {
        std::cerr << "ProResDecoder: Seek frame " << frameNumber << " exceeds duration " << m_duration << std::endl;
        return Result::InvalidParameter;
    }

#ifdef HAVE_FFMPEG
    AVStream* stream = m_formatContext->streams[m_videoStreamIndex];

    // Calculate timestamp in stream timebase
    // We seek to slightly before the target frame to ensure we can decode it
    AVRational frameRateRational;
    frameRateRational.num = 1;
    frameRateRational.den = static_cast<int>(m_frameRate + 0.5);
    int64_t timestamp = av_rescale_q(frameNumber, frameRateRational, stream->time_base);

    // Seek backwards to nearest keyframe
    int ret = av_seek_frame(m_formatContext, m_videoStreamIndex, timestamp, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        // Try seeking without stream index (uses container-level seeking)
        AVRational timeBaseQ;
        timeBaseQ.num = 1;
        timeBaseQ.den = AV_TIME_BASE;
        int64_t containerTs = av_rescale_q(timestamp, stream->time_base, timeBaseQ);
        ret = av_seek_frame(m_formatContext, -1, containerTs, AVSEEK_FLAG_BACKWARD);
        if (ret < 0) {
            char errBuf[256];
            av_strerror(ret, errBuf, sizeof(errBuf));
            std::cerr << "ProResDecoder: Seek failed: " << errBuf << std::endl;
            return Result::DecoderError;
        }
    }

    // Flush decoder to clear internal state
    avcodec_flush_buffers(m_codecContext);

    // Reset current frame to before target (will decode forward to reach it)
    m_currentFrame = frameNumber - 1;

    return Result::Success;
#else
    (void)frameNumber;
    std::cerr << "ProResDecoder: seek - FFmpeg not available" << std::endl;
    return Result::NotImplemented;
#endif
}

Result ProResDecoder::convertToRGBA(AVFrame* srcFrame, DecodedFrame& outFrame) {
#ifdef HAVE_FFMPEG
    if (!srcFrame) {
        return Result::InvalidParameter;
    }

    // Allocate output frame if needed
    if (outFrame.width != m_width || outFrame.height != m_height) {
        outFrame.allocate(m_width, m_height);
    }

    // Create SwsContext if not already created
    if (!m_swsContext) {
        m_swsContext = sws_getContext(
            static_cast<int>(m_width), static_cast<int>(m_height),
            m_codecContext->pix_fmt,
            static_cast<int>(m_width), static_cast<int>(m_height),
            AV_PIX_FMT_RGBA,
            SWS_BILINEAR,  // Fast scaling algorithm
            nullptr, nullptr, nullptr
        );

        if (!m_swsContext) {
            std::cerr << "ProResDecoder: Failed to create SwsContext" << std::endl;
            return Result::DecoderError;
        }
    }

    // Validate frame dimensions match decoder state to prevent buffer overflow
    if (srcFrame->width != static_cast<int>(m_width) ||
        srcFrame->height != static_cast<int>(m_height)) {
        std::cerr << "ProResDecoder: Frame dimensions mismatch: expected "
                  << m_width << "x" << m_height
                  << ", got " << srcFrame->width << "x" << srcFrame->height << std::endl;
        return Result::DecoderError;
    }

    // Validate output buffer size to prevent overflow
    size_t expectedSize = static_cast<size_t>(m_width) * m_height * 4;
    if (outFrame.data.size() < expectedSize) {
        std::cerr << "ProResDecoder: Output buffer too small: " << outFrame.data.size()
                  << " bytes, need " << expectedSize << std::endl;
        return Result::DecoderError;
    }

    // Setup output buffer for swscale
    uint8_t* outData[1] = {outFrame.data.data()};
    int outLinesize[1] = {static_cast<int>(m_width * 4)};

    // Perform color space conversion
    int height = sws_scale(
        m_swsContext,
        srcFrame->data, srcFrame->linesize,
        0, static_cast<int>(m_height),
        outData, outLinesize
    );

    if (height <= 0) {
        std::cerr << "ProResDecoder: sws_scale failed" << std::endl;
        return Result::DecoderError;
    }

    // Premultiply alpha if present
    if (m_hasAlpha) {
        uint8_t* rgba = outFrame.data.data();
        size_t pixelCount = static_cast<size_t>(m_width) * m_height;
        for (size_t i = 0; i < pixelCount; ++i) {
            size_t offset = i * 4;
            uint8_t a = rgba[offset + 3];
            if (a < 255) {
                // Premultiply with rounding to nearest
                rgba[offset + 0] = static_cast<uint8_t>((static_cast<uint32_t>(rgba[offset + 0]) * a + 127) / 255);
                rgba[offset + 1] = static_cast<uint8_t>((static_cast<uint32_t>(rgba[offset + 1]) * a + 127) / 255);
                rgba[offset + 2] = static_cast<uint8_t>((static_cast<uint32_t>(rgba[offset + 2]) * a + 127) / 255);
            }
        }
    }

    // Fill frame metadata
    outFrame.frameNumber = m_currentFrame;
    outFrame.pts = srcFrame->pts;
    outFrame.valid.store(true, std::memory_order_release);

    return Result::Success;
#else
    (void)srcFrame;
    (void)outFrame;
    std::cerr << "ProResDecoder: convertToRGBA - FFmpeg not available" << std::endl;
    return Result::NotImplemented;
#endif
}

Result ProResDecoder::decodeNextPacket() {
#ifdef HAVE_FFMPEG
    int ret;

    // Try to receive a frame first (there might be buffered frames)
    ret = avcodec_receive_frame(m_codecContext, m_frame);
    if (ret == 0) {
        // Got a frame from buffer
        m_currentFrame++;
        return Result::Success;
    }

    // Need more data, read packets
    while (true) {
        ret = av_read_frame(m_formatContext, m_packet);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                // End of file, flush decoder
                ret = avcodec_send_packet(m_codecContext, nullptr);
                if (ret < 0) {
                    return Result::DecoderError;
                }

                // Try to receive remaining frames
                ret = avcodec_receive_frame(m_codecContext, m_frame);
                if (ret == 0) {
                    m_currentFrame++;
                    return Result::Success;
                }
                return Result::DecoderError; // No more frames
            }
            char errBuf[256];
            av_strerror(ret, errBuf, sizeof(errBuf));
            std::cerr << "ProResDecoder: av_read_frame failed: " << errBuf << std::endl;
            return Result::DecoderError;
        }

        // Skip non-video packets
        if (m_packet->stream_index != m_videoStreamIndex) {
            av_packet_unref(m_packet);
            continue;
        }

        // Send packet to decoder
        ret = avcodec_send_packet(m_codecContext, m_packet);
        av_packet_unref(m_packet);

        if (ret < 0) {
            char errBuf[256];
            av_strerror(ret, errBuf, sizeof(errBuf));
            std::cerr << "ProResDecoder: avcodec_send_packet failed: " << errBuf << std::endl;
            return Result::DecoderError;
        }

        // Try to receive decoded frame
        ret = avcodec_receive_frame(m_codecContext, m_frame);
        if (ret == 0) {
            // Got a decoded frame
            m_currentFrame++;
            return Result::Success;
        } else if (ret == AVERROR(EAGAIN)) {
            // Need more packets
            continue;
        } else {
            char errBuf[256];
            av_strerror(ret, errBuf, sizeof(errBuf));
            std::cerr << "ProResDecoder: avcodec_receive_frame failed: " << errBuf << std::endl;
            return Result::DecoderError;
        }
    }
#else
    std::cerr << "ProResDecoder: decodeNextPacket - FFmpeg not available" << std::endl;
    return Result::NotImplemented;
#endif
}

void ProResDecoder::cleanupFFmpeg() {
#ifdef HAVE_FFMPEG
    // Cleanup in reverse order of allocation
    if (m_swsContext) {
        sws_freeContext(m_swsContext);
        m_swsContext = nullptr;
    }

    if (m_packet) {
        av_packet_free(&m_packet);
        m_packet = nullptr;
    }

    if (m_frame) {
        av_frame_free(&m_frame);
        m_frame = nullptr;
    }

    if (m_codecContext) {
        avcodec_free_context(&m_codecContext);
        m_codecContext = nullptr;
    }

    if (m_formatContext) {
        avformat_close_input(&m_formatContext);
        m_formatContext = nullptr;
    }

    m_videoStreamIndex = -1;
#endif
}

} // namespace entity
