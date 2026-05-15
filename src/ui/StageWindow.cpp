#include "entity/ui/StageWindow.hpp"
#include "entity/core/Engine.hpp"
#include "entity/render/IRenderer.hpp"
#include "entity/render/D3D12Renderer.hpp"
#include "entity/render/DescriptorHeapLayout.hpp"
#include "entity/renderer/Renderer.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/components/MediaLayer.hpp"
#include "entity/components/Transform.hpp"
#include "entity/components/Screen.hpp"
#include "entity/components/Model.hpp"
#include "entity/components/Projector.hpp"
#include "entity/components/Prop.hpp"
#include "entity/components/Layer.hpp"
#include "entity/components/ObjectAnimationLayer.hpp"
#include "entity/components/ObjectAnimationOutput.hpp"
#include "entity/systems/CameraControlSystem.hpp"
#include "entity/media/DecodedFrame.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>  // For std::sort, std::clamp
#include <cmath>      // For std::exp
#include <vector>

namespace entity {

namespace {

// Stage 3D pulls position/rotation/size directly from Screen / Prop
// components, but those don't reflect the live OA-layer overrides that
// AnimationSystem writes to ObjectAnimationOutput (the editor-side fold-in
// only happens in PlaybackTimeAuthority::buildSceneSnapshot, which feeds
// the projector output path — not the editor's Stage viewport). Without
// this fold-in here, dragging a PositionX slider on an OA layer would
// move the projector output but the Stage 3D preview would stay frozen
// at the screen's base position, which is exactly what the operator
// reports as "nothing happens."
//
// Precedence mirrors PlaybackTimeAuthority::buildSceneSnapshot's screens
// loop and ADR-0020: an active OA layer wins over an after-end-Hold OA
// layer; lower trackIndex wins within either category. AnimationSystem
// has already populated ObjectAnimationOutput correctly for both
// scenarios — we just pick the right one. Keep this in lockstep with the
// canonical fold-in if precedence ever changes.
void applyOAOverridesForTarget(entt::registry& registry,
                                FrameNumber currentFrame,
                                entt::entity targetEntity,
                                glm::vec3& position,
                                glm::vec3& rotation,
                                std::array<float, 3>& sizeOverride,
                                bool& sizeWasOverridden) {
    sizeWasOverridden = false;
    if (targetEntity == entt::null) return;

    const ObjectAnimationOutput* winningActive = nullptr;
    uint32_t winningActiveIdx = UINT32_MAX;
    const ObjectAnimationOutput* winningHold = nullptr;
    uint32_t winningHoldIdx = UINT32_MAX;

    auto view = registry.view<ObjectAnimationLayer, ObjectAnimationOutput, Layer>();
    for (auto [oaEntity, oal, oaOut, oaLayer] : view.each()) {
        if (oal.target != targetEntity) continue;
        const bool active   = currentFrame >= oaLayer.startFrame &&
                              currentFrame <  oaLayer.startFrame + oaLayer.duration;
        const bool afterEnd = currentFrame >= oaLayer.startFrame + oaLayer.duration;
        if (active) {
            if (oaLayer.trackIndex < winningActiveIdx) {
                winningActiveIdx = oaLayer.trackIndex;
                winningActive    = &oaOut;
            }
        } else if (afterEnd &&
                   oal.endBehavior == ObjectAnimationLayer::EndBehavior::Hold) {
            if (oaLayer.trackIndex < winningHoldIdx) {
                winningHoldIdx = oaLayer.trackIndex;
                winningHold    = &oaOut;
            }
        }
    }
    const ObjectAnimationOutput* winning = winningActive ? winningActive : winningHold;
    if (!winning) return;

    if (winning->hasPosition) {
        position.x = winning->positionOverride[0];
        position.y = winning->positionOverride[1];
        position.z = winning->positionOverride[2];
    }
    if (winning->hasRotation) {
        rotation.x = winning->rotationOverride[0];
        rotation.y = winning->rotationOverride[1];
        rotation.z = winning->rotationOverride[2];
    }
    if (winning->hasSize) {
        sizeOverride      = winning->sizeOverride;
        sizeWasOverridden = true;
    }
}

} // namespace

StageWindow::StageWindow(Engine* engine)
    : m_engine(engine)
    , m_3dRenderer(std::make_unique<Stage3DRenderer>())
    , m_calibWindow(std::make_unique<ProjectorCalibrationWindow>(engine)) {
    // Initialize camera to default position
    CameraControlSystem::reset(m_3dRenderer->getCamera());
}

void StageWindow::render() {
    // Carve a child region for the view that excludes the bottom toolbar.
    // Without this, render2DView() / render3DView() paint into the full
    // content region and the toolbar overlays the bottom strip.
    const float toolbarHeight = ImGui::GetFrameHeightWithSpacing()
                              + ImGui::GetStyle().ItemSpacing.y
                              + 6.0f;

    ImVec2 contentRegion = ImGui::GetContentRegionAvail();
    ImVec2 viewSize(contentRegion.x, std::max(contentRegion.y - toolbarHeight, 1.0f));

    // Auto-frame on view-mode change or first render with content.
    applyAutoFrame(viewSize);

    if (ImGui::BeginChild("##StageView", viewSize, false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        if (m_viewMode == StageViewMode::View3D) {
            render3DView();
        } else {
            render2DView();
        }
    }
    ImGui::EndChild();

    renderToolbar();

    // Calibration window is a separate floating ImGui window; render it every frame while open.
    if (m_calibWindow) {
        m_calibWindow->render();
    }
}

void StageWindow::render2DView() {
    ImVec2 contentSize = ImGui::GetContentRegionAvail();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 windowPos = ImGui::GetCursorScreenPos();

    IRenderer* renderer = m_engine ? m_engine->getRenderer() : nullptr;
    Timeline* timeline = m_engine ? m_engine->getTimeline() : nullptr;

    // Try to display the composited result from CompositorSystem
    if (renderer && renderer->isComposeTargetReady()) {
        void* textureID = renderer->getComposeTargetTextureID();

        if (textureID) {
            // Get compose target dimensions
            uint32_t composeWidth = renderer->getComposeTargetWidth();
            uint32_t composeHeight = renderer->getComposeTargetHeight();

            // Aspect-fit "natural" size, then apply m_2dZoom multiplier and pan offset.
            float composeAspect = static_cast<float>(composeWidth) / static_cast<float>(composeHeight);
            float windowAspect = contentSize.x / contentSize.y;

            ImVec2 fitSize;
            if (composeAspect > windowAspect) {
                fitSize.x = contentSize.x;
                fitSize.y = contentSize.x / composeAspect;
            } else {
                fitSize.y = contentSize.y;
                fitSize.x = contentSize.y * composeAspect;
            }

            ImVec2 imageSize(fitSize.x * m_2dZoom, fitSize.y * m_2dZoom);
            ImVec2 imagePos(
                windowPos.x + (contentSize.x - imageSize.x) * 0.5f + m_2dPan.x,
                windowPos.y + (contentSize.y - imageSize.y) * 0.5f + m_2dPan.y
            );

            // Gray background so the surface boundary is visible against black output
            drawList->AddRectFilled(
                windowPos,
                ImVec2(windowPos.x + contentSize.x, windowPos.y + contentSize.y),
                IM_COL32(60, 60, 65, 255)
            );

            // Display the composited texture (transforms already applied by CompositorSystem)
            ImGui::SetCursorScreenPos(imagePos);
            ImGui::Image(
                textureID,
                imageSize,
                ImVec2(0.0f, 0.0f),  // UV start
                ImVec2(1.0f, 1.0f),  // UV end
                ImVec4(1.0f, 1.0f, 1.0f, 1.0f),  // Full opacity (compositing already done)
                ImVec4(0.0f, 0.0f, 0.0f, 0.0f)   // Border (none)
            );

            // Thin border over the image to mark the output surface edge
            drawList->AddRect(
                imagePos,
                ImVec2(imagePos.x + imageSize.x, imagePos.y + imageSize.y),
                IM_COL32(100, 100, 100, 200)
            );

            // Pan / zoom input. Only act when the view is hovered so we don't
            // hijack scroll over toolbars or other windows.
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
                ImGuiIO& io = ImGui::GetIO();

                // Cursor-anchored zoom on scroll wheel.
                if (io.MouseWheel != 0.0f) {
                    float oldZoom = m_2dZoom;
                    float newZoom = std::clamp(oldZoom * std::exp(io.MouseWheel * 0.15f),
                                               MIN_2D_ZOOM, MAX_2D_ZOOM);
                    if (newZoom != oldZoom) {
                        // Keep the world-space point under the cursor fixed.
                        // imagePos = windowCenter - imageSize/2 + pan, so the
                        // cursor's offset from the image center scales with zoom.
                        ImVec2 windowCenter(windowPos.x + contentSize.x * 0.5f,
                                            windowPos.y + contentSize.y * 0.5f);
                        ImVec2 cursorRel(io.MousePos.x - windowCenter.x - m_2dPan.x,
                                         io.MousePos.y - windowCenter.y - m_2dPan.y);
                        float scale = newZoom / oldZoom;
                        m_2dPan.x += cursorRel.x - cursorRel.x * scale;
                        m_2dPan.y += cursorRel.y - cursorRel.y * scale;
                        m_2dZoom = newZoom;
                    }
                }

                // Pan: middle-drag, or shift+left-drag.
                bool wantPan = ImGui::IsMouseDown(ImGuiMouseButton_Middle)
                            || (ImGui::IsMouseDown(ImGuiMouseButton_Left) && io.KeyShift);
                if (wantPan) {
                    if (m_2dPanning) {
                        m_2dPan.x += io.MouseDelta.x;
                        m_2dPan.y += io.MouseDelta.y;
                    } else {
                        m_2dPanning = true;
                    }
                } else {
                    m_2dPanning = false;
                }

                // Double-click to reset.
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    reset2DView();
                }
            } else {
                m_2dPanning = false;
            }

            // Overlay frame info in corner
            if (timeline) {
                FrameNumber currentFrame = timeline->getCurrentFrame();
                char frameInfo[64];
                snprintf(frameInfo, sizeof(frameInfo), "Frame %lld | %ux%u | %.0f%%",
                         static_cast<long long>(currentFrame), composeWidth, composeHeight, m_2dZoom * 100.0f);

                ImVec2 infoPos(windowPos.x + 5.0f, windowPos.y + 5.0f);
                drawList->AddText(infoPos, IM_COL32(255, 255, 255, 180), frameInfo);
            }

            return;  // Successfully rendered compose target
        }
    }

