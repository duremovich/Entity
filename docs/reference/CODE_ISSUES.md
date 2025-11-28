# Known Code Issues

Condensed from code review (2025-11-27). Full details in `docs/archive/CODE_REVIEW_2025-11-27.md`.

**Summary**: 7 Critical, 18 High, 27 Medium, 15 Low

---

## Critical Issues (7)

| ID | File | Issue |
|----|------|-------|
| CRIT-01 | FrameRingBuffer.cpp:93-148 | Race condition in `consumeUpTo()` - TOCTOU between search and validity check |
| CRIT-02 | DecodeSystem.cpp:261-296 | Use-after-free in decode thread - raw pointer to DecodeWorker may become dangling |
| CRIT-03 | D3D12Renderer.cpp:1096-1100 | Static `firstUpload` persists across ALL video textures - breaks state transitions |
| CRIT-04 | D3D12Renderer.cpp:818-824 | Double-mapped constant buffer without GPU sync - race between CPU writes and GPU reads |
| CRIT-05 | Engine.cpp:1216-1217 | Memory leak - `m_currentFrame` re-allocated on each video load without freeing |
| CRIT-06 | Clip.hpp:23-27 | FFmpeg context pointers not cleaned up - no destructor frees AVFormatContext/AVCodecContext |
| CRIT-07 | ProResDecoder.cpp:306-307 | Buffer overflow risk - no dimension validation before `sws_scale()` |

---

## High Priority Issues (18)

| ID | File | Issue |
|----|------|-------|
| HIGH-01 | FrameBuffer.hpp:23-26 | `shared_ptr<FrameRingBuffer>` not thread-safe for concurrent access |
| HIGH-02 | Engine.cpp:1415,1522 | Playback state race - caches state then re-checks, inconsistent |
| HIGH-03 | DecodeSystem.cpp:346-363 | Seek error handling - clears buffer BEFORE seek attempt |
| HIGH-04 | D3D12Renderer.cpp:178-245 | Compose target not released in `shutdown()` |
| HIGH-05 | D3D12Renderer.cpp:289 | Viewport/scissor not set in `beginFrame()` - stale values after resize |
| HIGH-06 | D3D12Renderer.cpp:883,1266 | Descriptor heap overflow risk - no bounds check on slot allocation |
| HIGH-07 | D3D12Renderer.cpp:208-213 | Legacy `m_videoUploadBuffer` not released in shutdown |
| HIGH-08 | Timeline.cpp:231-411 | Split/duplicate doesn't copy AnimatedProperties - animation lost |
| HIGH-10 | MediaBinWindow.cpp:39-86 | O(n²) performance - iterates all clips for each media file |
| HIGH-11 | AnimatedProperties.hpp:78-89 | Keyframe insertion uses O(n log n) sort instead of O(n) insert |
| HIGH-12 | PNGSequenceDecoder.cpp:179-183 | No file size limit - DoS risk with huge files |
| HIGH-13 | HAPDecoder.cpp:12-37 | Not implemented but factory creates it - silent failure |
| HIGH-14 | ProResDecoder.cpp:230-275 | No frame number validation before seeking |
| HIGH-15 | FrameRingBuffer.cpp:127-129 | `isFull()` check is racy relative to `push()` |
| HIGH-16 | Engine.cpp:277-278 | Raw pointers to systems may become dangling |
| HIGH-17 | PropertyWindow.cpp:138 | Static `uniformScale` leaks between clips |
| HIGH-18 | MediaBinWindow.hpp:26 | `m_engine` pointer uninitialized (missing `{nullptr}`) |

---

## Medium Priority Issues (27)

