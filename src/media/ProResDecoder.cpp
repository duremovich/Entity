#include "entity/media/ProResDecoder.hpp"
#include <iostream>
#include <cstring>

// TODO: Add FFmpeg includes when available
// When HAVE_FFMPEG is defined by CMake, include FFmpeg headers
// #ifdef HAVE_FFMPEG
// extern "C" {
// #include <libavformat/avformat.h>
// #include <libavcodec/avcodec.h>
// #include <libswscale/swscale.h>
// #include <libavutil/imgutils.h>
// #include <libavutil/frame.h>
// }
// #endif

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
    // TODO: Implement FFmpeg opening sequence:
    // 1. avformat_open_input(&m_formatContext, filepath.c_str(), nullptr, nullptr)
    //    - Check for negative return value (error)
    // 2. avformat_find_stream_info(m_formatContext, nullptr)
    //    - Reads stream information from file
    // 3. Loop through streams to find video stream
    //    for (unsigned int i = 0; i < m_formatContext->nb_streams; ++i)
    //        if (m_formatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
    // 4. Get codec for stream:
    //    const AVCodec* codec = avcodec_find_decoder(codecpar->codec_id)
    // 5. Create codec context:
    //    m_codecContext = avcodec_alloc_context3(codec)
    //    avcodec_parameters_to_context(m_codecContext, codecpar)
    // 6. Open codec:
    //    avcodec_open2(m_codecContext, codec, nullptr)
    // 7. Extract media properties:
    //    m_width = m_codecContext->width
    //    m_height = m_codecContext->height
    //    m_frameRate = av_q2d(stream->r_frame_rate)
    //    m_duration = stream->nb_frames (or calculate from duration)
    // 8. Check for alpha channel (ProRes 4444 specific):
    //    m_hasAlpha = (m_codecContext->pix_fmt == AV_PIX_FMT_YUV422P10LE)
    //    // ProRes 4444 is YUV422P10LE with alpha in 4th plane
    // 9. Allocate frame and packet:
    //    m_frame = av_frame_alloc()
    //    m_packet = av_packet_alloc()
    // 10. Set m_isOpen = true on success

    std::cerr << "ProResDecoder: FFmpeg integration not yet implemented (HAVE_FFMPEG)" << std::endl;
    std::cerr << "  File: " << filepath << std::endl;
    return Result::NotImplemented;
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
    // TODO: Implement frame decoding:
    // 1. If frameNumber != m_currentFrame + 1, seek to frameNumber
    //    Result seekResult = seek(frameNumber)
    //    if (seekResult != Result::Success) return seekResult
    // 2. Decode frames until target frame number is found:
    //    while (m_currentFrame < frameNumber) {
    //        Result decodeResult = decodeNextPacket()
    //        if (decodeResult != Result::Success) return decodeResult
    //    }
    // 3. Convert decoded frame to RGBA:
    //    return convertToRGBA(m_frame, outFrame)

    (void)frameNumber;
    (void)outFrame;
    std::cerr << "ProResDecoder: decodeFrame not implemented (HAVE_FFMPEG)" << std::endl;
    return Result::NotImplemented;
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

#ifdef HAVE_FFMPEG
    // TODO: Implement seeking:
    // 1. Calculate timestamp in AVStream timebase:
    //    AVStream* stream = m_formatContext->streams[m_videoStreamIndex]
    //    int64_t timestamp = av_rescale_q(frameNumber, {1, static_cast<int>(m_frameRate)}, stream->time_base)
    // 2. Seek to timestamp:
    //    int ret = av_seek_frame(m_formatContext, m_videoStreamIndex, timestamp, AVSEEK_FLAG_BACKWARD)
    //    if (ret < 0) return Result::DecoderError
    // 3. Flush decoder to clear internal state:
    //    avcodec_flush_buffers(m_codecContext)
    // 4. Update current frame:
    //    m_currentFrame = frameNumber - 1  (will be incremented on next decode)

    (void)frameNumber;
    std::cerr << "ProResDecoder: seek not implemented (HAVE_FFMPEG)" << std::endl;
    return Result::NotImplemented;
#else
    (void)frameNumber;
    std::cerr << "ProResDecoder: seek - FFmpeg not available" << std::endl;
    return Result::NotImplemented;
#endif
}

