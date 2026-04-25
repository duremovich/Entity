#pragma once

namespace entity {

/**
 * FrameBuffer - empty marker tag for clips that are decode-managed.
 *
 * Pre-Phase C.10 this carried a per-clip FrameRingBuffer and atomic state
 * for the decode-thread / main-thread handshake. With the engine-global
 * FrameCache (include/entity/media/FrameCache.hpp) all of that state lives
 * in one place; the only thing left for FrameBuffer to do is mark "this
 * clip should have a decode worker" so DecodeSystem's view query keeps
 * working without touching every emplace site.
 *
 * Will likely be removed entirely once a follow-up confirms `Clip` alone is
 * a sufficient marker (every Clip-with-media is decode-managed today).
 */
struct FrameBuffer {};

} // namespace entity
