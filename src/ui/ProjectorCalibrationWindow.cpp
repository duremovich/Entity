#include "entity/ui/ProjectorCalibrationWindow.hpp"
#include "entity/core/Engine.hpp"
#include "entity/render/D3D12Renderer.hpp"
#include "entity/renderer/Renderer.hpp"
#include "entity/components/Projector.hpp"
#include "entity/components/Screen.hpp"
#include "entity/components/Model.hpp"
#include "entity/components/OutputDisplay.hpp"
#include "entity/media/ObjLoader.hpp"
#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <sstream>

namespace entity {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static glm::vec2 imvec2ToGlm(ImVec2 v) { return glm::vec2(v.x, v.y); }
static ImVec2    glmToImVec2(glm::vec2 v) { return ImVec2(v.x, v.y); }

// 2D cross product (scalar)
static float cross2D(ImVec2 a, ImVec2 b) { return a.x * b.y - a.y * b.x; }

// Barycentric coordinates of P in 2D triangle (a, b, c).
// Returns false if degenerate.
static bool barycentricCoords2D(ImVec2 p, ImVec2 a, ImVec2 b, ImVec2 c,
                                  float& u, float& v, float& w) {
    ImVec2 v0{b.x - a.x, b.y - a.y};
    ImVec2 v1{c.x - a.x, c.y - a.y};
    ImVec2 v2{p.x - a.x, p.y - a.y};
    float d00 = v0.x*v0.x + v0.y*v0.y;
    float d01 = v0.x*v1.x + v0.y*v1.y;
    float d11 = v1.x*v1.x + v1.y*v1.y;
    float d20 = v2.x*v0.x + v2.y*v0.y;
    float d21 = v2.x*v1.x + v2.y*v1.y;
    float denom = d00*d11 - d01*d01;
    if (std::abs(denom) < 1e-8f) return false;
    v = (d11*d20 - d01*d21) / denom;
    w = (d00*d21 - d01*d20) / denom;
    u = 1.0f - v - w;
    return (u >= -1e-4f && v >= -1e-4f && w >= -1e-4f);
}

// Project a 3D world point through a camera's VP matrix to 2D screen coords.
// Returns ImVec2(-FLT_MAX, -FLT_MAX) if behind camera.
static ImVec2 projectPoint(const glm::vec3& worldPos, const glm::mat4& vp,
                             ImVec2 panePos, ImVec2 paneSize) {
    glm::vec4 clip = vp * glm::vec4(worldPos, 1.0f);
    if (clip.w < 1e-6f) return ImVec2(-1e9f, -1e9f);
    float x = clip.x / clip.w;
    float y = clip.y / clip.w;
    float sx = panePos.x + (x + 1.0f) * 0.5f * paneSize.x;
    float sy = panePos.y + (1.0f - y) * 0.5f * paneSize.y;
    return ImVec2(sx, sy);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

ProjectorCalibrationWindow::ProjectorCalibrationWindow(Engine* engine)
    : m_engine(engine) {
    m_sceneCamera.reset();
    m_sceneCamera.updateFromOrbit();
}

ProjectorCalibrationWindow::~ProjectorCalibrationWindow() {
    close();
}

void ProjectorCalibrationWindow::open(entt::entity projectorEntity) {
    m_projectorEntity = projectorEntity;
    m_isOpen          = true;
    m_points.clear();
    m_activePointIdx  = -1;
    m_hasPending      = false;
    m_hasSolution     = false;
    m_lastResult      = {};
    m_rightPaneDragging = false;

    // Load persisted calibration points from the Projector component.
    if (m_engine && m_engine->getRegistry().valid(projectorEntity)) {
        auto& reg = m_engine->getRegistry();
        if (auto* p = reg.try_get<Projector>(projectorEntity)) {
            m_points.reserve(p->calibrationPoints.size());
            for (const auto& cp : p->calibrationPoints) {
                CorrespondencePointEx pt;
                pt.worldPos    = glm::vec3(cp.worldPos[0], cp.worldPos[1], cp.worldPos[2]);
                pt.projectorUV = glm::vec2(cp.projectorUV[0], cp.projectorUV[1]);
                pt.isAligned   = cp.isAligned;
                m_points.push_back(pt);
            }
            if (!m_points.empty()) m_activePointIdx = 0;
        }
    }

    // Restore screen choice from Projector.targetSurfaces[0] if previously
    // saved; otherwise fall back to the first Screen entity in the registry.
    if (m_engine) {
        auto& reg = m_engine->getRegistry();
        m_screenEntity = entt::null;
        if (reg.valid(projectorEntity)) {
            if (const auto* p = reg.try_get<Projector>(projectorEntity)) {
                if (p->targetSurfaceCount > 0 &&
                    reg.valid(p->targetSurfaces[0]) &&
                    reg.all_of<Screen>(p->targetSurfaces[0])) {
                    m_screenEntity = p->targetSurfaces[0];
                }
            }
        }
        if (m_screenEntity == entt::null) {
            for (auto [e, s] : reg.view<Screen>().each()) {
                m_screenEntity = e;
                break;
            }
        }

        // Auto-route to a sensible output so the user immediately sees the
        // projector view on the actual display. Without this, the right pane
        // works (software render) but the physical output keeps showing the
        // flat sourceScreen texture until the user manually picks a route.
        if (reg.valid(m_projectorEntity)) {
            auto& proj = reg.get<Projector>(m_projectorEntity);

            entt::entity targetOutput = entt::null;
            // 1. Explicit link from a previous session / MappingWindow assignment.
            if (reg.valid(proj.linkedOutput) && reg.all_of<OutputDisplay>(proj.linkedOutput))
                targetOutput = proj.linkedOutput;
            // 2. An output already configured with this projector as its source.
            if (targetOutput == entt::null) {
                for (auto [oe, od] : reg.view<OutputDisplay>().each()) {
                    if (od.sourceProjector == m_projectorEntity) {
                        targetOutput = oe;
                        break;
                    }
                }
            }
            // 3. Exactly one Physical output exists — unambiguous default.
            if (targetOutput == entt::null) {
                int physicalCount = 0;
                entt::entity onlyPhysical = entt::null;
                for (auto [oe, od] : reg.view<OutputDisplay>().each()) {
                    if (od.type == OutputType::Physical) {
                        onlyPhysical = oe;
                        ++physicalCount;
                    }
                }
                if (physicalCount == 1) targetOutput = onlyPhysical;
            }

            if (targetOutput != entt::null) {
                if (const auto* od = reg.try_get<OutputDisplay>(targetOutput))
                    m_outputSize = glm::uvec2(od->width, od->height);
                proj.linkedOutput = targetOutput;  // persist the choice
                routeToOutput(targetOutput);
            } else if (reg.valid(proj.linkedOutput)) {
                if (const auto* od = reg.try_get<OutputDisplay>(proj.linkedOutput))
                    m_outputSize = glm::uvec2(od->width, od->height);
            }
        }
    }

    m_sceneCamera.reset();
    m_sceneCamera.updateFromOrbit();
}

void ProjectorCalibrationWindow::close() {
    if (!m_isOpen) return;
    unrouteOutput();
    m_isOpen = false;
}

// ---------------------------------------------------------------------------
// Main render
// ---------------------------------------------------------------------------

void ProjectorCalibrationWindow::render() {
    if (!m_isOpen) return;
    if (!m_engine) return;

    // ImGuiCond_Appearing: apply the size hint whenever the window (re)appears,
    // but don't fight with dock layout if already docked and sized.
    ImGui::SetNextWindowSize(ImVec2(1100, 680), ImGuiCond_Appearing);
    bool windowOpen = m_isOpen;  // local copy — ImGui writes false when user clicks X
    bool visible    = ImGui::Begin("Projector Calibration", &windowOpen);
    if (!windowOpen) {
        // User clicked the X button (floating or docked tab close)
        ImGui::End();
        close();
        return;
    }
    if (!visible) {
        // Window exists but is collapsed or behind another docked tab
        ImGui::End();
        return;
    }

    renderToolbar();
    ImGui::Separator();

    // Split pane: left scene, right projector output
    ImVec2 avail = ImGui::GetContentRegionAvail();
    const float kMinPaneW  = 80.0f;
    const float kMinPaneH  = 80.0f;
    float  tableHeight = ImGui::GetTextLineHeightWithSpacing() * 7.0f + 30.0f;
    float  solveBarH   = ImGui::GetFrameHeightWithSpacing() * 2.5f;
    float  panesHeight = std::max(avail.y - tableHeight - solveBarH, kMinPaneH);
    float  spacing     = ImGui::GetStyle().ItemSpacing.x;
    float  paneW       = std::max((avail.x - spacing) * 0.5f, kMinPaneW);
    float  rightW      = std::max(avail.x - paneW - spacing, kMinPaneW);

    // Left scene pane
    ImGui::BeginChild("##CalibScene", ImVec2(paneW, panesHeight), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    renderScenePane(ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail());
    ImGui::EndChild();

    ImGui::SameLine();

    // Right projector pane
    ImGui::BeginChild("##CalibProj", ImVec2(rightW, panesHeight), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    renderProjectorPane(ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail());
    ImGui::EndChild();

    renderPointsTable();
    ImGui::Separator();
    renderSolveBar();

    // Persist any point modifications from this frame so the user doesn't
    // lose work if the calibration window is closed and reopened.
    persistPoints();

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------

void ProjectorCalibrationWindow::renderToolbar() {
    auto& reg = m_engine->getRegistry();

    // Projector selector
    ImGui::Text("Projector:");
    ImGui::SameLine();
    std::string projName = "None";
    if (reg.valid(m_projectorEntity)) {
        if (const auto* p = reg.try_get<Projector>(m_projectorEntity))
            projName = p->name;
    }
    ImGui::SetNextItemWidth(140);
    if (ImGui::BeginCombo("##CalibProj", projName.c_str())) {
        for (auto [e, proj] : reg.view<Projector>().each()) {
            bool sel = (e == m_projectorEntity);
            if (ImGui::Selectable(proj.name.c_str(), sel))
                open(e);
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::Text("Screen:");
    ImGui::SameLine();
    std::string screenName = "None";
    if (reg.valid(m_screenEntity)) {
        if (const auto* s = reg.try_get<Screen>(m_screenEntity))
            screenName = s->name;
    }
    ImGui::SetNextItemWidth(120);
    if (ImGui::BeginCombo("##CalibScreen", screenName.c_str())) {
        for (auto [e, screen] : reg.view<Screen>().each()) {
            bool sel = (e == m_screenEntity);
            if (ImGui::Selectable(screen.name.c_str(), sel)) {
                m_screenEntity = e;
                // Persist on Projector.targetSurfaces[0] so the calibration
                // window restores the same screen on reopen.
                if (auto* p = reg.try_get<Projector>(m_projectorEntity)) {
                    p->targetSurfaces[0] = e;
                    if (p->targetSurfaceCount == 0) p->targetSurfaceCount = 1;
                }
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::Text("Route to:");
    ImGui::SameLine();
    std::string routeName = "Off";
    if (reg.valid(m_routedOutputEntity)) {
        if (const auto* od = reg.try_get<OutputDisplay>(m_routedOutputEntity))
            routeName = od->name;
    }
    ImGui::SetNextItemWidth(130);
    if (ImGui::BeginCombo("##CalibRoute", routeName.c_str())) {
        if (ImGui::Selectable("Off", !reg.valid(m_routedOutputEntity)))
            unrouteOutput();
        for (auto [e, od] : reg.view<OutputDisplay>().each()) {
            bool sel = (e == m_routedOutputEntity);
            if (ImGui::Selectable(od.name.c_str(), sel))
                routeToOutput(e);
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::Checkbox("Wireframe", &m_showWireframe);

    ImGui::SameLine();
    bool prevPrecision = m_precisionCursor;
    ImGui::Checkbox("Precision Cursor (C)", &m_precisionCursor);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Full-frame quadrant checkerboard centered on the active\n"
                          "crosshair. Align the sharp 4-quadrant intersection against\n"
                          "the physical feature for sub-pixel placement.");
    }
    // Keyboard 'C' toggle (only when no text input is focused).
    if (!ImGui::GetIO().WantCaptureKeyboard && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        m_precisionCursor = !m_precisionCursor;
    }
    if (m_precisionCursor != prevPrecision) {
        // Force overlay re-render so the physical projector switches mode
        // immediately (otherwise the new state only takes effect on the next
        // crosshair edit that triggers syncOverlayPoints).
        syncOverlayPoints();
    }

    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.0f + ImGui::GetCursorPosX());
    if (ImGui::Button("Done")) close();
}

// ---------------------------------------------------------------------------
// Scene pane (left) — orbitable 3D view, click to pick 3D mesh points
// ---------------------------------------------------------------------------

void ProjectorCalibrationWindow::renderScenePane(ImVec2 panePos, ImVec2 paneSize) {
    if (!m_engine) return;

    // Claim the pane area for mouse input via an invisible button. This prevents
    // ImGui's docking layer from interpreting in-pane drags as "detach this tab"
    // and stops the parent window from being moved.
    //
    // When a pending point exists, exclude the bottom strip so the "Add Point" /
    // "Cancel" buttons get genuine click events (SetItemAllowOverlap fixes hover
    // precedence but not the active-ID claim, so physical separation is the
    // robust solution).
    ImVec2 cursorStart = ImGui::GetCursorScreenPos();
    const float kBottomReserve = m_hasPending ? 36.0f : 0.0f;
    ImVec2 interactSize{paneSize.x, std::max(paneSize.y - kBottomReserve, 50.0f)};
    ImGui::InvisibleButton("##sceneInteract", interactSize,
        ImGuiButtonFlags_MouseButtonLeft |
        ImGuiButtonFlags_MouseButtonRight |
        ImGuiButtonFlags_MouseButtonMiddle);
    bool active        = ImGui::IsItemActive();
    bool hovered       = ImGui::IsItemHovered();
    bool clickedInPane = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    ImGui::SetCursorScreenPos(cursorStart); // rewind for the "Add Point" overlay button

    // Update camera aspect
    m_sceneCamera.aspectRatio = paneSize.x / std::max(paneSize.y, 1.0f);
    m_renderer.setCamera(m_sceneCamera);

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Render stage background + mesh
    m_renderer.beginRender(dl, panePos, paneSize);

    auto& reg = m_engine->getRegistry();
    const MeshData* meshForOverlay = nullptr;
    glm::mat4       meshModel(1.0f);
    if (reg.valid(m_screenEntity)) {
        const auto* screen = reg.try_get<Screen>(m_screenEntity);
        if (screen) {
            const MeshData* mesh = nullptr;
            if (reg.valid(screen->modelEntity)) {
                if (const auto* model = reg.try_get<Model>(screen->modelEntity))
                    mesh = model->mesh.isValid() ? &model->mesh : nullptr;
            }
            void* texID = nullptr;
            if (auto* renderer = m_engine->getRenderer())
                texID = renderer->getComposeTargetTextureID(screen->renderTargetSlot);

            m_renderer.drawScreen(dl, panePos, paneSize,
                glm::vec3(screen->position[0], screen->position[1], screen->position[2]),
                glm::vec3(screen->rotation[0], screen->rotation[1], screen->rotation[2]),
                glm::vec3(screen->scale[0],    screen->scale[1],    screen->scale[2]),
                reinterpret_cast<ImTextureID>(texID), false, mesh);

            if (m_showWireframe && mesh) {
                meshForOverlay = mesh;
                // Match Stage3DRenderer::drawScreen exactly — including screenElevation Y offset.
                glm::vec3 pos(screen->position[0],
                              screen->position[1] + m_renderer.screenElevation,
                              screen->position[2]);
                glm::vec3 rot(screen->rotation[0], screen->rotation[1], screen->rotation[2]);
                glm::vec3 scl(screen->scale[0],    screen->scale[1],    screen->scale[2]);
                meshModel = glm::translate(glm::mat4(1.0f), pos);
                meshModel = glm::rotate(meshModel, glm::radians(rot.y), glm::vec3(0,1,0));
                meshModel = glm::rotate(meshModel, glm::radians(rot.x), glm::vec3(1,0,0));
                meshModel = glm::rotate(meshModel, glm::radians(rot.z), glm::vec3(0,0,1));
                meshModel = glm::scale(meshModel, scl);
            }
        }
    }
    m_renderer.endRender(dl, panePos, paneSize);

    glm::mat4 vp = m_sceneCamera.getViewProjectionMatrix();

    // Wireframe overlay — draw mesh triangle edges in cyan
    if (meshForOverlay) {
        const ImU32 wireCol = IM_COL32(80, 200, 230, 200);
        glm::mat4 mvp = vp * meshModel;
        const auto& verts   = meshForOverlay->vertices;
        const auto& indices = meshForOverlay->indices;
        for (size_t t = 0; t + 2 < indices.size(); t += 3) {
            const auto& v0 = verts[indices[t]];
            const auto& v1 = verts[indices[t+1]];
            const auto& v2 = verts[indices[t+2]];
            glm::vec3 p0(v0.position[0], v0.position[1], v0.position[2]);
            glm::vec3 p1(v1.position[0], v1.position[1], v1.position[2]);
            glm::vec3 p2(v2.position[0], v2.position[1], v2.position[2]);
            ImVec2 s0 = projectPoint(p0, mvp, panePos, paneSize);
            ImVec2 s1 = projectPoint(p1, mvp, panePos, paneSize);
            ImVec2 s2 = projectPoint(p2, mvp, panePos, paneSize);
            if (s0.x > -1e8f && s1.x > -1e8f) dl->AddLine(s0, s1, wireCol, 1.0f);
            if (s1.x > -1e8f && s2.x > -1e8f) dl->AddLine(s1, s2, wireCol, 1.0f);
            if (s2.x > -1e8f && s0.x > -1e8f) dl->AddLine(s2, s0, wireCol, 1.0f);
        }
    }

    // Overlay: confirmed calibration points
    for (int i = 0; i < static_cast<int>(m_points.size()); ++i) {
        ImVec2 sp = projectPoint(m_points[i].worldPos, vp, panePos, paneSize);
        if (sp.x < -1e8f) continue;
        ImU32 col = (i == m_activePointIdx) ? IM_COL32(255,255,0,255) : IM_COL32(255,255,255,200);
        dl->AddCircleFilled(sp, 5.0f, col);
        char label[8];
        snprintf(label, sizeof(label), "%d", i + 1);
        dl->AddText(ImVec2(sp.x + 7, sp.y - 6), col, label);
    }
    // Pending point (red)
    if (m_hasPending) {
        ImVec2 sp = projectPoint(m_pendingWorldPos, vp, panePos, paneSize);
        if (sp.x > -1e8f) {
            dl->AddCircle(sp, 7.0f, IM_COL32(255, 60, 60, 255), 0, 2.0f);
            dl->AddLine(ImVec2(sp.x - 8, sp.y), ImVec2(sp.x + 8, sp.y), IM_COL32(255,60,60,255));
            dl->AddLine(ImVec2(sp.x, sp.y - 8), ImVec2(sp.x, sp.y + 8), IM_COL32(255,60,60,255));
        }
    }

    // Hint text
    dl->AddText(ImVec2(panePos.x + 6, panePos.y + 6),
                IM_COL32(180, 180, 180, 200),
                "3D Scene — click mesh to pick point");

    // Orbit camera — buttons gated by IsItemActive (drag started in this pane).
    ImVec2 mouse = ImGui::GetIO().MousePos;
    m_renderer.handleInput(mouse, panePos, paneSize,
                           active && ImGui::IsMouseDown(0),
                           active && ImGui::IsMouseDown(1),
                           active && ImGui::IsMouseDown(2),
                           ImGui::GetIO().KeyShift,
                           hovered ? ImGui::GetIO().MouseWheel : 0.0f);
    m_sceneCamera = m_renderer.getCamera();

    // Left click → pick 3D mesh point (single click, not the start of a drag)
    if (clickedInPane && !ImGui::IsMouseDragging(0, 4.0f)) {
        glm::vec3 hit;
        if (pickMeshPoint(mouse, panePos, paneSize, m_screenEntity, hit)) {
            m_pendingWorldPos = hit;
            m_hasPending      = true;
        }
    }

    // "Add Point" button — appears when a pending point exists
    if (m_hasPending) {
        ImGui::SetCursorScreenPos(ImVec2(panePos.x + 6, panePos.y + paneSize.y - 28));
        if (ImGui::Button("Add Point")) {
            addCorrespondencePoint();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            m_hasPending = false;
        }
    }
}

// ---------------------------------------------------------------------------
// Projector pane (right) — projector viewpoint, moveable crosshairs
// ---------------------------------------------------------------------------

void ProjectorCalibrationWindow::renderProjectorPane(ImVec2 panePos, ImVec2 paneSize) {
    if (!m_engine) return;

    // Claim mouse input for this pane — same rationale as renderScenePane.
    ImVec2 cursorStart = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##projInteract", paneSize,
        ImGuiButtonFlags_MouseButtonLeft |
        ImGuiButtonFlags_MouseButtonRight |
        ImGuiButtonFlags_MouseButtonMiddle);
    bool active        = ImGui::IsItemActive();
    bool hovered       = ImGui::IsItemHovered();
    bool clickedInPane = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    ImGui::SetItemAllowOverlap();
    ImGui::SetCursorScreenPos(cursorStart);

    // Compute an aspect-correct view rectangle inside the pane that exactly
    // matches the projector's aspect ratio. The projector's UV space (0..1)
    // maps to this rectangle, so the crosshairs the user drags here have the
    // SAME UV interpretation as the physical projector — no aspect-skew bug.
    const float projAspect = (m_outputSize.x > 0 && m_outputSize.y > 0)
                             ? static_cast<float>(m_outputSize.x) / static_cast<float>(m_outputSize.y)
                             : 16.0f / 9.0f;
    const float paneAspect = paneSize.x / std::max(paneSize.y, 1.0f);
    ImVec2 viewPos  = panePos;
    ImVec2 viewSize = paneSize;
    if (paneAspect > projAspect) {
        viewSize.x = paneSize.y * projAspect;
        viewPos.x  = panePos.x + (paneSize.x - viewSize.x) * 0.5f;
    } else {
        viewSize.y = paneSize.x / projAspect;
        viewPos.y  = panePos.y + (paneSize.y - viewSize.y) * 0.5f;
    }

    rebuildProjectorCamera(viewSize);
    m_renderer.setCamera(m_projectorCamera);

    // Simulate physical lens distortion in the right-pane preview so the
    // rendered mesh matches what the user sees through the real projector.
    // (Output rendering stays distortion-free — the hardware lens does it.)
    // Plus: mirror the post-fit residual warp so the preview tracks the
    // physical output when "Warp to Points" is enabled.
    if (m_engine && m_engine->getRegistry().valid(m_projectorEntity)) {
        if (const auto* p = m_engine->getRegistry().try_get<Projector>(m_projectorEntity)) {
            m_renderer.lensK1 = p->distortionK1;
            m_renderer.lensK2 = p->distortionK2;

            m_renderer.warpResiduals.clear();
            if (p->useResidualWarp && p->isCalibrated && !p->calibrationPoints.empty()) {
                const float aspect = paneSize.x / std::max(paneSize.y, 1.0f);
                const float tanHalfFov = std::tan(glm::radians(p->fovDegrees) * 0.5f);
                const float sx = aspect * tanHalfFov;
                const float sy = tanHalfFov;
                glm::mat4 vpMat = m_projectorCamera.getViewProjectionMatrix();
                m_renderer.warpResiduals.reserve(p->calibrationPoints.size());
                for (const auto& cp : p->calibrationPoints) {
                    glm::vec3 wp(cp.worldPos[0], cp.worldPos[1], cp.worldPos[2]);
                    glm::vec4 clip = vpMat * glm::vec4(wp, 1.0f);
                    if (clip.w < 1e-5f) continue;
                    float ndc_x = clip.x / clip.w;
                    float ndc_y = clip.y / clip.w;
                    float nx = ndc_x * sx;
                    float ny = ndc_y * sy;
                    float r2 = nx * nx + ny * ny;
                    float scale = 1.0f + p->distortionK1 * r2 + p->distortionK2 * r2 * r2;
                    nx *= scale; ny *= scale;
                    ndc_x = nx / sx;
                    ndc_y = ny / sy;
                    glm::vec2 predUV((ndc_x + 1.0f) * 0.5f, (1.0f - ndc_y) * 0.5f);
                    glm::vec2 measUV(cp.projectorUV[0], cp.projectorUV[1]);
                    m_renderer.warpResiduals.emplace_back(predUV, measUV - predUV);
                }
            }
        }
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Letterbox bars: fill the pane area outside the projector view rect
    // with black so the user can see the projector's actual frame extent.
    const ImU32 letterboxCol = IM_COL32(15, 15, 15, 255);
    if (viewPos.x > panePos.x)
        dl->AddRectFilled(panePos, ImVec2(viewPos.x, panePos.y + paneSize.y), letterboxCol);
    if (viewPos.x + viewSize.x < panePos.x + paneSize.x)
        dl->AddRectFilled(ImVec2(viewPos.x + viewSize.x, panePos.y),
                          ImVec2(panePos.x + paneSize.x, panePos.y + paneSize.y), letterboxCol);
    if (viewPos.y > panePos.y)
        dl->AddRectFilled(ImVec2(viewPos.x, panePos.y),
                          ImVec2(viewPos.x + viewSize.x, viewPos.y), letterboxCol);
    if (viewPos.y + viewSize.y < panePos.y + paneSize.y)
        dl->AddRectFilled(ImVec2(viewPos.x, viewPos.y + viewSize.y),
                          ImVec2(viewPos.x + viewSize.x, panePos.y + paneSize.y), letterboxCol);

    m_renderer.beginRender(dl, viewPos, viewSize);

    auto& reg = m_engine->getRegistry();
    if (reg.valid(m_screenEntity)) {
        const auto* screen = reg.try_get<Screen>(m_screenEntity);
        if (screen) {
            const MeshData* mesh = nullptr;
            if (reg.valid(screen->modelEntity)) {
                if (const auto* model = reg.try_get<Model>(screen->modelEntity))
                    mesh = model->mesh.isValid() ? &model->mesh : nullptr;
            }
            void* texID = nullptr;
            if (auto* renderer = m_engine->getRenderer())
                texID = renderer->getComposeTargetTextureID(screen->renderTargetSlot);

            m_renderer.drawScreen(dl, viewPos, viewSize,
                glm::vec3(screen->position[0], screen->position[1], screen->position[2]),
                glm::vec3(screen->rotation[0], screen->rotation[1], screen->rotation[2]),
                glm::vec3(screen->scale[0],    screen->scale[1],    screen->scale[2]),
                reinterpret_cast<ImTextureID>(texID), false, mesh);
        }
    }
    m_renderer.endRender(dl, viewPos, viewSize);

    // Reset distortion so the next pane render (left scene pane on the next
    // frame) doesn't pick it up.
    m_renderer.lensK1 = 0.0f;
    m_renderer.lensK2 = 0.0f;

    // Precision cursor mode: cover the rendered mesh with the same quadrant
    // checkerboard the physical projector is showing, so the right pane and
    // physical output stay visually consistent.
    if (m_precisionCursor && m_activePointIdx >= 0 &&
        m_activePointIdx < static_cast<int>(m_points.size())) {
        glm::vec2 c = m_points[m_activePointIdx].projectorUV;
        ImVec2 split{viewPos.x + c.x * viewSize.x, viewPos.y + c.y * viewSize.y};
        const ImU32 W = IM_COL32(255, 255, 255, 255);
        const ImU32 K = IM_COL32(0, 0, 0, 255);
        const ImU32 G = IM_COL32(128, 128, 128, 255);
        // TL=white, TR=black, BL=black, BR=white
        dl->AddRectFilled(viewPos, split, W);
        dl->AddRectFilled(ImVec2(split.x, viewPos.y),
                          ImVec2(viewPos.x + viewSize.x, split.y), K);
        dl->AddRectFilled(ImVec2(viewPos.x, split.y),
                          ImVec2(split.x, viewPos.y + viewSize.y), K);
        dl->AddRectFilled(split,
                          ImVec2(viewPos.x + viewSize.x, viewPos.y + viewSize.y), W);
        // Thin gray reference lines through the cursor.
        dl->AddLine(ImVec2(viewPos.x, split.y),
                    ImVec2(viewPos.x + viewSize.x, split.y), G, 1.0f);
        dl->AddLine(ImVec2(split.x, viewPos.y),
                    ImVec2(split.x, viewPos.y + viewSize.y), G, 1.0f);
    }

    // Compute predicted UV per point using the calibrated projector's pose
    // (Stage3DRenderer's projectPoint already applies our lens distortion in
    // the right pane). Draw a residual line from the user's crosshair to the
    // predicted point — direction + magnitude of the solver's miss.
    auto worldToViewPx = [&](const glm::vec3& wp) -> ImVec2 {
        return m_renderer.projectPoint(wp, viewPos, viewSize);
    };

    // Draw crosshairs at projector-UV positions + residual lines.
    for (int i = 0; i < static_cast<int>(m_points.size()); ++i) {
        glm::vec2 uv = m_points[i].projectorUV;
        ImVec2 cp{viewPos.x + uv.x * viewSize.x, viewPos.y + uv.y * viewSize.y};

        // Residual line: from where the projector's camera says this 3D point
        // projects → to where the user placed the crosshair. Color-coded by
        // pixel magnitude (matches the points table traffic light).
        if (m_hasSolution) {
            ImVec2 pred = worldToViewPx(m_points[i].worldPos);
            if (pred.x > -1e8f) {
                float dx = cp.x - pred.x, dy = cp.y - pred.y;
                float pxMag = std::sqrt(dx*dx + dy*dy)
                            * (static_cast<float>(m_outputSize.x) / std::max(viewSize.x, 1.0f));
                ImU32 lineCol = (pxMag < 1.5f) ? IM_COL32(80, 220, 80, 255)
                              : (pxMag < 5.0f) ? IM_COL32(240, 200, 60, 255)
                                                : IM_COL32(240, 90, 90, 255);
                dl->AddLine(pred, cp, lineCol, 1.5f);
                dl->AddCircleFilled(pred, 3.0f, lineCol);
            }
        }

        float armLen   = 20.0f;
        float armWidth = 1.5f;
        ImU32 col = (i == m_activePointIdx) ? IM_COL32(255,255,0,255) : IM_COL32(255,255,255,220);
        dl->AddLine(ImVec2(cp.x - armLen, cp.y), ImVec2(cp.x + armLen, cp.y), col, armWidth);
        dl->AddLine(ImVec2(cp.x, cp.y - armLen), ImVec2(cp.x, cp.y + armLen), col, armWidth);
        dl->AddCircle(cp, 4.0f, col, 0, armWidth);
        char label[8];
        snprintf(label, sizeof(label), "%d", i + 1);
        dl->AddText(ImVec2(cp.x + 8, cp.y - 14), col, label);
    }

    dl->AddText(ImVec2(panePos.x + 6, panePos.y + 6),
                IM_COL32(180,180,180,200),
                "Projector Output — drag crosshairs to align");

    // Interaction: drag active crosshair (mouse gated by InvisibleButton state).
    // Drags use viewSize (projector UV space), not paneSize, so 1 UV unit = 1
    // projector frame width regardless of how the calibration window is sized.
    ImVec2 mouse = ImGui::GetIO().MousePos;

    if (m_activePointIdx >= 0 && m_activePointIdx < static_cast<int>(m_points.size())) {
        auto& pt = m_points[m_activePointIdx];

        if (clickedInPane) {
            m_rightPaneDragging    = true;
            m_rightPaneDragStart   = mouse;
            m_rightPaneDragStartUV = pt.projectorUV;
        }
        if (!ImGui::IsMouseDown(0)) m_rightPaneDragging = false;

        if (m_rightPaneDragging) {
            ImVec2 delta{mouse.x - m_rightPaneDragStart.x, mouse.y - m_rightPaneDragStart.y};
            pt.projectorUV.x = std::clamp(m_rightPaneDragStartUV.x + delta.x / viewSize.x, 0.0f, 1.0f);
            pt.projectorUV.y = std::clamp(m_rightPaneDragStartUV.y + delta.y / viewSize.y, 0.0f, 1.0f);
        }

        if (hovered || active || m_rightPaneDragging) {
            float pixelX = 1.0f / static_cast<float>(m_outputSize.x);
            float pixelY = 1.0f / static_cast<float>(m_outputSize.y);
            // Modifier scaling: Shift = 10px (coarse), Ctrl = 0.1px (sub-pixel
            // for final precision tuning), default = 1px.
            float mult = 1.0f;
            if      (ImGui::GetIO().KeyShift) mult = 10.0f;
            else if (ImGui::GetIO().KeyCtrl)  mult = 0.1f;
            if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))  pt.projectorUV.x -= pixelX * mult;
            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) pt.projectorUV.x += pixelX * mult;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))    pt.projectorUV.y -= pixelY * mult;
            if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))  pt.projectorUV.y += pixelY * mult;
            pt.projectorUV.x = std::clamp(pt.projectorUV.x, 0.0f, 1.0f);
            pt.projectorUV.y = std::clamp(pt.projectorUV.y, 0.0f, 1.0f);
        }

        pt.isAligned = true;
        syncOverlayPoints();
    } else if (clickedInPane) {
        // Selecting a crosshair to make active — measure distance in view-rect coords.
        float bestDist = 25.0f;
        int bestIdx    = -1;
        for (int i = 0; i < static_cast<int>(m_points.size()); ++i) {
            glm::vec2 uv = m_points[i].projectorUV;
            ImVec2 cp{viewPos.x + uv.x * viewSize.x, viewPos.y + uv.y * viewSize.y};
            float dx = mouse.x - cp.x, dy = mouse.y - cp.y;
            float d = std::sqrt(dx*dx + dy*dy);
            if (d < bestDist) { bestDist = d; bestIdx = i; }
        }
        if (bestIdx >= 0) activatePoint(bestIdx);
    }
}

// ---------------------------------------------------------------------------
// Points table
// ---------------------------------------------------------------------------

void ProjectorCalibrationWindow::renderPointsTable() {
    int minPts = 6; // default DLT
    // Check if planar → might accept 4 with known FOV
    bool coplanar = (m_points.size() >= 4 && CalibrationSolver::isCoplanar(
        std::vector<CorrespondencePair>(m_points.begin(), m_points.end())));
    if (coplanar) minPts = 4;

    int need = std::max(0, minPts - static_cast<int>(m_points.size()));
    char statusBuf[128];
    if (need > 0)
        snprintf(statusBuf, sizeof(statusBuf), "Need %d more point%s (%s surface, min %d)",
                 need, need == 1 ? "" : "s",
                 coplanar ? "planar" : "3D", minPts);
    else
        snprintf(statusBuf, sizeof(statusBuf), "Ready to solve (%s surface, %zu points)",
                 coplanar ? "planar" : "3D", m_points.size());
    ImGui::TextColored(need > 0 ? ImVec4(1,0.7f,0.3f,1) : ImVec4(0.4f,1,0.4f,1), "%s", statusBuf);

    // Compute per-point reprojection residuals once for this frame, using the
    // most recent solve. Empty when no solution yet — column shows '--'.
    std::vector<float> perPtErr;
    if (m_hasSolution) {
        std::vector<CorrespondencePair> pairs(m_points.begin(), m_points.end());
        perPtErr = CalibrationSolver::perPointResiduals(pairs, m_outputSize, m_lastResult);
    }

    if (ImGui::BeginTable("##CalibPts", 5,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                           ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("#",       ImGuiTableColumnFlags_WidthFixed, 22);
        ImGui::TableSetupColumn("3D World (X, Y, Z)", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Proj UV (u, v)",     ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableSetupColumn("Error",              ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Actions",            ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableHeadersRow();

        for (int i = 0; i < static_cast<int>(m_points.size()); ++i) {
            auto& pt = m_points[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", i + 1);

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("(%.2f, %.2f, %.2f)", pt.worldPos.x, pt.worldPos.y, pt.worldPos.z);

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("(%.3f, %.3f)", pt.projectorUV.x, pt.projectorUV.y);

            ImGui::TableSetColumnIndex(3);
            if (i < static_cast<int>(perPtErr.size())) {
                float e = perPtErr[i];
                // Disguise-style traffic light. Tight thresholds so the user
                // can immediately see which points need re-dragging.
                ImVec4 col = (e < 1.5f) ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f)
                           : (e < 5.0f) ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f)
                                        : ImVec4(1.0f, 0.35f, 0.35f, 1.0f);
                ImGui::TextColored(col, "%.2f px", e);
            } else {
                ImGui::TextDisabled("--");
            }

            ImGui::TableSetColumnIndex(4);
            ImGui::PushID(i);
            if (ImGui::SmallButton("Select")) activatePoint(i);
            ImGui::SameLine();
            if (ImGui::SmallButton("Del")) {
                m_points.erase(m_points.begin() + i);
                if (m_activePointIdx >= static_cast<int>(m_points.size()))
                    m_activePointIdx = static_cast<int>(m_points.size()) - 1;
                syncOverlayPoints();
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// ---------------------------------------------------------------------------
// Solve bar
// ---------------------------------------------------------------------------

void ProjectorCalibrationWindow::renderSolveBar() {
    auto& reg = m_engine->getRegistry();

    // Initial guess for the LM refiner: projector's current pose. This is the
    // key to robust convergence — the optimizer naturally lands near the
    // initial pose, which sidesteps the DLT/homography sign ambiguity that
    // makes the analytical solver unreliable.
    glm::vec3 initialPos(0.0f, 3.0f, 5.0f);
    glm::vec3 initialRot(-20.0f, 0.0f, 0.0f);
    float     initialFov = 50.0f;
    if (reg.valid(m_projectorEntity)) {
        if (const auto* p = reg.try_get<Projector>(m_projectorEntity)) {
            initialPos = glm::vec3(p->position[0], p->position[1], p->position[2]);
            initialRot = glm::vec3(p->rotation[0], p->rotation[1], p->rotation[2]);
            initialFov = p->fovDegrees;
        }
    }

    bool canSolve = m_points.size() >= 4;
    if (!canSolve) ImGui::BeginDisabled();
    if (ImGui::Button("Solve & Preview")) {
        std::vector<CorrespondencePair> pairs(m_points.begin(), m_points.end());
        m_lastResult  = CalibrationSolver::solveRefine(pairs, m_outputSize,
                                                        initialPos, initialRot, initialFov,
                                                        m_lockFov, m_lockDistortion);
        m_hasSolution = m_lastResult.success;
    }
    if (!canSolve) ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::Checkbox("Lock FOV", &m_lockFov);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Don't optimize the projector FOV — use the value\n"
                          "currently set on the Projector component (set it from\n"
                          "the manufacturer spec before solving).");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Lock Distortion", &m_lockDistortion);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Don't fit lens distortion (k1, k2). Useful when the projector\n"
                          "lens is a clean pinhole, or when you have few points (≤7) and\n"
                          "the per-point error column shows distortion is fitting noise\n"
                          "instead of real lens behavior.");
    }

    ImGui::SameLine();
    {
        auto* p = reg.try_get<Projector>(m_projectorEntity);
        const bool warpAvailable = p && p->isCalibrated && !p->calibrationPoints.empty();
        bool warp = p ? p->useResidualWarp : false;
        if (!warpAvailable) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Warp to Points", &warp) && p) {
            p->useResidualWarp = warp;
        }
        if (!warpAvailable) ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Treat every calibration point as ground truth and bend the\n"
                              "rendered output (inverse-distance-weighted residual warp) so\n"
                              "each crosshair lands EXACTLY where you placed it. Closes the\n"
                              "last-mile gap between the LM-fitted pose and pixel-perfect\n"
                              "alignment. Apply a solve first.");
        }
    }

    ImGui::SameLine();

    if (m_hasSolution) {
        float rms = m_lastResult.rmsErrorPixels;
        ImVec4 errCol = (rms < 3.0f) ? ImVec4(0.3f,1,0.3f,1)
                      : (rms < 10.0f) ? ImVec4(1,0.8f,0,1)
                                       : ImVec4(1,0.3f,0.3f,1);
        ImGui::TextColored(errCol, "RMS %.1f px  (via %s)",
                           rms,
                           m_lastResult.method == SolveMethod::Homography ? "Homography" : "DLT");
        ImGui::SameLine();
        if (ImGui::Button("Apply to Projector")) applyResult();
    } else if (!m_lastResult.errorMessage.empty()) {
        ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "Error: %s",
                           m_lastResult.errorMessage.c_str());
    }

    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 80.0f);
    if (ImGui::Button("Reset All")) {
        m_points.clear();
        m_activePointIdx = -1;
        m_hasPending     = false;
        m_hasSolution    = false;
        m_lastResult     = {};
        syncOverlayPoints();
    }
}

