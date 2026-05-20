# ADR-0011: Projector calibration — manual correspondence + LM solve + post-fit IDW residual warp

- **Status:** Accepted
- **Date:** 2026-05-03
- **Context source:** iterative session ending in the per-vertex
  distortion fix that closed the "warp doesn't quite land at cal
  points" bug. Working plan:
  `~/.claude/plans/i-would-like-to-robust-token.md`.
- **Implemented by:** Three commits on `master`:
  - `9d097be` — initial QuickCal-style point picking + LM solver
  - `a3543d2` — lens distortion (k1, k2) + precision-cursor UX
  - this commit — per-vertex distortion in OutputManager mesh
    rendering + opt-in post-fit IDW residual warp ("Warp to Points")

## Context

Projector calibration recovers the projector's pose, intrinsics, and
lens distortion so virtual content can be projected onto a physical
surface with sub-pixel accuracy. Entity is a single-machine MVP — no
auxiliary structured-light camera, no dedicated calibration rig.
Whatever calibration we ship has to work with the hardware the user
already has: the projector, the surface, and a mouse.

Three industry approaches were considered:

1. **Manual correspondence + nonlinear least squares.** User picks 3D
   world points on the surface, drags a crosshair to where each point
   physically lands in the projector frame, hits Solve. The de-facto
   approach for live-show media servers without aux camera. Sample
   counts: 6–15 correspondences typical, 4 minimum for coplanar
   surfaces.
2. **Camera-based / structured light.** A camera observes the surface
   while the projector flashes a known pattern; correspondences are
   recovered automatically. Highest accuracy, but requires aux
   hardware, calibrated camera intrinsics, and physical line-of-sight
   to the projection surface from a useful angle.
3. **Neural / learned calibration.** Image-to-pose networks (DeepCalib,
   CompenNet). Solves a different problem space (single-image, fixed
   FOV) and isn't useful for our manual-correspondence workflow.

Approach #1 is what we ship. The rest of the ADR documents how it's
factored.

## Decision

Three-tier responsibility split:

### Tier 1 — Solver

`CalibrationSolver` (in `src/calibration/`) owns the **9-DOF nonlinear
fit**: position (3), Euler rotation pitch/yaw/roll (3), vertical FOV
(1), Brown-Conrady radial distortion `k1`, `k2` (2). Levenberg-Marquardt
via `Eigen::NumericalDiff` with multi-start (~30 deterministic +
Gaussian-perturbed seeds), a roll-sanity filter (rejects 180°
flipped-roll local minima), iterative outlier-trim (drop worst >
max(2.5×median, 5px) up to 3 iterations), and a final iterative-LM
refinement loop (≤20 passes, stop at improvement < 0.001 px).

Public surface:

- `solve()` — analytical bootstrap (homography for coplanar+known FOV,
  DLT for general 3D). Brittle; used only as one of the multi-start
  seeds.
- `solveRefine(pairs, outputSize, initialPos, initialRot, initialFov,
  lockFov, lockDistortion)` — the real entry point. Returns a
  `CalibrationResult` with `position`, `rotationEuler`, `fovDegrees`,
  `distortionK1`, `distortionK2`, `rmsErrorPixels`, `success`,
  `errorMessage`.
- `perPointResiduals(pairs, outputSize, result)` — per-point
  reprojection error in pixels, for the per-point error column in the
  UI.

The residual functor projects each world point through pinhole
matrix → Brown-Conrady distortion → projector pixel space, residual is
2 components (du, dv) per correspondence. Distortion is computed in
**normalized image coordinates** (aspect-aware), not raw NDC:

```
nx = ndc.x * aspect * tan(FOV/2)
ny = ndc.y * tan(FOV/2)
r²  = nx² + ny²
scale = 1 + k1·r² + k2·r⁴
nx *= scale; ny *= scale
ndc.x = nx / (aspect * tan(FOV/2))
ndc.y = ny / tan(FOV/2)
```

Aspect-awareness matters: real lens distortion is radially symmetric
in image coordinates (which include aspect ratio), not in NDC (which
treats x and y symmetrically over [-1, 1]). For 16:9 outputs with
non-trivial k1 the difference is several pixels.

### Tier 2 — Renderer

Two renderers consume the solver's output: `OutputManager` (physical
projector) and `Stage3DRenderer` (right-pane preview in the
calibration window). Both apply identical math, in this order:

1. World point → projector view-projection matrix → clip space →
   perspective divide to NDC.
2. **Lens distortion per-vertex** using the same aspect-aware formula
   the solver used, with the same `k1`, `k2` values that were just
   fit. This step is load-bearing — see "the framebuffer↔world
   chain" below.
3. **Optional residual warp** in projector UV space, blending
   per-cal-point residuals via inverse-distance weighting:

   ```
   for each cal point i:
       predUV_i = lens_distort(pinhole_project(world_i, pose))
       residual_i = measuredUV_i - predUV_i
   for each rendered vertex:
       uv = vertex's distorted UV
       weights_i = 1 / (||uv - predUV_i||² + 1e-6)
       warp = Σ(weights_i · residual_i) / Σ(weights_i)
       uv += warp
   ```

   Special case: if `||uv - predUV_i||² < 1e-8` (vertex coincides with
   a cal point) → just return `uv + residual_i` directly. This pins
   cal points to exactly their measured framebuffer position.

The warp is **opt-in** via `Projector::useResidualWarp` (off by
default). When off, the rendered output is the solver's pose+distortion
fit and nothing else — clean, deterministic, no point-pinning.

### Tier 3 — UI

`ProjectorCalibrationWindow` owns correspondence collection and
persistence. It does not implement any projection math; it calls into
the solver and renderers. Features:

- 3D scene pane: orbit camera, click-to-pick 3D mesh point with
  vertex snap.
- Projector preview pane: drag crosshair to align with physical beam.
- Per-point error column (color-coded green<2px / yellow<10px /
  red>10px) and residual arrows from each crosshair to the predicted
  UV.
- Lock-FOV / Lock-Distortion checkboxes (reduce DOF when manufacturer
  specs are known or when point count is too low to fit lens model).
- Precision Cursor toggle: full-frame quadrant checkerboard centered
  on the active crosshair for sub-pixel placement against physical
  features.
- "Warp to Points" toggle: flips `useResidualWarp` after a successful
  solve.
- All correspondences persist on the `Projector` component
  (`calibrationPoints`) so close-and-reopen doesn't lose work.

## The framebuffer ↔ world chain (the load-bearing invariant)

The chain that anyone editing the projection pipeline must keep
straight:

> Framebuffer pixel `F` → projector lens → physical world position `W`.
>
> The projector lens is a fixed ray bundle: each framebuffer pixel
> shoots out at a specific world ray. The lens model `(pose, k1, k2)`
> approximates this mapping.
>
> To render a mesh vertex at world `W_v`, the framebuffer pixel must
> be `F = lens_distort(pinhole_project(W_v, pose))`. The physical lens
> then "undoes" the distortion and lands at `W_v`.
>
> **Skip the distortion step in rendering and every vertex is offset
> by the lens distortion amount, before any warp.** This was the
> bug fixed in this ADR's commit: OutputManager rendered at pinhole
> coordinates only, while the warp's residuals were computed in
> distorted coordinates. At every cal point the warp left an
> unrecoverable residual exactly equal to the lens distortion at that
> point.

Phrased as a rule for editors:

**Anywhere a 3D point becomes a framebuffer pixel for a physical
projector, lens distortion (currently k1+k2; in the future maybe
tangential p1+p2 or fisheye) must be applied with the same formula
the solver fit it with.**

Today this rule is enforced by hand at five sites:

1. `CalibrationSolver`'s `ProjectorRefineFunctor::operator()` (residuals)
2. `OutputManager`'s projector mesh-render branch (per-vertex distortion)
3. `OutputManager`'s warp-residual computation (predUV at each cal point)
4. `Stage3DRenderer::projectPoint` (preview projection)
5. `ProjectorCalibrationWindow::renderProjectorPane` (warp residuals
   for the Stage3DRenderer preview)

If you change the lens model in one and forget the others, cal points
won't land. The four follow-up cards on the project board include a
"single `projectorProjection()` helper" refactor to collapse this to
one site.

## The warp absorbs ground truth

The IDW warp's design intent: **cal points are absolute truth, no
matter what the solver fit**. The user spent time placing each
crosshair against a physical feature; the warp respects that
placement exactly via the `dist² < 1e-8` early-return.

