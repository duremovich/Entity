#include "entity/audio/AudioDecoder.hpp"

#ifdef HAVE_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
}
#endif

#include <iostream>
#include <cstring>

namespace entity {

AudioDecoder::~AudioDecoder() {
#ifdef HAVE_FFMPEG
    swr_free(&m_swr);
    if (m_frame)  av_frame_free(&m_frame);
    if (m_packet) av_packet_free(&m_packet);
    if (m_codecCtx) avcodec_free_context(&m_codecCtx);
    if (m_fmtCtx)   avformat_close_input(&m_fmtCtx);
#endif
}

bool AudioDecoder::open(const std::string& filepath, int targetSampleRate) {
#ifndef HAVE_FFMPEG
    (void)filepath; (void)targetSampleRate;
    return false;
#else
    m_targetSampleRate = targetSampleRate;

    // 1. Open container.
    int ret = avformat_open_input(&m_fmtCtx, filepath.c_str(), nullptr, nullptr);
    if (ret < 0) {
        char errBuf[256];
        av_strerror(ret, errBuf, sizeof(errBuf));
        std::cerr << "[AudioDecoder] avformat_open_input: " << errBuf << std::endl;
        return false;
    }

    ret = avformat_find_stream_info(m_fmtCtx, nullptr);
    if (ret < 0) {
        char errBuf[256];
        av_strerror(ret, errBuf, sizeof(errBuf));
        std::cerr << "[AudioDecoder] avformat_find_stream_info: " << errBuf << std::endl;
        avformat_close_input(&m_fmtCtx);
        return false;
    }

    // 2. Find best audio stream.
    const AVCodec* decoder = nullptr;
    ret = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (ret < 0 || !decoder) {
        std::cerr << "[AudioDecoder] No audio stream in: " << filepath << std::endl;
        avformat_close_input(&m_fmtCtx);
        return false;
    }
    m_streamIndex = ret;
    AVStream* stream = m_fmtCtx->streams[m_streamIndex];

    // 3. Open codec.
    m_codecCtx = avcodec_alloc_context3(decoder);
    if (!m_codecCtx) {
        avformat_close_input(&m_fmtCtx);
        return false;
    }
    ret = avcodec_parameters_to_context(m_codecCtx, stream->codecpar);
    if (ret < 0) {
        avcodec_free_context(&m_codecCtx);
        avformat_close_input(&m_fmtCtx);
        return false;
    }
    ret = avcodec_open2(m_codecCtx, decoder, nullptr);
    if (ret < 0) {
        char errBuf[256];
        av_strerror(ret, errBuf, sizeof(errBuf));
        std::cerr << "[AudioDecoder] avcodec_open2: " << errBuf << std::endl;
        avcodec_free_context(&m_codecCtx);
        avformat_close_input(&m_fmtCtx);
        return false;
    }

    // 4. Configure swresample: input from codec, output stereo float32 at target rate.
    AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
    ret = swr_alloc_set_opts2(&m_swr,
        &outLayout,             AV_SAMPLE_FMT_FLT,  targetSampleRate,
        &m_codecCtx->ch_layout, m_codecCtx->sample_fmt, m_codecCtx->sample_rate,
        0, nullptr);
    if (ret < 0 || !m_swr) {
        char errBuf[256];
        av_strerror(ret, errBuf, sizeof(errBuf));
        std::cerr << "[AudioDecoder] swr_alloc_set_opts2: " << errBuf << std::endl;
        avcodec_free_context(&m_codecCtx);
        avformat_close_input(&m_fmtCtx);
        return false;
    }
    ret = swr_init(m_swr);
    if (ret < 0) {
        char errBuf[256];
        av_strerror(ret, errBuf, sizeof(errBuf));
        std::cerr << "[AudioDecoder] swr_init: " << errBuf << std::endl;
        swr_free(&m_swr);
        avcodec_free_context(&m_codecCtx);
        avformat_close_input(&m_fmtCtx);
        return false;
    }

    // 5. Record source metadata.
    m_sourceSampleRate = m_codecCtx->sample_rate;
    m_sourceChannels   = m_codecCtx->ch_layout.nb_channels;
    if (stream->duration != AV_NOPTS_VALUE) {
        // Convert stream duration (in stream timebase) to source samples.
        m_sourceLengthSamples = av_rescale_q(stream->duration,
            stream->time_base, AVRational{1, m_sourceSampleRate});
    } else {
        m_sourceLengthSamples = 0;
    }

    // 6. Allocate reusable packet + frame.
    m_packet = av_packet_alloc();
    m_frame  = av_frame_alloc();
    if (!m_packet || !m_frame) {
        if (m_frame)  av_frame_free(&m_frame);
        if (m_packet) av_packet_free(&m_packet);
        swr_free(&m_swr);
        avcodec_free_context(&m_codecCtx);
        avformat_close_input(&m_fmtCtx);
        return false;
    }

    return true;
#endif
}

void AudioDecoder::seekToOutputSample(int64_t outSamplePos) {
#ifdef HAVE_FFMPEG
    if (!m_fmtCtx || m_streamIndex < 0) return;
    AVStream* stream = m_fmtCtx->streams[m_streamIndex];
    int64_t ts = av_rescale_q(outSamplePos,
        AVRational{1, m_targetSampleRate},
        stream->time_base);
    av_seek_frame(m_fmtCtx, m_streamIndex, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(m_codecCtx);
#else
    (void)outSamplePos;
#endif
}

size_t AudioDecoder::decodeChunk(std::vector<float>& out, size_t maxFrames) {
#ifndef HAVE_FFMPEG
    (void)out; (void)maxFrames;
    return 0;
#else
    if (!m_fmtCtx || !m_codecCtx || !m_swr) return 0;

    size_t framesAppended = 0;
    // Temporary buffer for swr_convert output (stereo float32).
    std::vector<float> swrBuf;

    while (framesAppended < maxFrames) {
        // Drain the decoder first before reading more packets.
        int ret = avcodec_receive_frame(m_codecCtx, m_frame);
        if (ret == AVERROR(EAGAIN)) {
            // Need more input — read the next audio packet.
            bool gotPacket = false;
            while (av_read_frame(m_fmtCtx, m_packet) >= 0) {
                if (m_packet->stream_index == m_streamIndex) {
                    ret = avcodec_send_packet(m_codecCtx, m_packet);
                    av_packet_unref(m_packet);
                    if (ret < 0 && ret != AVERROR(EAGAIN)) return framesAppended;
                    gotPacket = true;
                    break;
                }
                av_packet_unref(m_packet);
            }
            if (!gotPacket) {
                // EOF — flush.
                avcodec_send_packet(m_codecCtx, nullptr);
                ret = avcodec_receive_frame(m_codecCtx, m_frame);
                if (ret < 0) return framesAppended;
            } else {
                ret = avcodec_receive_frame(m_codecCtx, m_frame);
                if (ret == AVERROR(EAGAIN) || ret < 0) continue;
            }
        } else if (ret < 0) {
            return framesAppended; // EOF or error
        }

        // Convert this frame via swresample.
        const int inSamples = m_frame->nb_samples;
        // Max possible output samples (with SRC the output count can differ).
        const int maxOut = static_cast<int>(
            av_rescale_rnd(swr_get_delay(m_swr, m_sourceSampleRate) + inSamples,
                           m_targetSampleRate, m_sourceSampleRate, AV_ROUND_UP));
        swrBuf.resize(maxOut * 2); // stereo
        uint8_t* outPtr = reinterpret_cast<uint8_t*>(swrBuf.data());
        const int converted = swr_convert(m_swr,
            &outPtr, maxOut,
            const_cast<const uint8_t**>(m_frame->data), inSamples);
        av_frame_unref(m_frame);
        if (converted <= 0) continue;

        // Append ALL converted frames — never cap at maxFrames. swr_convert
        // emits a variable frame count per decoded packet (notably when the
        // source rate != target rate). Discarding the overshoot drops audio
        // at every chunk boundary: an audible click (~12/sec) AND a
        // cumulative skip that races the audio ahead of the video.
        // maxFrames is only a soft target for how much to decode per call;
        // `out` is allowed to end up slightly larger and the caller writes
        // all of it to the ring.
        const size_t convFrames = static_cast<size_t>(converted);
        const size_t prevSize = out.size();
        out.resize(prevSize + convFrames * 2);
        std::memcpy(&out[prevSize], swrBuf.data(),
                    convFrames * 2 * sizeof(float));
        framesAppended += convFrames;
    }

    return framesAppended;
#endif
}

} // namespace entity