// ---------------------------------------------------------------------------
// Interaction helpers
// ---------------------------------------------------------------------------

bool ProjectorCalibrationWindow::pickMeshPoint(ImVec2 mousePos,
                                                ImVec2 panePos, ImVec2 paneSize,
                                                entt::entity screenEntity,
                                                glm::vec3& outWorldPos) {
    if (!m_engine || !m_engine->getRegistry().valid(screenEntity)) return false;

    auto& reg = m_engine->getRegistry();
    const auto* screen = reg.try_get<Screen>(screenEntity);
    if (!screen) return false;

    const MeshData* mesh = nullptr;
    if (reg.valid(screen->modelEntity)) {
        if (const auto* model = reg.try_get<Model>(screen->modelEntity))
            mesh = model->mesh.isValid() ? &model->mesh : nullptr;
    }
    if (!mesh) return false;

    // Match Stage3DRenderer::drawScreen exactly — including screenElevation Y offset.
    glm::vec3 pos(screen->position[0],
                  screen->position[1] + m_renderer.screenElevation,
                  screen->position[2]);
    glm::vec3 rot(screen->rotation[0], screen->rotation[1], screen->rotation[2]);
    glm::vec3 scl(screen->scale[0],    screen->scale[1],    screen->scale[2]);

    // Build screen model matrix (same convention as Stage3DRenderer::drawScreen)
    glm::mat4 model(1.0f);
    model = glm::translate(model, pos);
    model = glm::rotate(model, glm::radians(rot.y), glm::vec3(0,1,0));
    model = glm::rotate(model, glm::radians(rot.x), glm::vec3(1,0,0));
    model = glm::rotate(model, glm::radians(rot.z), glm::vec3(0,0,1));
    model = glm::scale(model, scl);

    glm::mat4 vp = m_sceneCamera.getViewProjectionMatrix();
    glm::mat4 mvp = vp * model;

    const auto& verts   = mesh->vertices;
    const auto& indices = mesh->indices;

    // Vertex snap (highest priority): mesh vertices are typically the
    // most-aligned features (corners, seams, edges). If the click is near a
    // vertex in screen space, snap directly to it and skip face/edge logic.
    const float kVertexSnapPx = 12.0f;
    {
        float bestVertDist = kVertexSnapPx;
        glm::vec3 bestVertLocal{0.0f};
        bool foundVert = false;
        for (const auto& v : verts) {
            glm::vec3 vp_local(v.position[0], v.position[1], v.position[2]);
            ImVec2 sp = projectPoint(vp_local, mvp, panePos, paneSize);
            if (sp.x < -1e8f) continue;
            float d = std::hypot(mousePos.x - sp.x, mousePos.y - sp.y);
            if (d < bestVertDist) {
                bestVertDist = d;
                bestVertLocal = vp_local;
                foundVert = true;
            }
        }
        if (foundVert) {
            outWorldPos = glm::vec3(model * glm::vec4(bestVertLocal, 1.0f));
            return true;
        }
    }

    for (size_t t = 0; t < indices.size(); t += 3) {
        const auto& v0 = verts[indices[t]];
        const auto& v1 = verts[indices[t+1]];
        const auto& v2 = verts[indices[t+2]];

        glm::vec3 p0(v0.position[0], v0.position[1], v0.position[2]);
        glm::vec3 p1(v1.position[0], v1.position[1], v1.position[2]);
        glm::vec3 p2(v2.position[0], v2.position[1], v2.position[2]);

        ImVec2 s0 = projectPoint(p0, mvp, panePos, paneSize);
        ImVec2 s1 = projectPoint(p1, mvp, panePos, paneSize);
        ImVec2 s2 = projectPoint(p2, mvp, panePos, paneSize);

        if (s0.x < -1e8f || s1.x < -1e8f || s2.x < -1e8f) continue;

        // Backface cull (same sign convention as Stage3DRenderer)
        float area = cross2D({s1.x - s0.x, s1.y - s0.y}, {s2.x - s0.x, s2.y - s0.y});
        if (area >= 0.0f) continue; // back face

        float u, v, w;
        if (!barycentricCoords2D(mousePos, s0, s1, s2, u, v, w)) continue;

        // Face hit point in local space.
        glm::vec3 localHit = u * p0 + v * p1 + w * p2;

        // Edge snap: if the click is within kSnapPx of an edge in screen space,
        // snap the local hit to the nearest point on that edge. Calibration
        // features are almost always corners or seams.
        const float kSnapPx = 8.0f;
        auto edgeDist = [&](ImVec2 a, ImVec2 b, float& outT) -> float {
            float dx = b.x - a.x, dy = b.y - a.y;
            float lenSq = dx*dx + dy*dy;
            if (lenSq < 1e-6f) { outT = 0.0f; return std::hypot(mousePos.x-a.x, mousePos.y-a.y); }
            float tParam = ((mousePos.x-a.x)*dx + (mousePos.y-a.y)*dy) / lenSq;
            tParam = std::clamp(tParam, 0.0f, 1.0f);
            outT = tParam;
            float cx = a.x + tParam*dx, cy = a.y + tParam*dy;
            return std::hypot(mousePos.x-cx, mousePos.y-cy);
        };
        float t01, t12, t20;
        float d01 = edgeDist(s0, s1, t01);
        float d12 = edgeDist(s1, s2, t12);
        float d20 = edgeDist(s2, s0, t20);
        float bestDist = kSnapPx; int bestEdge = -1; float bestT = 0.0f;
        if (d01 < bestDist) { bestDist = d01; bestEdge = 0; bestT = t01; }
        if (d12 < bestDist) { bestDist = d12; bestEdge = 1; bestT = t12; }
        if (d20 < bestDist) { bestDist = d20; bestEdge = 2; bestT = t20; }
        if (bestEdge >= 0) {
            glm::vec3 ea = (bestEdge == 0) ? p0 : (bestEdge == 1) ? p1 : p2;
            glm::vec3 eb = (bestEdge == 0) ? p1 : (bestEdge == 1) ? p2 : p0;
            localHit = ea + bestT * (eb - ea);
        }

        outWorldPos = glm::vec3(model * glm::vec4(localHit, 1.0f));
        return true;
    }
    return false;
}