Between cal points, IDW interpolates with `1/dist²` weights. This is
C⁰-continuous (no derivative continuity), which means the warp can
have visible "creases" through cal points if residuals at adjacent
points pull in opposite directions. Thin-plate splines would be
smoother, but require a 2N×2N solve per warp setup; deferred until a
user actually notices the C⁰ artifact.

## Consequences

**Enables:**
- Sub-pixel cal-point alignment without any structured-light hardware.
- Iterative refinement: user places points, solves, sees per-point
  error, fixes the worst, re-solves. The solve loop is cheap enough
  (~50ms for 8 points) that this is real-time.
- Right-pane preview perfectly matches physical output (when both
  apply identical math) — the user can iterate on alignment without
  walking back and forth between the editor and the projector.
- The warp is opt-in, so users who prefer "show me the actual solver
  fit" can leave it off and ship a deterministic pose.

**Forbids:**
- Editing lens-model code in just one of the five sites (see "the
  framebuffer↔world chain" above). All five must move together.
- Rendering projector output without applying distortion when k1/k2
  are non-zero. The output will be visibly off, the warp will fail
  to fully recover, and the bug looks like "warp doesn't work" rather
  than "renderer skipped distortion".

**Forces:**
- Per-vertex distortion application is a linear approximation of a
  smooth radial field. Sparse meshes (a 12-tri cube) show visible
  mid-edge distortion error. Dense meshes (anything subdivided to
  ~screen-pixel triangle size) are fine. Tracked as a follow-up.
- Adding a new lens-model term (tangential, fisheye) is a five-site
  change today. Refactor target.

## Alternatives considered

- **Camera-based / structured-light calibration** (an automated
  approach used by high-end systems). Multi-day implementation,
  requires aux hardware, requires camera intrinsic calibration first.
  Deferred to Phase E+ as a Pro feature gated on proper hardware
  story. Today's manual workflow gets to "good enough for live shows"
  without the hardware ask.
- **Solver-only (no warp).** Ships RMS in the 3–8 px range on
  realistic placements; ceiling determined by solver model accuracy
  and click precision. Even a perfect Ceres-quality solve can't beat
  the lens model's accuracy at cal points the way an explicit warp
  can. Rejected as the only mode; kept as the default-off behavior.
- **Warp-only (skip the lens model, let the warp absorb everything).**
  Tested in early design. At cal points it works (warp pins them
  exactly), but between cal points the warp is interpolating the
  entire radial distortion field from sparse points — IDW from 8
  points cannot recover a smooth radial function as well as an
  explicit `k1·r² + k2·r⁴` model. The combination (lens model +
  warp residual) is strictly better than either alone.
- **Per-pixel lens distortion in a fragment shader** (instead of
  per-vertex). Mathematically correct, no mid-edge approximation
  error. Requires a fragment-shader path through OutputManager.
  Per-vertex is good enough at projector-mapping mesh densities;
  revisit if a user complains.
- **Higher-order distortion (k3, rational, fisheye).** Gated on much
  higher correspondence counts than typical setups have (~20+). Skip
  unless a use case appears.
- **Random-start LM with no closed-form initial guess.** This is what
  we ship today, and it's why we need ~30 multi-start seeds. SQPnP
  (general 3D) and IPPE (planar) provide globally-optimal closed-form
  pose; one closed-form seed → one LM refine instead of 30 random
  seeds. Tracked as a future improvement; downgraded since the warp
  now pins cal points exactly, so the solver only needs to provide a
  smooth interpolation backbone.
- **Ceres Solver swap** (autodiff Jacobian + quaternion manifold +
  Cauchy loss). Was the original "Phase B" plan; would lift RMS from
  3–8 px to 0.5–2 px. Still queued, but downgraded — the warp now
  pins cal points exactly, so RMS is no longer the dominant quality
  metric; cal-point coverage and click precision are.

## References

- ADR-0009 (structured projects) — calibration points persist on the
  `Projector` component; the project file carries the cal state.
- Working plan:
  `~/.claude/plans/i-would-like-to-robust-token.md` — the full
  research notes (including SOTA survey) and follow-up roadmap.
- Follow-up cards on the project board (filed alongside this ADR):
  dense-mesh tessellation, thin-plate spline warp option, warp-
  magnitude diagnostic, single-projection-helper refactor.