Result ProResDecoder::convertToRGBA(AVFrame* srcFrame, DecodedFrame& outFrame) {
#ifdef HAVE_FFMPEG
    // TODO: Implement pixel format conversion:
    // 1. Allocate output frame if needed:
    //    if (!outFrame.data.empty() && outFrame.width != m_width) outFrame.clear()
    //    if (outFrame.data.empty()) outFrame.allocate(m_width, m_height)
    // 2. Create/initialize SwsContext for YUV422P10LE -> RGBA32 conversion:
    //    if (!m_swsContext) {
    //        m_swsContext = sws_getContext(
    //            m_width, m_height, m_codecContext->pix_fmt,
    //            m_width, m_height, AV_PIX_FMT_RGBA,
    //            SWS_BICUBIC, nullptr, nullptr, nullptr
    //        )
    //    }
    // 3. Setup output buffer for swscale:
    //    uint8_t* outData[1] = {outFrame.data.data()}
    //    int outLinesize[1] = {static_cast<int>(m_width * 4)}
    // 4. Perform conversion:
    //    int height = sws_scale(m_swsContext, srcFrame->data, srcFrame->linesize, 0,
    //                           m_height, outData, outLinesize)
    //    if (height <= 0) return Result::DecoderError
    // 5. Premultiply alpha (if hasAlpha):
    //    if (m_hasAlpha) {
    //        uint8_t* rgba = outFrame.data.data()
    //        for (size_t i = 0; i < outFrame.data.size(); i += 4) {
    //            float alpha = rgba[i+3] / 255.0f
    //            rgba[i+0] = static_cast<uint8_t>(rgba[i+0] * alpha)  // R
    //            rgba[i+1] = static_cast<uint8_t>(rgba[i+1] * alpha)  // G
    //            rgba[i+2] = static_cast<uint8_t>(rgba[i+2] * alpha)  // B
    //            // A unchanged
    //        }
    //    }
    // 6. Fill frame metadata:
    //    outFrame.frameNumber = m_currentFrame
    //    outFrame.pts = srcFrame->pts
    //    outFrame.valid = true
    // 7. Return success

    (void)srcFrame;
    (void)outFrame;
    std::cerr << "ProResDecoder: convertToRGBA not implemented (HAVE_FFMPEG)" << std::endl;
    return Result::NotImplemented;
#else
    (void)srcFrame;
    (void)outFrame;
    std::cerr << "ProResDecoder: convertToRGBA - FFmpeg not available" << std::endl;
    return Result::NotImplemented;
#endif
}

Result ProResDecoder::decodeNextPacket() {
#ifdef HAVE_FFMPEG
    // TODO: Implement packet decoding:
    // 1. Read next packet from file:
    //    while (av_read_frame(m_formatContext, m_packet) >= 0) {
    //        if (m_packet->stream_index != m_videoStreamIndex) {
    //            av_packet_unref(m_packet)
    //            continue  // Skip non-video packets
    //        }
    //        break
    //    }
    //    Returns negative on EOF or error
    // 2. Send packet to decoder:
    //    int ret = avcodec_send_packet(m_codecContext, m_packet)
    //    av_packet_unref(m_packet)
    //    if (ret < 0) return Result::DecoderError
    // 3. Receive decoded frame:
    //    ret = avcodec_receive_frame(m_codecContext, m_frame)
    //    if (ret == AVERROR(EAGAIN)) return Result::DecoderError  // Need more packets
    //    if (ret < 0) return Result::DecoderError  // EOF or error
    // 4. Update frame counter:
    //    m_currentFrame++
    // 5. Return success

    std::cerr << "ProResDecoder: decodeNextPacket not implemented (HAVE_FFMPEG)" << std::endl;
    return Result::NotImplemented;
#else
    std::cerr << "ProResDecoder: decodeNextPacket - FFmpeg not available" << std::endl;
    return Result::NotImplemented;
#endif
}

void ProResDecoder::cleanupFFmpeg() {
#ifdef HAVE_FFMPEG
    // TODO: Cleanup FFmpeg resources in reverse order of allocation:
    // 1. if (m_swsContext) { sws_freeContext(m_swsContext); m_swsContext = nullptr; }
    // 2. if (m_packet) { av_packet_free(&m_packet); m_packet = nullptr; }
    // 3. if (m_frame) { av_frame_free(&m_frame); m_frame = nullptr; }
    // 4. if (m_codecContext) { avcodec_free_context(&m_codecContext); m_codecContext = nullptr; }
    // 5. if (m_formatContext) { avformat_close_input(&m_formatContext); m_formatContext = nullptr; }
    // 6. m_videoStreamIndex = -1

    // Placeholder: Print cleanup notice when FFmpeg is integrated
    if (m_isOpen) {
        std::cerr << "ProResDecoder: cleanupFFmpeg - cleanup code not yet implemented (HAVE_FFMPEG)" << std::endl;
    }
#endif
}

} // namespace entity
