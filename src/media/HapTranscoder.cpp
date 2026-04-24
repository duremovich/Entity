#include "entity/media/HapTranscoder.hpp"

#include <iostream>
#include <cstring>

#ifdef HAVE_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
}
#endif

namespace entity {

#ifdef HAVE_FFMPEG

namespace {

// RAII guards — libavformat/codec has a lot of resources that all need
// cleanup on every early-return path; lambdas + unique_ptr deleters get
// noisy fast. Just use explicit small classes.
struct FormatCloser {
    AVFormatContext* ctx{nullptr};
    ~FormatCloser() { if (ctx) avformat_close_input(&ctx); }
};
struct MuxerCloser {
    AVFormatContext* ctx{nullptr};
    ~MuxerCloser() {
        if (ctx) {
            if (ctx->pb) avio_closep(&ctx->pb);
            avformat_free_context(ctx);
        }
    }
};
struct CodecCtxFree {
    AVCodecContext* ctx{nullptr};
    ~CodecCtxFree() { if (ctx) avcodec_free_context(&ctx); }
};
struct FrameFree {
    AVFrame* f{nullptr};
    ~FrameFree() { if (f) av_frame_free(&f); }
};
struct PacketFree {
    AVPacket* p{nullptr};
    ~PacketFree() { if (p) av_packet_free(&p); }
};
struct SwsFree {
    SwsContext* s{nullptr};
    ~SwsFree() { if (s) sws_freeContext(s); }
};

void logAvError(const char* context, int ret) {
    char buf[256];
    av_strerror(ret, buf, sizeof(buf));
    std::cerr << "HapTranscoder: " << context << " failed: " << buf << std::endl;
}

} // anonymous namespace

Result transcodeToHap(const std::string& srcPath,
                      const std::string& dstPath,
                      const std::string& variant,
                      TranscodeProgress progress,
                      double srcFps) {
    // ---------- 1. Open source container + video stream -------------------
    // For image-sequence inputs (image2 demuxer via "%d" in the path), the
    // framerate/size/pixel_format defaults aren't enough to establish stream
    // parameters — pass hints. For normal video containers these are all
    // ignored by the demuxer.
    AVDictionary* inOpts = nullptr;
    if (srcFps > 0.0) {
        char fpsStr[64];
        std::snprintf(fpsStr, sizeof(fpsStr), "%.6f", srcFps);
        av_dict_set(&inOpts, "framerate", fpsStr, 0);
    }
    const bool isImageSequence = (srcPath.find('%') != std::string::npos);
    if (isImageSequence) {
        // PNG sequences: tell image2 the format/size so it doesn't need to
        // probe. Values are probe hints — the actual PNG decoder reads the
        // real size from each frame's IHDR, so a safe upper bound works.
        av_dict_set(&inOpts, "pixel_format", "rgba", 0);
        av_dict_set(&inOpts, "video_size", "8192x8192", 0);
        av_dict_set(&inOpts, "probesize", "100000000", 0);
        av_dict_set(&inOpts, "analyzeduration", "10000000", 0);
    }
    FormatCloser in;
    int openRet = avformat_open_input(&in.ctx, srcPath.c_str(), nullptr, &inOpts);
    av_dict_free(&inOpts);
    if (openRet < 0) {
        logAvError("avformat_open_input", openRet);
        return Result::FileNotFound;
    }
    if (int ret = avformat_find_stream_info(in.ctx, nullptr); ret < 0) {
        logAvError("avformat_find_stream_info", ret);
        return Result::DecoderError;
    }

    int videoStreamIndex = -1;
    for (unsigned i = 0; i < in.ctx->nb_streams; ++i) {
        if (in.ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoStreamIndex = static_cast<int>(i);
            break;
        }
    }
    if (videoStreamIndex < 0) {
        std::cerr << "HapTranscoder: no video stream in " << srcPath << std::endl;
        return Result::UnsupportedFormat;
    }
    AVStream* inStream = in.ctx->streams[videoStreamIndex];
    AVCodecParameters* inPar = inStream->codecpar;

    const AVCodec* inDec = avcodec_find_decoder(inPar->codec_id);
    if (!inDec) {
        std::cerr << "HapTranscoder: no decoder for source codec_id " << inPar->codec_id << std::endl;
        return Result::UnsupportedFormat;
    }

    CodecCtxFree inCtx;
    inCtx.ctx = avcodec_alloc_context3(inDec);
    if (!inCtx.ctx) return Result::OutOfMemory;
    if (int ret = avcodec_parameters_to_context(inCtx.ctx, inPar); ret < 0) {
        logAvError("avcodec_parameters_to_context (input)", ret);
        return Result::DecoderError;
    }
    inCtx.ctx->thread_count = 0;
    inCtx.ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    if (int ret = avcodec_open2(inCtx.ctx, inDec, nullptr); ret < 0) {
        logAvError("avcodec_open2 (input)", ret);
        return Result::DecoderError;
    }

    const int width  = inCtx.ctx->width;
    const int height = inCtx.ctx->height;

    // HAP stores BC1/BC3/BC7 blocks — the FFmpeg encoder rejects dims that
    // aren't multiples of 4 with "Invalid data found when processing input".
    // Round up and let sws_scale resize the content to fit. Aspect-ratio
    // drift is at most 3/W (worst case 0.07% on 4K-class input) —
    // imperceptible. Rounding up (vs down) preserves every source pixel.
    const int encW = (width  + 3) & ~3;
    const int encH = (height + 3) & ~3;
    if (encW != width || encH != height) {
        std::cout << "HapTranscoder: source " << width << "x" << height
                  << " not 4-aligned; encoding at " << encW << "x" << encH
                  << " (HAP requires 4-aligned dimensions)" << std::endl;
    }

    // ---------- 2. Set up HAP encoder -------------------------------------
    const AVCodec* hapEnc = avcodec_find_encoder(AV_CODEC_ID_HAP);
    if (!hapEnc) {
        std::cerr << "HapTranscoder: HAP encoder not registered in FFmpeg build "
                  << "(need ffmpeg vcpkg 'snappy' feature enabled)" << std::endl;
        return Result::NotImplemented;
    }

    CodecCtxFree encCtx;
    encCtx.ctx = avcodec_alloc_context3(hapEnc);
    if (!encCtx.ctx) return Result::OutOfMemory;
    encCtx.ctx->width     = encW;
    encCtx.ctx->height    = encH;
    encCtx.ctx->pix_fmt   = AV_PIX_FMT_RGBA; // HAP encoder input format
    encCtx.ctx->time_base = inStream->time_base;
    encCtx.ctx->framerate = (inStream->r_frame_rate.den > 0) ? inStream->r_frame_rate
                                                              : inStream->avg_frame_rate;
    // Select the HAP variant ("hap" / "hap_alpha" / "hap_q") via the encoder's "format" option.
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "format", variant.c_str(), 0);
    if (int ret = avcodec_open2(encCtx.ctx, hapEnc, &opts); ret < 0) {
        av_dict_free(&opts);
        logAvError("avcodec_open2 (HAP encoder)", ret);
        return Result::DecoderError;
    }
    av_dict_free(&opts);

