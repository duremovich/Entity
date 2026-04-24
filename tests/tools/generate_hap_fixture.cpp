/**
 * One-off CLI utility that produces a HAP .mov test fixture. Synthesizes
 * a small procedural gradient sequence in memory and encodes it via
 * libavcodec's HAP encoder (needs ffmpeg built with snappy feature — see
 * the vcpkg.json ffmpeg feature).
 *
 * Usage:
 *   generate_hap_fixture <dst-path> [variant=hap_alpha] [w=64] [h=64] [frames=16] [fps=30]
 *
 * Example:
 *   generate_hap_fixture test_media/hap_gradient/test.mov hap_alpha 64 64 16 30
 */

#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
}

static int die(const char* what, int ret = 0) {
    char buf[256] = {0};
    if (ret < 0) av_strerror(ret, buf, sizeof(buf));
    std::cerr << what << (ret < 0 ? ": " : "") << (ret < 0 ? buf : "") << std::endl;
    return 1;
}

// Fill an RGBA buffer with a deterministic gradient that varies per frame.
// Matches the visual idea of test_media/gradient_seq but doesn't depend on
// the actual PNG content — pure math.
static void fillGradient(uint8_t* rgba, int w, int h, int frameIdx, int totalFrames) {
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int i = (y * w + x) * 4;
            rgba[i + 0] = static_cast<uint8_t>((x * 255) / (w - 1));
            rgba[i + 1] = static_cast<uint8_t>((y * 255) / (h - 1));
            rgba[i + 2] = static_cast<uint8_t>((frameIdx * 255) / (totalFrames - 1));
            rgba[i + 3] = 255;
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0]
                  << " <dst-path> [variant=hap_alpha] [w=64] [h=64] [frames=16] [fps=30]"
                  << std::endl;
        return 2;
    }
    const std::string dst   = argv[1];
    const std::string variant = (argc >= 3) ? argv[2] : "hap_alpha";
    const int width  = (argc >= 4) ? std::atoi(argv[3]) : 64;
    const int height = (argc >= 5) ? std::atoi(argv[4]) : 64;
    const int frames = (argc >= 6) ? std::atoi(argv[5]) : 16;
    const int fps    = (argc >= 7) ? std::atoi(argv[6]) : 30;

    const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_HAP);
    if (!enc) return die("HAP encoder not available (ffmpeg needs snappy feature)");

    AVCodecContext* encCtx = avcodec_alloc_context3(enc);
    if (!encCtx) return die("avcodec_alloc_context3");
    encCtx->width   = width;
    encCtx->height  = height;
    encCtx->pix_fmt = AV_PIX_FMT_RGBA;
    encCtx->time_base  = AVRational{1, fps};
    encCtx->framerate  = AVRational{fps, 1};
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "format", variant.c_str(), 0);
    int ret = avcodec_open2(encCtx, enc, &opts);
    av_dict_free(&opts);
    if (ret < 0) return die("avcodec_open2", ret);

    AVFormatContext* out = nullptr;
    ret = avformat_alloc_output_context2(&out, nullptr, "mov", dst.c_str());
    if (ret < 0 || !out) return die("avformat_alloc_output_context2", ret);
    AVStream* stream = avformat_new_stream(out, enc);
    if (!stream) return die("avformat_new_stream");
    stream->time_base = encCtx->time_base;
    ret = avcodec_parameters_from_context(stream->codecpar, encCtx);
    if (ret < 0) return die("avcodec_parameters_from_context", ret);

    if (!(out->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&out->pb, dst.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) return die("avio_open", ret);
    }
    ret = avformat_write_header(out, nullptr);
    if (ret < 0) return die("avformat_write_header", ret);

    AVFrame* frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_RGBA;
    frame->width  = width;
    frame->height = height;
    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) return die("av_frame_get_buffer", ret);

    AVPacket* pkt = av_packet_alloc();

    auto drain = [&](AVFrame* f) -> int {
        int r = avcodec_send_frame(encCtx, f);
        if (r < 0 && r != AVERROR_EOF) return die("avcodec_send_frame", r);
        while (true) {
            r = avcodec_receive_packet(encCtx, pkt);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return 0;
            if (r < 0) return die("avcodec_receive_packet", r);
            pkt->stream_index = stream->index;
            av_packet_rescale_ts(pkt, encCtx->time_base, stream->time_base);
            r = av_interleaved_write_frame(out, pkt);
            av_packet_unref(pkt);
            if (r < 0) return die("av_interleaved_write_frame", r);
        }
        return 0;
    };

    for (int i = 0; i < frames; ++i) {
        av_frame_make_writable(frame);
        fillGradient(frame->data[0], width, height, i, frames);
        frame->pts = i;
        if (int r = drain(frame); r) return r;
    }
    if (int r = drain(nullptr); r) return r; // flush

    ret = av_write_trailer(out);
    if (ret < 0) return die("av_write_trailer", ret);

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&encCtx);
    if (out->pb) avio_closep(&out->pb);
    avformat_free_context(out);

    std::cout << "wrote " << dst << " (" << width << "x" << height << ", "
              << frames << " frames, " << variant << ")" << std::endl;
    return 0;
}