    // Fallback: No compose target ready, show placeholder
    {
        // No video loaded, display test pattern
        drawList->AddRectFilledMultiColor(
            windowPos,
            ImVec2(windowPos.x + contentSize.x, windowPos.y + contentSize.y),
            IM_COL32(40, 40, 45, 255),   // Top-left
            IM_COL32(40, 40, 45, 255),   // Top-right
            IM_COL32(25, 25, 30, 255),   // Bottom-right
            IM_COL32(25, 25, 30, 255)    // Bottom-left
        );

        // Draw center crosshair
        ImVec2 center(windowPos.x + contentSize.x * 0.5f, windowPos.y + contentSize.y * 0.5f);
        float crosshairSize = 20.0f;

        drawList->AddLine(
            ImVec2(center.x - crosshairSize, center.y),
            ImVec2(center.x + crosshairSize, center.y),
            IM_COL32(100, 100, 100, 255),
            1.0f
        );

        drawList->AddLine(
            ImVec2(center.x, center.y - crosshairSize),
            ImVec2(center.x, center.y + crosshairSize),
            IM_COL32(100, 100, 100, 255),
            1.0f
        );

        // Draw instruction text
        const char* instructionText = "No Video Loaded\n\nFile > Import Video to load PNG sequence or video file";
        ImVec2 textSize = ImGui::CalcTextSize(instructionText);
        ImVec2 textPos(center.x - textSize.x * 0.5f, center.y + crosshairSize + 10.0f);

        drawList->AddText(textPos, IM_COL32(150, 150, 150, 255), instructionText);
    }
}