glm::vec2 ProjectorCalibrationWindow::projectThroughProjector(const glm::vec3& worldPos) {
    glm::mat4 vp = m_projectorCamera.getViewProjectionMatrix();
    glm::vec4 clip = vp * glm::vec4(worldPos, 1.0f);
    if (clip.w < 1e-6f) return glm::vec2(0.5f, 0.5f);
    float x = std::clamp(clip.x / clip.w, -1.0f, 1.0f);
    float y = std::clamp(clip.y / clip.w, -1.0f, 1.0f);
    return glm::vec2((x + 1.0f) * 0.5f, (1.0f - y) * 0.5f);
}

void ProjectorCalibrationWindow::rebuildProjectorCamera(ImVec2 paneSize) {
    auto& reg = m_engine->getRegistry();
    if (!reg.valid(m_projectorEntity)) return;
    const auto& proj = reg.get<Projector>(m_projectorEntity);

    float aspect = paneSize.x / std::max(paneSize.y, 1.0f);
    m_projectorCamera = Stage3DRenderer::buildProjectorCamera(
        glm::vec3(proj.position[0], proj.position[1], proj.position[2]),
        glm::vec3(proj.rotation[0], proj.rotation[1], proj.rotation[2]),
        proj.fovDegrees, aspect,
        proj.nearClip, proj.farClip);
}

