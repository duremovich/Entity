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
 *
 * The "haph" / "hap_hdr" variant takes a different code path: FFmpeg's HAP
 * encoder doesn't support BC6H output, so we hand-roll a constant-value
 * BC6H_UF16 mode-11 block, tile it across the image, wrap it in a raw HAP
 * 0xA2 (HapHU) section, and write packets directly through libavformat
 * without an attached encoder. The result is a synthetic HDR fixture for
 * Phase C.12 #10's FP16-survival smoke test — not visually meaningful, but
 * stable and parseable end-to-end.
 */

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
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

// =============================================================================
// HapH (BC6H_UF16) synthetic-block path. Hand-rolled because FFmpeg's HAP
// encoder rejects HapH inputs.
// =============================================================================

// HAP raw-section type byte for HapH unsigned BC6H. See HapFormat.cpp.
constexpr uint8_t SEC_HAPHU_RAW = 0xA2;

// Pick the 10-bit unsigned BC6H endpoint that, after the BC6H_UF16 unquantize +
// finishUnquantize pipeline, decodes to the half-float closest to `targetHalf`.
// Per Microsoft's BC6H spec / DirectXTex reference:
//   unq        = ((e << 16) + 0x8000) >> 10   for 1 ≤ e ≤ 1022 (0 → 0, 1023 → 0xFFFF special)
//   finalHalf  = (unq * 31) >> 6              (unsigned variant)
// `finalHalf` is the IEEE 754 binary16 bit pattern.
static int pickBC6HUnsignedEndpoint(uint16_t targetHalf) {
    int bestE = 0;
    int bestErr = std::numeric_limits<int>::max();
    for (int e = 1; e <= 1022; ++e) {
        const int unq = ((e << 16) + 0x8000) >> 10;
        const int finalH = (unq * 31) >> 6;
        const int err = std::abs(finalH - static_cast<int>(targetHalf));
        if (err < bestErr) {
            bestErr = err;
            bestE = e;
        }
    }
    return bestE;
}

// Bit-pack a BC6H mode-11 block (1-region, 10:10:10:10:10:10, no transform) with
// both endpoints set to `e`, all pixel indices zero. Layout (LSB first inside
// the 128-bit value, then little-endian byte order on the wire):
//   bits  [4:0]    = mode = 0b00011 (mode 11 selector)
//   bits [14:5]    = R endpoint 0
//   bits [24:15]   = G endpoint 0
//   bits [34:25]   = B endpoint 0
//   bits [44:35]   = R endpoint 1
//   bits [54:45]   = G endpoint 1
//   bits [64:55]   = B endpoint 1
//   bits [127:65]  = 63 bits of pixel indices (all zero — both endpoints equal,
//                    so weights don't matter; result is identical for any index)
static std::array<uint8_t, 16> encodeBC6HMode11ConstantBlock(int e) {
    uint64_t lo = 0;
    uint64_t hi = 0;
    auto setBits = [&](int startBit, int count, uint64_t value) {
        const uint64_t mask = (count == 64) ? ~0ULL : ((1ULL << count) - 1);
        value &= mask;
        if (startBit + count <= 64) {
            lo |= value << startBit;
        } else if (startBit >= 64) {
            hi |= value << (startBit - 64);
        } else {
            const int loBits = 64 - startBit;
            lo |= (value & ((1ULL << loBits) - 1)) << startBit;
            hi |= value >> loBits;
        }
    };

    setBits(0, 5, 0b00011);              // mode 11
    setBits(5, 10, static_cast<uint64_t>(e));   // RW
    setBits(15, 10, static_cast<uint64_t>(e));  // GW
    setBits(25, 10, static_cast<uint64_t>(e));  // BW
    setBits(35, 10, static_cast<uint64_t>(e));  // RX
    setBits(45, 10, static_cast<uint64_t>(e));  // GX
    setBits(55, 10, static_cast<uint64_t>(e));  // BX
    // Indices [127:65] stay zero.

    std::array<uint8_t, 16> out{};
    for (int i = 0; i < 8; ++i) out[i]     = static_cast<uint8_t>((lo >> (i * 8)) & 0xFF);
    for (int i = 0; i < 8; ++i) out[i + 8] = static_cast<uint8_t>((hi >> (i * 8)) & 0xFF);
    return out;
}

// Wrap dense BC6H block bytes in a single SEC_HAPHU_RAW (0xA2) HAP section,
// with the spec's 4-byte header (24-bit LE size + 1-byte type).
static std::vector<uint8_t> wrapAsHapHURaw(const std::vector<uint8_t>& blocks) {
    std::vector<uint8_t> packet;
    packet.reserve(4 + blocks.size());
    const uint32_t size = static_cast<uint32_t>(blocks.size());
    packet.push_back(static_cast<uint8_t>(size & 0xFF));
    packet.push_back(static_cast<uint8_t>((size >> 8) & 0xFF));
    packet.push_back(static_cast<uint8_t>((size >> 16) & 0xFF));
    packet.push_back(SEC_HAPHU_RAW);
    packet.insert(packet.end(), blocks.begin(), blocks.end());
    return packet;
}