void StageWindow::render3DView() {
    ImVec2 contentSize = ImGui::GetContentRegionAvail();

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 windowPos = ImGui::GetCursorScreenPos();

    Timeline* timeline = m_engine ? m_engine->getTimeline() : nullptr;

    // D3D12Renderer needed for the GPU stage pass (renderMeshes).
    D3D12Renderer* d3d = nullptr;
    if (m_engine) {
        if (auto* rs = m_engine->getRendererService())
            d3d = rs->getD3D12Renderer();
    }

    // Get selected stage primitive for highlighting (mutually exclusive
    // between screens / props / projectors at the Timeline level).
    entt::entity selectedProp   = timeline ? timeline->getSelectedProp()   : entt::null;

    // Begin 3D rendering (draws background, grid, axes)
    m_3dRenderer->beginRender(drawList, windowPos, contentSize);

    // -------------------------------------------------------------------------
    // Collect visible screens and props.
    // All screens go into meshDraws (GPU pass). Props with meshes also go into
    // meshDraws; mesh-less props fall back to the ImGui placeholder-cross path.
    // -------------------------------------------------------------------------
    // Minimal per-screen data kept for CPU hit-testing (all screens, including
    // those routed through the GPU mesh pass).
    struct ScreenHitData {
        entt::entity entity;
        glm::vec3 position;
        glm::vec3 rotation;
        glm::vec3 scale;
        const MeshData* mesh{nullptr};
    };
    std::vector<ScreenHitData> screenHitData;

    struct PropDrawData {
        entt::entity entity;
        glm::vec3 position;
        glm::vec3 rotation;
        glm::vec3 scale;
        glm::vec4 displayColor;
        bool isSelected;
        float distanceFromCamera;
        const MeshData* mesh{nullptr};
    };
    std::vector<PropDrawData> propsToDraw;  // no-mesh props (placeholder cross) only

    std::vector<Stage3DRenderer::StageMeshDraw> meshDraws;

    if (m_engine) {
        auto& registry = m_engine->getRegistry();
        glm::vec3 cameraPos = m_3dRenderer->getCamera().position;

        // Current frame snapshot for OA-override resolution. Read once per
        // render so all per-entity calls see a consistent value.
        const FrameNumber currentFrame =
            (m_engine->getTimeline() ? m_engine->getTimeline()->getCurrentFrame() : 0);

        // --- Screens ---
        // Every visible screen routes through the GPU mesh pass for depth-correct z-order.
        // Custom-mesh screens use their model's slot; flat screens use the shared default quad.
        for (auto [entity, screen] : registry.view<Screen>().each()) {
            if (!screen.visible) continue;

            glm::vec3 position(screen.position[0], screen.position[1], screen.position[2]);
            glm::vec3 rotation(screen.rotation[0], screen.rotation[1], screen.rotation[2]);

            // OA fold-in for Stage preview — see applyOAOverridesForTarget
            // for the precedence rule. Without this, dragging an OA value
            // slider would update ObjectAnimationOutput correctly but the
            // Stage 3D viewport would keep showing the screen's base
            // position, making the layer feel broken.
            std::array<float, 3> oaSize{1.0f, 1.0f, 1.0f};
            bool oaSizeOverridden = false;
            applyOAOverridesForTarget(registry, currentFrame, entity,
                                      position, rotation, oaSize, oaSizeOverridden);

            uint32_t meshSlot = UINT32_MAX;
            const MeshData* mesh = nullptr;

            if (screen.modelEntity != entt::null && registry.valid(screen.modelEntity)) {
                if (const auto* m = registry.try_get<Model>(screen.modelEntity)) {
                    if (m->mesh.isValid() && m->gpuResourcesValid) {
                        meshSlot = m->vertexBufferSlot;
                        mesh = &m->mesh;
                    }
                }
            }

            if (meshSlot == UINT32_MAX && d3d) {
                meshSlot = d3d->getDefaultScreenMeshSlot();  // shared 16:9 quad
            }

            // Convert real-world size (meters) to vertex multiplier given the
            // mesh's native bounds. `mesh` is null here when the screen uses
            // the default 16:9 plane fallback, which computeEffectiveScale
            // handles internally. OA size override (when present) substitutes
            // for screen.size — same semantics as buildSceneSnapshot.
            const auto baseSize = oaSizeOverridden ? oaSize : screen.size;
            const auto effScale = computeEffectiveScale(baseSize, mesh);
            glm::vec3 scale(effScale[0], effScale[1], effScale[2]);

            Stage3DRenderer::StageMeshDraw draw;
            draw.meshSlot   = meshSlot;
            draw.position   = position;
            draw.rotation   = rotation;
            draw.scale      = scale;
            draw.isScreen   = true;

            // Triple-buffered compose targets: must read from the *stable*
            // sub-resource the show thread last finished, never the base slot
            // (which may be mid-write — reading it crashes the GPU).
            uint32_t stableSrv = UINT32_MAX;
            if (d3d && screen.renderTargetValid && screen.renderTargetSlot != UINT32_MAX) {
                stableSrv = d3d->getComposeTargetStableSrvSlot(screen.renderTargetSlot);
            }
            if (stableSrv != UINT32_MAX) {
                draw.srvSlot    = stableSrv;
                draw.tint       = glm::vec4(1.0f);
                draw.renderMode = 0;  // textured
            } else {
                draw.srvSlot    = 0;  // unused in renderMode=1
                draw.tint       = glm::vec4(0.16f, 0.16f, 0.18f, 1.0f);  // dark gray no-signal
                draw.renderMode = 1;  // matte
            }
            meshDraws.push_back(draw);

            // Preserve hit-test data for all screens (CPU ray-test is independent of render path).
            screenHitData.push_back({entity, position, rotation, scale, mesh});
        }

        // --- Props ---
        for (auto [entity, prop] : registry.view<Prop>().each()) {
            if (!prop.visible) continue;

            glm::vec3 position(prop.position[0], prop.position[1], prop.position[2]);
            glm::vec3 rotation(prop.rotation[0], prop.rotation[1], prop.rotation[2]);
            glm::vec4 color(prop.displayColor[0], prop.displayColor[1],
                            prop.displayColor[2], prop.displayColor[3]);

            // OA fold-in for Stage preview (props animate the same way
            // screens do — see the screens loop above).
            std::array<float, 3> oaSize{1.0f, 1.0f, 1.0f};
            bool oaSizeOverridden = false;
            applyOAOverridesForTarget(registry, currentFrame, entity,
                                      position, rotation, oaSize, oaSizeOverridden);

            bool isSelected = (entity == selectedProp);
            float dist = glm::length(position - cameraPos);

            const MeshData* mesh = nullptr;
            const Model*    model = nullptr;
            if (prop.modelEntity != entt::null && registry.valid(prop.modelEntity)) {
                if (const auto* m = registry.try_get<Model>(prop.modelEntity))
                    if (m->mesh.isValid()) { mesh = &m->mesh; model = m; }
            }

            // Convert prop.size (meters) → vertex multiplier given the mesh
            // native bounds. For props without a mesh the multiplier is
            // unused (placeholder-cross path uses position only).
            const auto baseSize = oaSizeOverridden ? oaSize : prop.size;
            const auto propEff = computeEffectiveScale(baseSize, mesh);
            glm::vec3 scale(propEff[0], propEff[1], propEff[2]);

            if (mesh) {
                Stage3DRenderer::StageMeshDraw draw;
                draw.meshSlot   = (model && model->gpuResourcesValid) ? model->vertexBufferSlot : UINT32_MAX;
                draw.position   = position;
                draw.rotation   = rotation;
                draw.scale      = scale;
                draw.srvSlot    = 0;  // renderMode=1: PS ignores the texture SRV
                draw.tint       = color;
                draw.renderMode = 1;
                draw.isScreen   = false;
                meshDraws.push_back(draw);
            } else {
                // No mesh → placeholder-cross bucket
                propsToDraw.push_back({entity, position, rotation, scale, color, isSelected, dist, nullptr});
            }
        }

        // Depth-sort the no-mesh prop bucket (farthest first, painter's algorithm)
        std::sort(propsToDraw.begin(), propsToDraw.end(),
                  [](const PropDrawData& a, const PropDrawData& b) {
                      return a.distanceFromCamera > b.distanceFromCamera;
                  });
    }

    // -------------------------------------------------------------------------
    // GPU pass: render all mesh entities into the stage render target, then
    // composite the result behind all 2D overlays via AddImage.
    // -------------------------------------------------------------------------
    if (!meshDraws.empty()) {
        Camera gpuCam = m_3dRenderer->getCamera();
        gpuCam.aspectRatio = contentSize.x / std::max(contentSize.y, 1.0f);

        ImTextureID stageTexId = m_3dRenderer->renderMeshes(
            d3d, gpuCam, meshDraws,
            static_cast<uint32_t>(contentSize.x),
            static_cast<uint32_t>(contentSize.y));

        if (stageTexId) {
            ImVec2 p1(windowPos.x + contentSize.x, windowPos.y + contentSize.y);
            drawList->AddImage(stageTexId, windowPos, p1);
        }
    }

    // -------------------------------------------------------------------------
    // 2D overlay pass: placeholder props, projector gizmos.
    // These draw on top of the GPU image.
    // -------------------------------------------------------------------------
    if (m_engine) {
        auto& registry = m_engine->getRegistry();

        // No-mesh props (placeholder cross, painter's algorithm)
        for (const auto& propData : propsToDraw) {
            m_3dRenderer->drawProp(drawList, windowPos, contentSize,
                                   propData.position, propData.rotation, propData.scale,
                                   propData.displayColor, propData.isSelected, propData.mesh);
        }

        // Projector gizmos
        entt::entity selectedProjector = timeline ? timeline->getSelectedProjector() : entt::null;
        for (auto [projEntity, proj] : registry.view<Projector>().each()) {
            glm::vec3 projPos{proj.position[0], proj.position[1], proj.position[2]};
            glm::vec3 projRot{proj.rotation[0], proj.rotation[1], proj.rotation[2]};
            m_3dRenderer->drawProjector(drawList, windowPos, contentSize,
                                        projPos, projRot, proj.fovDegrees,
                                        proj.enabled, (projEntity == selectedProjector),
                                        proj.name.c_str());
        }
    }

    // End 3D rendering (camera info overlay, pops clip rect)
    m_3dRenderer->endRender(drawList, windowPos, contentSize);

    // Handle input
    ImGuiIO& io = ImGui::GetIO();
    bool isHovered = ImGui::IsWindowHovered();

    if (isHovered) {
        // Handle camera controls
        m_3dRenderer->handleInput(
            io.MousePos,
            windowPos, contentSize,
            io.MouseDown[0],  // Left button
            io.MouseDown[1],  // Right button
            io.MouseDown[2],  // Middle button
            io.KeyShift,
            io.MouseWheel
        );

        // Click to select/deselect. GetMouseDragDelta stays valid on the release frame
        // (unlike IsMouseDragging which requires the button to still be down).
        // It returns (0,0) only when the mouse never moved past io.MouseDragThreshold.
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && timeline) {
            ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
            if (drag.x == 0.0f && drag.y == 0.0f) {
                // Test screens first (nearest to camera = drawn last = tested first).
                // Screens win ties over props because screens are the primary
                // interactive primitive — clip mapping, output routing — while
                // props are passive set dressing.
                entt::entity hitScreen = entt::null;
                for (auto it = screenHitData.rbegin(); it != screenHitData.rend(); ++it) {
                    if (m_3dRenderer->hitTestScreen(io.MousePos, windowPos, contentSize,
                                                    it->position, it->rotation, it->scale, it->mesh)) {
                        hitScreen = it->entity;
                        break;
                    }
                }
                entt::entity hitProp = entt::null;
                if (hitScreen == entt::null) {
                    for (auto it = propsToDraw.rbegin(); it != propsToDraw.rend(); ++it) {
                        if (m_3dRenderer->hitTestProp(io.MousePos, windowPos, contentSize,
                                                      it->position, it->rotation, it->scale, it->mesh)) {
                            hitProp = it->entity;
                            break;
                        }
                    }
                }
                if (hitScreen != entt::null) {
                    timeline->setSelectedScreen(hitScreen);
                    timeline->setSelectedClip(entt::null);
                } else if (hitProp != entt::null) {
                    timeline->setSelectedProp(hitProp);
                    timeline->setSelectedClip(entt::null);
                } else {
                    // Test projector apex positions (within a small radius)
                    entt::entity hitProjector = entt::null;
                    if (m_engine) {
                        const float kRadius = 8.0f;
                        auto& reg = m_engine->getRegistry();
                        Camera projCam = m_3dRenderer->getCamera();
                        projCam.aspectRatio = contentSize.x / contentSize.y;
                        glm::mat4 vp = projCam.getViewProjectionMatrix();

                        for (auto [projEntity, proj] : reg.view<Projector>().each()) {
                            glm::vec4 clip = vp * glm::vec4(proj.position[0], proj.position[1], proj.position[2], 1.0f);
                            if (clip.w <= 0.001f) continue;
                            glm::vec3 ndc = glm::vec3(clip) / clip.w;
                            float sx = windowPos.x + (ndc.x + 1.0f) * 0.5f * contentSize.x;
                            float sy = windowPos.y + (1.0f - ndc.y) * 0.5f * contentSize.y;
                            float dx = io.MousePos.x - sx;
                            float dy = io.MousePos.y - sy;
                            if (dx * dx + dy * dy <= kRadius * kRadius) {
                                hitProjector = projEntity;
                                break;
                            }
                        }
                    }
                    if (hitProjector != entt::null) {
                        timeline->setSelectedProjector(hitProjector);
                        timeline->setSelectedClip(entt::null);
                    } else {
                        timeline->setSelectedScreen(entt::null);
                        timeline->setSelectedProjector(entt::null);
                        timeline->setSelectedProp(entt::null);
                    }
                }
            }
        }
    }

}