    // ---------- 3. Set up output muxer ------------------------------------
    MuxerCloser out;
    if (int ret = avformat_alloc_output_context2(&out.ctx, nullptr, "mov", dstPath.c_str());
        ret < 0 || !out.ctx) {
        logAvError("avformat_alloc_output_context2", ret);
        return Result::Failure;
    }
    AVStream* outStream = avformat_new_stream(out.ctx, hapEnc);
    if (!outStream) return Result::OutOfMemory;
    if (int ret = avcodec_parameters_from_context(outStream->codecpar, encCtx.ctx); ret < 0) {
        logAvError("avcodec_parameters_from_context (output)", ret);
        return Result::Failure;
    }
    outStream->time_base = encCtx.ctx->time_base;

    if (!(out.ctx->oformat->flags & AVFMT_NOFILE)) {
        if (int ret = avio_open(&out.ctx->pb, dstPath.c_str(), AVIO_FLAG_WRITE); ret < 0) {
            logAvError("avio_open (output)", ret);
            return Result::Failure;
        }
    }
    if (int ret = avformat_write_header(out.ctx, nullptr); ret < 0) {
        logAvError("avformat_write_header", ret);
        return Result::Failure;
    }

    // ---------- 4. Pixel-format conversion context (src → RGBA) -----------
    // Source is WxH (original), dest is encWxencH (4-aligned). sws_scale
    // handles the resize in the same pass as the YUV→RGBA conversion.
    SwsFree sws;
    sws.s = sws_getContext(width, height, inCtx.ctx->pix_fmt,
                            encW, encH, AV_PIX_FMT_RGBA,
                            SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws.s) {
        std::cerr << "HapTranscoder: sws_getContext failed" << std::endl;
        return Result::DecoderError;
    }

    FrameFree srcFrame;  srcFrame.f  = av_frame_alloc();
    FrameFree rgbaFrame; rgbaFrame.f = av_frame_alloc();
    if (!srcFrame.f || !rgbaFrame.f) return Result::OutOfMemory;