void ProjectorCalibrationWindow::persistPoints() {
    if (!m_engine) return;
    auto& reg = m_engine->getRegistry();
    if (!reg.valid(m_projectorEntity)) return;
    auto* p = reg.try_get<Projector>(m_projectorEntity);
    if (!p) return;

    p->calibrationPoints.clear();
    p->calibrationPoints.reserve(m_points.size());
    for (const auto& pt : m_points) {
        CalibrationPoint cp;
        cp.worldPos[0]    = pt.worldPos.x;
        cp.worldPos[1]    = pt.worldPos.y;
        cp.worldPos[2]    = pt.worldPos.z;
        cp.projectorUV[0] = pt.projectorUV.x;
        cp.projectorUV[1] = pt.projectorUV.y;
        cp.isAligned      = pt.isAligned;
        p->calibrationPoints.push_back(cp);
    }
}

void ProjectorCalibrationWindow::syncOverlayPoints() {
    // Editor-thread state mutation only — no D3D12 commands recorded here.
    // The show thread reads OutputDisplay::calibrationOverlay through the
    // SceneSnapshot and draws the overlay directly onto the projector output.
    if (!m_engine) return;
    auto& reg = m_engine->getRegistry();
    if (!reg.valid(m_routedOutputEntity)) return;
    auto* od = reg.try_get<OutputDisplay>(m_routedOutputEntity);
    if (!od) return;

    auto& ov = od->calibrationOverlay;
    ov.enabled         = true;
    ov.numPoints       = static_cast<int32_t>(
        std::min(m_points.size(), ov.points.size()));
    ov.activeIndex     = m_activePointIdx;
    ov.precisionCursor = m_precisionCursor;
    for (int i = 0; i < ov.numPoints; ++i)
        ov.points[i] = m_points[i].projectorUV;
}