void StageWindow::renderToolbar() {
    ImGui::Separator();

    // View mode buttons
    ImGui::BeginGroup();

    bool is2D = (m_viewMode == StageViewMode::View2D);
    bool is3D = (m_viewMode == StageViewMode::View3D);

    if (is2D) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::SmallButton("2D")) {
        requestViewMode(StageViewMode::View2D);
    }
    if (is2D) ImGui::PopStyleColor();

    ImGui::SameLine();

    if (is3D) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::SmallButton("3D")) {
        requestViewMode(StageViewMode::View3D);
    }
    if (is3D) ImGui::PopStyleColor();

    if (m_viewMode == StageViewMode::View2D) {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        if (ImGui::SmallButton("Fit")) {
            reset2DView();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("| Wheel: Zoom  MMB/Shift+LMB: Pan  Dbl-click: Fit");
    } else if (m_viewMode == StageViewMode::View3D && m_3dRenderer) {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        if (ImGui::SmallButton("Frame")) {
            // Manual content-aware reframe. Use the current view area
            // size from the parent window's content region for aspect.
            ImVec2 viewSize = ImGui::GetWindowSize();
            frameAllScreens(viewSize);
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Reset")) {
            CameraControlSystem::reset(m_3dRenderer->getCamera());
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Front")) {
            CameraControlSystem::setFrontView(m_3dRenderer->getCamera());
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Top")) {
            CameraControlSystem::setTopView(m_3dRenderer->getCamera());
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Side")) {
            CameraControlSystem::setSideView(m_3dRenderer->getCamera());
        }

        ImGui::SameLine();
        ImGui::TextDisabled("| LMB: Orbit  MMB: Pan  Scroll: Zoom");

        // Calibrate button — only shown when a Projector is selected
        Timeline* tl = m_engine ? m_engine->getTimeline() : nullptr;
        entt::entity selProj = tl ? tl->getSelectedProjector() : entt::null;
        if (selProj != entt::null) {
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();

            bool calibOpen = m_calibWindow && m_calibWindow->isOpen();
            if (calibOpen)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::SmallButton("Calibrate")) {
                if (calibOpen) {
                    m_calibWindow->close();
                } else if (m_calibWindow) {
                    m_calibWindow->open(selProj);
                }
            }
            if (calibOpen)
                ImGui::PopStyleColor();
        }
    }

    ImGui::EndGroup();
}

