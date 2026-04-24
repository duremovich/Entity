#pragma once

/**
 * HapTranscoder — convert any FFmpeg-decodable video into a HAP-family .mov.
 *
 * Used on media import to turn heavy source formats (ProRes 4444 XQ, H.264,
 * HEVC, etc.) into GPU-friendly HAP so real-time playback doesn't CPU-bind
 * on decode. HAP decoding is near-free — pre-compressed BC blocks upload
 * directly to the GPU via our HAPDecoder + HapFormat parser.
 *
 * Variants (FFmpeg's built-in HAP encoder supports these):
 *   - "hap"       — Hap1, BC1 RGB, no alpha, smallest files
 *   - "hap_alpha" — Hap5, BC3 RGBA, direct, works with existing shaders (DEFAULT)
 *   - "hap_q"     — HapY, BC3 scaled YCoCg, higher quality RGB (no alpha);
 *                   needs the YCoCg shader variant to render correctly
 *
 * HAP R (Hap7, BC7) and HAP H (BC6H HDR) encoding require an out-of-tree
 * FFmpeg fork or a separate BC7/BC6H encoder (see Phase C.2 notes in the
 * plan file). Decoding HAP R/H works today via HapFormat + HAPDecoder.
 *
 * Requires the ffmpeg vcpkg port built with the 'snappy' feature so the
 * hap encoder is registered. Without it, avcodec_find_encoder(AV_CODEC_ID_HAP)
 * returns nullptr and `transcodeToHap` returns DecoderError.
 */

#include "entity/core/Types.hpp"
#include <string>
#include <functional>

namespace entity {

/**
 * Optional progress callback. Invoked roughly once per encoded frame with
 * (framesDone, framesTotal). framesTotal may be 0 if the source container
 * didn't report nb_frames; callers should display "frame N / ?" in that
 * case. Return false from the callback to abort the transcode (producing
 * Result::Failure at the final return).
 */
using TranscodeProgress = std::function<bool(int64_t framesDone, int64_t framesTotal)>;

/**
 * Transcode `srcPath` to a HAP-family .mov at `dstPath`. Synchronous; the
 * caller is expected to run this on a background thread (see TranscodeWorker
 * when that lands).
 *
 * `variant` is one of "hap", "hap_alpha" (default), or "hap_q". See the
 * class doc for trade-offs.
 *
 * `srcFps` > 0 hints the source framerate — required for image-sequence
 * inputs (e.g. "frames_%03d.png") where the image2 demuxer can't infer
 * timing. Use 0.0 for regular video containers (default).
 */
Result transcodeToHap(const std::string& srcPath,
                      const std::string& dstPath,
                      const std::string& variant = "hap_alpha",
                      TranscodeProgress progress = {},
                      double srcFps = 0.0);

} // namespace entity