static int generateHapHFixture(const std::string& dst, int width, int height,
                               int frames, int fps) {
    if ((width & 3) != 0 || (height & 3) != 0) {
        std::cerr << "haph: width and height must be multiples of 4 (got "
                  << width << "x" << height << ")" << std::endl;
        return 1;
    }

    // Constant ~half-float 4.0 across the whole image. Anything > 1.0 makes
    // the FP16-survival point — exact value isn't critical; the goal is "this
    // pixel arrives at the compose target with non-clipped HDR amplitude."
    constexpr uint16_t kTargetHalf = 0x4400;  // IEEE binary16 for 4.0
    const int e = pickBC6HUnsignedEndpoint(kTargetHalf);
    const auto block = encodeBC6HMode11ConstantBlock(e);

    const int blocksX = width / 4;
    const int blocksY = height / 4;
    std::vector<uint8_t> blocks(static_cast<size_t>(blocksX) * blocksY * 16);
    for (int i = 0; i < blocksX * blocksY; ++i) {
        std::memcpy(blocks.data() + static_cast<size_t>(i) * 16, block.data(), 16);
    }
    const std::vector<uint8_t> hapPacket = wrapAsHapHURaw(blocks);

    AVFormatContext* out = nullptr;
    int ret = avformat_alloc_output_context2(&out, nullptr, "mov", dst.c_str());
    if (ret < 0 || !out) return die("avformat_alloc_output_context2", ret);

    AVStream* stream = avformat_new_stream(out, nullptr);
    if (!stream) return die("avformat_new_stream");
    stream->time_base     = AVRational{1, fps};
    stream->avg_frame_rate = AVRational{fps, 1};
    stream->r_frame_rate  = AVRational{fps, 1};

    AVCodecParameters* cp = stream->codecpar;
    cp->codec_type = AVMEDIA_TYPE_VIDEO;
    cp->codec_id   = AV_CODEC_ID_HAP;
    // FFmpeg's ff_codec_movvideo_tags table doesn't list 'HapH' for AV_CODEC_ID_HAP
    // — the MOV muxer/demuxer only recognizes Hap1 / Hap5 / HapY / Hap7 / HapA /
    // HapM. We tag the container as 'Hap1' (which round-trips), but the packet
    // body is a SEC_HAPHU_RAW (0xA2) section, so HapFormat::parseHapPacket
    // produces BC6H_UF16 regardless. The codec_tag only drives dispatch (HAP-vs-
    // anything-else), not the per-frame texture format. The "Hap (BC1 RGB)" log
    // line that HAPDecoder::open emits is harmless; the decoded outFrame.format
    // is the parser's BC6H_UF16.
    cp->codec_tag  = MKTAG('H', 'a', 'p', '1');
    cp->width      = width;
    cp->height     = height;
    cp->format     = AV_PIX_FMT_NONE;  // compressed bitstream — no source pix_fmt

    if (!(out->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&out->pb, dst.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) return die("avio_open", ret);
    }
    ret = avformat_write_header(out, nullptr);
    if (ret < 0) return die("avformat_write_header", ret);

    // The MOV muxer may rewrite stream->time_base (typically to 1/15360 for
    // video) inside write_header. Rescale per-frame timestamps from the source
    // 1/fps timebase into whatever the muxer settled on, matching what the
    // libavcodec encoder path does via av_packet_rescale_ts.
    const AVRational srcTb{1, fps};

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return die("av_packet_alloc");

    for (int i = 0; i < frames; ++i) {
        ret = av_new_packet(pkt, static_cast<int>(hapPacket.size()));
        if (ret < 0) return die("av_new_packet", ret);
        std::memcpy(pkt->data, hapPacket.data(), hapPacket.size());
        pkt->stream_index = stream->index;
        pkt->pts = av_rescale_q(i, srcTb, stream->time_base);
        pkt->dts = pkt->pts;
        pkt->duration = av_rescale_q(1, srcTb, stream->time_base);
        pkt->flags |= AV_PKT_FLAG_KEY;
        ret = av_interleaved_write_frame(out, pkt);
        av_packet_unref(pkt);
        if (ret < 0) return die("av_interleaved_write_frame", ret);
    }

    ret = av_write_trailer(out);
    if (ret < 0) return die("av_write_trailer", ret);

    av_packet_free(&pkt);
    if (out->pb) avio_closep(&out->pb);
    avformat_free_context(out);

    std::cout << "wrote " << dst << " (" << width << "x" << height << ", "
              << frames << " frames, haph BC6H_UF16 endpoint=" << e
              << " ≈ half-float 0x" << std::hex
              << ((((static_cast<int>(((static_cast<uint32_t>(e) << 16) + 0x8000) >> 10)) * 31) >> 6) & 0xFFFF)
              << std::dec << ")" << std::endl;
    return 0;
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

    // HapH (BC6H_UF16 HDR) goes through a hand-rolled raw-packet path because
    // libavcodec's HAP encoder doesn't accept HDR inputs. Everything else uses
    // the encoder.
    if (variant == "haph" || variant == "hap_hdr") {
        return generateHapHFixture(dst, width, height, frames, fps);
    }

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