void StageWindow::requestViewMode(StageViewMode mode) {
    if (mode == m_viewMode) {
        return;
    }
    m_viewMode = mode;
    m_pendingAutoFrame = true;
}

void StageWindow::applyAutoFrame(ImVec2 viewSize) {
    // First-render auto-frame: once the engine has any visible Screens,
    // frame them so an opened project doesn't land on the default
    // (0, 0.5, 0) Reset position.
    if (!m_didInitialFrame && m_engine) {
        auto& registry = m_engine->getRegistry();
        bool hasAnyStage = false;
        for (auto [entity, screen] : registry.view<Screen>().each()) {
            if (screen.visible) { hasAnyStage = true; break; }
        }
        if (!hasAnyStage) {
            for (auto [entity, prop] : registry.view<Prop>().each()) {
                if (prop.visible) { hasAnyStage = true; break; }
            }
        }
        if (hasAnyStage) {
            m_pendingAutoFrame = true;
            m_didInitialFrame = true;
        }
    }

    if (!m_pendingAutoFrame) {
        return;
    }

    if (m_viewMode == StageViewMode::View2D) {
        reset2DView();
    } else if (m_viewMode == StageViewMode::View3D && m_3dRenderer) {
        frameAllScreens(viewSize);
    }

    m_pendingAutoFrame = false;
}