    rgbaFrame.f->format = AV_PIX_FMT_RGBA;
    rgbaFrame.f->width  = encW;
    rgbaFrame.f->height = encH;
    if (int ret = av_frame_get_buffer(rgbaFrame.f, 0); ret < 0) {
        logAvError("av_frame_get_buffer (RGBA)", ret);
        return Result::OutOfMemory;
    }

    PacketFree inPkt;  inPkt.p  = av_packet_alloc();
    PacketFree outPkt; outPkt.p = av_packet_alloc();
    if (!inPkt.p || !outPkt.p) return Result::OutOfMemory;

    // ---------- 5. Main transcode loop ------------------------------------
    int64_t framesTotal = (inStream->nb_frames > 0) ? inStream->nb_frames : 0;
    int64_t framesDone  = 0;
    bool aborted = false;

    auto encodeOneFrame = [&](AVFrame* frame) -> Result {
        if (int ret = avcodec_send_frame(encCtx.ctx, frame); ret < 0) {
            if (ret != AVERROR_EOF) {
                logAvError("avcodec_send_frame (HAP)", ret);
                return Result::DecoderError;
            }
        }
        while (true) {
            int ret = avcodec_receive_packet(encCtx.ctx, outPkt.p);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) { logAvError("avcodec_receive_packet", ret); return Result::DecoderError; }

            outPkt.p->stream_index = outStream->index;
            av_packet_rescale_ts(outPkt.p, encCtx.ctx->time_base, outStream->time_base);
            if (int w = av_interleaved_write_frame(out.ctx, outPkt.p); w < 0) {
                logAvError("av_interleaved_write_frame", w);
                av_packet_unref(outPkt.p);
                return Result::Failure;
            }
            av_packet_unref(outPkt.p);
        }
        return Result::Success;
    };

    while (!aborted) {
        av_packet_unref(inPkt.p);
        int r = av_read_frame(in.ctx, inPkt.p);
        if (r == AVERROR_EOF) break;
        if (r < 0) { logAvError("av_read_frame", r); return Result::DecoderError; }
        if (inPkt.p->stream_index != videoStreamIndex) continue;

        if (int ret = avcodec_send_packet(inCtx.ctx, inPkt.p); ret < 0) {
            logAvError("avcodec_send_packet", ret);
            return Result::DecoderError;
        }

        while (true) {
            int ret = avcodec_receive_frame(inCtx.ctx, srcFrame.f);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) { logAvError("avcodec_receive_frame", ret); return Result::DecoderError; }

            sws_scale(sws.s, srcFrame.f->data, srcFrame.f->linesize, 0, height,
                      rgbaFrame.f->data, rgbaFrame.f->linesize);
            rgbaFrame.f->pts = srcFrame.f->best_effort_timestamp != AV_NOPTS_VALUE
                               ? srcFrame.f->best_effort_timestamp
                               : framesDone;

            if (Result er = encodeOneFrame(rgbaFrame.f); er != Result::Success) return er;

            ++framesDone;
            if (progress && !progress(framesDone, framesTotal)) {
                aborted = true;
                break;
            }
            av_frame_unref(srcFrame.f);
        }
    }

    // Flush the source decoder
    avcodec_send_packet(inCtx.ctx, nullptr);
    while (!aborted) {
        int ret = avcodec_receive_frame(inCtx.ctx, srcFrame.f);
        if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN)) break;
        if (ret < 0) { logAvError("flush decode", ret); break; }

        sws_scale(sws.s, srcFrame.f->data, srcFrame.f->linesize, 0, height,
                  rgbaFrame.f->data, rgbaFrame.f->linesize);
        rgbaFrame.f->pts = srcFrame.f->best_effort_timestamp != AV_NOPTS_VALUE
                           ? srcFrame.f->best_effort_timestamp
                           : framesDone;
        if (Result er = encodeOneFrame(rgbaFrame.f); er != Result::Success) return er;
        ++framesDone;
        if (progress && !progress(framesDone, framesTotal)) { aborted = true; break; }
        av_frame_unref(srcFrame.f);
    }

    // Flush the HAP encoder
    if (!aborted) {
        if (Result er = encodeOneFrame(nullptr); er != Result::Success) return er;
    }

    if (int ret = av_write_trailer(out.ctx); ret < 0) {
        logAvError("av_write_trailer", ret);
        return Result::Failure;
    }

    if (aborted) return Result::Failure;

    std::cout << "HapTranscoder: wrote " << framesDone << " frames → " << dstPath
              << " [" << variant << "]" << std::endl;
    return Result::Success;
}

#else  // HAVE_FFMPEG

Result transcodeToHap(const std::string&, const std::string&,
                      const std::string&, TranscodeProgress) {
    std::cerr << "HapTranscoder: FFmpeg not available in this build" << std::endl;
    return Result::NotImplemented;
}

#endif

} // namespace entity