void ProjectorCalibrationWindow::routeToOutput(entt::entity outputEntity) {
    // Restore previous output first
    unrouteOutput();

    auto& reg = m_engine->getRegistry();
    if (!reg.valid(outputEntity)) return;

    auto* od = reg.try_get<OutputDisplay>(outputEntity);
    if (!od) return;

    m_routedOutputEntity = outputEntity;
    m_savedSourceScreen  = od->sourceScreen;

    // Route the projector to this output: OutputManager will render the
    // projector's target screen content fullscreen to the physical display,
    // then draw the calibration crosshair overlay on top (show thread).
    od->sourceProjector = m_projectorEntity;
    od->sourceScreen    = entt::null;

    m_outputSize = glm::uvec2(od->width, od->height);
    syncOverlayPoints();
}

void ProjectorCalibrationWindow::unrouteOutput() {
    if (!m_engine || !m_engine->getRegistry().valid(m_routedOutputEntity)) {
        m_routedOutputEntity = entt::null;
        return;
    }
    // Restore saved source and clear projector link + overlay state.
    auto& reg = m_engine->getRegistry();
    if (auto* od = reg.try_get<OutputDisplay>(m_routedOutputEntity)) {
        od->sourceScreen                 = m_savedSourceScreen;
        od->sourceProjector              = entt::null;
        od->calibrationOverlay.enabled   = false;
        od->calibrationOverlay.numPoints = 0;
    }

    m_routedOutputEntity = entt::null;
    m_savedSourceScreen  = entt::null;
}