void StageWindow::frameAllScreens(ImVec2 viewSize) {
    if (!m_3dRenderer || !m_engine) {
        return;
    }

    // Match aspect to the view we'll actually render into so the FOV
    // calculation in frameScreens() picks the binding axis correctly.
    if (viewSize.x > 0.0f && viewSize.y > 0.0f) {
        m_3dRenderer->getCamera().aspectRatio = viewSize.x / viewSize.y;
    }

    std::vector<Stage3DRenderer::ScreenFrameInput> inputs;
    auto& registry = m_engine->getRegistry();
    for (auto [entity, screen] : registry.view<Screen>().each()) {
        if (!screen.visible) continue;
        const MeshData* mesh = nullptr;
        if (screen.modelEntity != entt::null && registry.valid(screen.modelEntity)) {
            if (const auto* model = registry.try_get<Model>(screen.modelEntity))
                if (model->mesh.isValid()) mesh = &model->mesh;
        }
        const auto eff = computeEffectiveScale(screen.size, mesh);
        inputs.push_back({
            glm::vec3(screen.position[0], screen.position[1], screen.position[2]),
            glm::vec3(screen.rotation[0], screen.rotation[1], screen.rotation[2]),
            glm::vec3(eff[0], eff[1], eff[2]),
            mesh,
        });
    }

    // Include props in the framing AABB so a "Frame" press centers the full
    // stage, not just the projection surfaces. Screens with no mesh + zero
    // props still framing-defaults to the legacy behavior.
    std::vector<Stage3DRenderer::PropFrameInput> propInputs;
    for (auto [entity, prop] : registry.view<Prop>().each()) {
        if (!prop.visible) continue;
        const MeshData* mesh = nullptr;
        if (prop.modelEntity != entt::null && registry.valid(prop.modelEntity)) {
            if (const auto* model = registry.try_get<Model>(prop.modelEntity))
                if (model->mesh.isValid()) mesh = &model->mesh;
        }
        const auto eff = computeEffectiveScale(prop.size, mesh);
        propInputs.push_back({
            glm::vec3(prop.position[0], prop.position[1], prop.position[2]),
            glm::vec3(prop.rotation[0], prop.rotation[1], prop.rotation[2]),
            glm::vec3(eff[0], eff[1], eff[2]),
            mesh,
        });
    }

    m_3dRenderer->frameScreens(inputs, propInputs);
}

void StageWindow::reset2DView() {
    m_2dZoom = 1.0f;
    m_2dPan = ImVec2(0.0f, 0.0f);
    m_2dPanning = false;
}

} // namespace entity
