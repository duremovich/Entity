# Entity Media Server - Development Checklist

## Phase 0: Documentation & Foundation ✅ COMPLETE
## Phase 1: Project Scaffold & Dependencies ✅ COMPLETE
## Phase 2: Core Engine & Window ✅ COMPLETE
## Phase 3: ECS Components & Systems ✅ COMPLETE

## Phase 4: Media Decoding Pipeline 🔄 IN PROGRESS

### Completed
- [x] System infrastructure (System.hpp base class, Engine integration)
- [x] Component refactoring (Clip, FrameBuffer → pure data)
- [x] FrameRingBuffer (lock-free circular buffer, 18 tests passing)
- [x] Decoder base class + ProRes/HAP/PNG implementations

### Current Task
- [ ] **DecodeSystem** - Integrate decoders with ECS (per-clip workers, feed FrameRingBuffer)

### Remaining
- [ ] Decode threading (background workers, atomic state)
- [ ] Premultiplied alpha conversion pipeline
- [ ] Integration tests (full decode → buffer → texture flow)

## Phase 5: D3D12 Rendering & Compositing
- [ ] Texture upload pipeline (staging buffers, GPU copy)
- [ ] HLSL shaders (vertex/pixel for textured quads)
- [ ] Compositor (multi-layer sorting and blending)
- [ ] RenderSystem integration with swap chain

## Phase 6: Timeline Engine ✅ COMPLETE (UI Components)
- [x] Timeline model (tracks, playhead, frame mapping)
- [x] Timeline widget rendering (ruler, tracks, clips, playhead)
- [x] Interactive features (zoom, scrub, drag clips)
- [x] Time-to-pixel coordinate conversion
- [ ] Transport controls (play, pause, stop) - requires media playback
- [ ] Seeking and looping support - requires media playback

## Phase 7: ImGui Debug UI ✅ COMPLETE (Core Features)
- [x] ImGui D3D12 backend integration
- [x] Timeline viewer with scrubbing
- [ ] Layer list with visibility toggles
- [ ] Preview window and transport controls

## Phase 8: Display Output Mapping
- [ ] DXGI display enumeration
- [ ] EDID parsing for persistent mapping
- [ ] Per-output swap chains
- [ ] Multi-monitor configuration UI

---

**Current Focus**: Phase 4 - DecodeSystem implementation
**Next Up**: Phase 5 - D3D12 texture upload and rendering
**See**: CLAUDE.md for architecture, ECS_GUIDELINES.hpp for design patterns
