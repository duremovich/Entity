# ADR-0001: D3D12 first, Metal as Phase E+ second backend

- **Status:** Accepted
- **Date:** 2026-04-19
- **Context source:** master roadmap `~/.claude/plans/i-haven-t-worked-on-declarative-hennessy.md` § "ADR-2026-04-19"
- **Implemented by:** Phase B decomposition (commits `070b556`, `ecadc83`, `7d8a7c8`, `b224bde`, `2cdf086`, `9b6a142`)

## Context

Before committing to Phase B's renderer decomposition we evaluated four
alternatives to native D3D12: SDL3 GPU (Valve-backed abstraction), BGFX
(mature 10+ year abstraction), WebGPU native (too early for commercial 2026),
and a custom thin RHI. We also evaluated native-per-platform (D3D12 + Metal +
optional Vulkan-for-Linux).

The stated commercial target is Windows live-performance venues — ~95% of the
existing Disguise/Watchout/Pixera/Resolume/Notch market is Windows-native
already. Mac is "eventually wanted" with no hard timeline. Linux is
deprioritized.

## Decision

Keep D3D12 as the sole rendering implementation through Phase D. Design an
`IRenderer` pure-virtual interface during Phase B so systems depend on the
abstraction, not on D3D12 concretely. Move HLSL shader compilation from
runtime (`D3DCompileFromFile`) to offline DXC-emitted DXIL — the same pipeline
can emit SPIR-V later for a Metal port.

Implement a Metal backend in Phase E+ when Mac customer demand materializes.
Linux/Vulkan is not currently on the roadmap.

## Consequences

**Enables:**
- Full native D3D12 control + PIX-level debugging (gold standard for
  live-show GPU issues).
- Cleaner root signatures, simpler synchronization, ~20-30% less code than
  Vulkan for equivalent functionality on Windows.
- Future Metal port possible without rewriting non-renderer code (the
  `IRenderer` interface is the seam).

**Forbids:**
- Mac/Linux ports today. Both are explicitly deferred.

**Forces:**
- Discipline on the `IRenderer` interface so it doesn't drift toward
  D3D12-isms. Method names and semantics stay at the level of compose-targets,
  layers, surfaces — not API primitives like "set D3D12 root constant
  buffer." Mitigated by keeping D3D12 leakage out of headers above
  `src/render/`: no `D3D12_GPU_DESCRIPTOR_HANDLE` or `DirectX::XMMATRIX` in
  signatures outside the renderer subtree (verified post-Phase B #18).
- A future Mac port will be ~3-4K LOC of new Metal code — not a flag flip.

## Alternatives considered

- **SDL3 GPU.** Valve-backed, ABI-stable, HLSL→SPIR-V→MSL via SDL_shadercross.
  Rejected because: (a) lacks compute shaders and descriptor indexing, (b) no
  public commercial media-server shipping on it, (c) the 2-3 week migration
  was 10-20% of the Phase A-C solo budget for zero immediate customer
  benefit.
- **BGFX.** Mature, but requires rewriting all HLSL to its `.sc` dialect, and
  weaker descriptor indexing hurts video-texture pipelines.
- **Custom thin RHI.** Months-long project that only pays off if a second
  backend lands anyway — at which point writing Metal directly against a thin
  C++ interface is cheaper.
- **Vulkan on Windows.** No cross-platform value when only shipping Windows.
  Doesn't give us Mac (MoltenVK has translation overhead and feature gaps).
  Strictly more code than D3D12 for the same functionality.
- **Native-per-platform from day one.** Metal port is large enough to deserve
  its own phase. Doing it speculatively under solo-developer time pressure
  would either ship two half-broken backends or bottleneck Phase B for months.

## Revisit when

- Mac customer / partner demand materializes with hard timeline.
- A venue-market shift makes Windows-only feel like a constraint.
- The `IRenderer` interface starts taking on D3D12-specific shape that would
  make a Metal port impractical — this is the early-warning signal that the
  abstraction needs a real second implementation to keep it honest.

## References

- Master roadmap: `~/.claude/plans/i-haven-t-worked-on-declarative-hennessy.md`
- Phase B subtask 17 (`IRenderer` interface): commit `070b556`
- Phase B subtask 19 (offline DXC compilation): commit `7d8a7c8`