void ProjectorCalibrationWindow::activatePoint(int idx) {
    m_activePointIdx    = idx;
    m_rightPaneDragging = false;
    syncOverlayPoints();
}

void ProjectorCalibrationWindow::addCorrespondencePoint() {
    if (!m_hasPending) return;
    CorrespondencePointEx pt;
    pt.worldPos    = m_pendingWorldPos;
    pt.projectorUV = projectThroughProjector(m_pendingWorldPos); // initial guess
    pt.isAligned   = false;
    m_points.push_back(pt);
    m_hasPending     = false;
    activatePoint(static_cast<int>(m_points.size()) - 1);
}

void ProjectorCalibrationWindow::applyResult() {
    if (!m_hasSolution || !m_engine) return;
    auto& reg = m_engine->getRegistry();
    if (!reg.valid(m_projectorEntity)) return;
    auto& proj = reg.get<Projector>(m_projectorEntity);

    // Defensive: don't apply NaN/Inf if the LM output is somehow degenerate.
    const auto& r = m_lastResult;
    if (!std::isfinite(r.position.x) || !std::isfinite(r.position.y) || !std::isfinite(r.position.z) ||
        !std::isfinite(r.rotationEuler.x) || !std::isfinite(r.rotationEuler.y) || !std::isfinite(r.rotationEuler.z) ||
        !std::isfinite(r.fovDegrees) ||
        !std::isfinite(r.distortionK1) || !std::isfinite(r.distortionK2)) {
        std::cout << "[Calibration] Apply REJECTED — solver returned non-finite values\n";
        return;
    }

    const glm::vec3 oldPos(proj.position[0], proj.position[1], proj.position[2]);
    const glm::vec3 oldRot(proj.rotation[0], proj.rotation[1], proj.rotation[2]);
    const float     oldFov = proj.fovDegrees;
    const float     oldK1  = proj.distortionK1;
    const float     oldK2  = proj.distortionK2;

    proj.position[0] = r.position.x;
    proj.position[1] = r.position.y;
    proj.position[2] = r.position.z;
    proj.rotation[0] = r.rotationEuler.x;
    proj.rotation[1] = r.rotationEuler.y;
    proj.rotation[2] = r.rotationEuler.z;
    proj.fovDegrees  = std::clamp(r.fovDegrees, 5.0f, 170.0f);
    proj.distortionK1 = r.distortionK1;
    proj.distortionK2 = r.distortionK2;
    proj.isCalibrated = true;

    std::cout << "[Calibration] Apply  pos="
              << "(" << r.position.x << ", " << r.position.y << ", " << r.position.z << ")"
              << "  rot=(" << r.rotationEuler.x << ", " << r.rotationEuler.y << ", " << r.rotationEuler.z << ")"
              << "  fov=" << proj.fovDegrees
              << "  k1=" << r.distortionK1 << "  k2=" << r.distortionK2
              << "  | Δpos=(" << (r.position.x - oldPos.x)
              << ", " << (r.position.y - oldPos.y)
              << ", " << (r.position.z - oldPos.z) << ")"
              << "  Δrot=(" << (r.rotationEuler.x - oldRot.x)
              << ", " << (r.rotationEuler.y - oldRot.y)
              << ", " << (r.rotationEuler.z - oldRot.z) << ")"
              << "  Δfov=" << (proj.fovDegrees - oldFov)
              << "  Δk1=" << (r.distortionK1 - oldK1)
              << "  Δk2=" << (r.distortionK2 - oldK2)
              << "  RMS=" << r.rmsErrorPixels << "px\n";
}

} // namespace entity