| ID | File | Issue |
|----|------|-------|
| MED-01 | Transform.hpp:28-51 | Component has logic (ECS violation) - `updateMatrix()` should be in system |
| MED-02 | OutputMapping.hpp | Components contain large COM pointers and std::string - poor cache locality |
| MED-03 | VideoTexture.hpp:30-32 | Dangling pointer risk - `currentFrame` owned by FrameBuffer |
| MED-04 | TimelineTrack.hpp:23-26 | Clips vector not kept sorted - relies on TimelineSystem |
| MED-05 | Engine.cpp:316-318 | Three separate maps for per-clip data - fragmentation |
| MED-06 | ProResDecoder.cpp:288-303 | SwsContext not recreated on format change |
| MED-07 | DecodeSystem.cpp:384-409 | Silent frame drop on full buffer - infinite retry loop |
| MED-08 | ProResDecoder.cpp:121-133 | Frame duration uses floating point - off-by-one risk |
| MED-09 | PNGSequenceDecoder.cpp:240 | Premultiply alpha integer division rounds down - color banding |
| MED-10 | PNGSequenceDecoder.cpp:60-68 | No PNG sequence validation - assumes all same dimensions |
| MED-11 | DecodeSystem.cpp:104-119 | Discontinuity detection uses UINT32_MAX but FrameNumber is int64_t |
| MED-12 | D3D12Renderer.cpp:1615-1617 | Redundant descriptor heap sets on every draw call |
| MED-13 | D3D12Renderer.cpp:561-577 | GPU stall in `moveToNextFrame()` - INFINITE wait |
| MED-14 | D3D12Renderer.cpp:2143-2199 | Screenshot buffer never shrinks after large capture |
| MED-15 | D3D12Renderer.cpp:1954 | No compose target dimension validation (0x0) |
| MED-16 | TimelineWidget.cpp:347 | Clip Y position doesn't account for expanded clips |
| MED-17 | AnimatedProperties.hpp:133,147 | Boundary condition inconsistency (`<=` vs `<`) |
| MED-18 | Timeline.hpp:70-74 | Frame number truncation - should round |
| MED-19 | AnimatedProperties.hpp:78-154 | Linear search instead of binary search on sorted keyframes |
| MED-20 | Timeline.cpp:61-67 | `seek()` auto-pause not thread-safe |
| MED-21 | Engine.cpp:1074,1241,1658 | Inconsistent decoder creation error handling |
| MED-22 | MappingWindow.cpp:65-67 | Static counter for naming surfaces never resets |
| MED-23 | MappingWindow.cpp:112-127 | UI selection state mixed with domain logic |
| MED-24 | MappingWindow.cpp:262-357 | Event handling relies on implicit order |
| MED-25 | WindowManager.cpp:255-285 | File dialogs assume MAX_PATH, no error logging |
| MED-26 | MappingWindow.cpp:220 | No null check for `m_engine` |
| MED-27 | StageWindow.cpp:35 | Manual cursor offset calculation is fragile |

---

## Low Priority Issues (15)

| ID | File | Issue |
|----|------|-------|
| LOW-01 | Transform.hpp:21-22 | `mutable` keyword breaks const-correctness |
| LOW-02 | Decoder.cpp:25-51 | Media type detection is extension-based only |
| LOW-03 | DecodeSystem.hpp:165-167 | Fixed `DECODE_AHEAD_FRAMES = 8` doesn't adapt |
| LOW-04 | DecodeSystem.hpp:172-188 | No seek completion callback |
| LOW-05 | DecodeSystem.cpp:415-418 | `calculateDecodeAhead()` function unused |
| LOW-06 | DecodeSystem.cpp:407 | Excessive frame decode logging |
| LOW-07 | TimelineWidget.cpp:750 | Uses INT32_MAX but FrameNumber is int64_t |
| LOW-08 | AnimatedProperties.hpp:75-89 | Keyframes can be added outside clip duration |
| LOW-09 | Timeline.cpp:135-166 | `clear()` doesn't clear TimelineWidget expansion state |
| LOW-10 | WindowManager.cpp:94-101 | O(n) window lookup by name |
| LOW-11 | WindowManager.cpp:237-239 | Hardcoded layout magic numbers |
| LOW-12 | Multiple | Inconsistent null check patterns |
| LOW-13 | MappingWindow.cpp:443-472 | PushID/PopID not scoped in loop |
| ~~LOW-14~~ | ~~D3D12Renderer.cpp~~ | ~~Descriptor heap slot layout hardcoded~~ - **FIXED**: Added MAX_COMPOSE_TARGETS constant |
| LOW-15 | Stage3DRenderer.cpp:17 | Temporary Camera copy on every projection |

---

## Recommended Fix Order

**Phase 1 - Critical Stability**:
1. CRIT-01, CRIT-02 (threading safety)
2. CRIT-03 (static state bug)
3. CRIT-06 (FFmpeg leak)
4. HIGH-08 (animation loss)

**Phase 2 - Memory Safety**:
5. CRIT-04, CRIT-05, CRIT-07
6. HIGH-01, HIGH-04, HIGH-07

**Phase 3 - Correctness**:
7. HIGH-02, HIGH-03, HIGH-05, HIGH-06
8. HIGH-14, HIGH-15

**Phase 4 - Performance**:
9. HIGH-10, HIGH-11
10. MED-05, MED-12, MED-19
