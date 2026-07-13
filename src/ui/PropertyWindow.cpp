/**
 * PropertyWindow Implementation
 *
 * Displays and allows editing of selected clip properties.
 */

#include "entity/ui/PropertyWindow.hpp"
#include "entity/ui/MathInput.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/timeline/CueTag.hpp"
#include "entity/core/Types.hpp"
#include "entity/components/Transform.hpp"
#include "entity/components/MediaLayer.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/ContentRouting.hpp"
#include "entity/components/ContentRoutingAsset.hpp"
#include "entity/components/ContentRoutingAssetOps.hpp"
#include "entity/components/ContentRoutingRef.hpp"
#include "entity/components/Layer.hpp"
#include "entity/components/ObjectAnimationLayer.hpp"
#include "entity/components/GenerativeLayer.hpp"
#include "entity/components/MunchersGameState.hpp"
#include "entity/components/SignalLayer.hpp"
#include "entity/components/TextLayerState.hpp"
#include "entity/components/AudioSource.hpp"
#include "entity/components/RemotePatch.hpp"
#include "entity/remote/RemoteControlStore.hpp"
#include "entity/render/TextRasterizer.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include "entity/components/ScaleLock.hpp"
#include "entity/components/Effect.hpp"
#include "entity/components/EffectAnimatedParameters.hpp"
#include "entity/components/EffectChain.hpp"
#include "entity/components/EffectParameters.hpp"
#include "entity/effects/EffectKindRegistry.hpp"
#include "entity/components/Screen.hpp"
#include "entity/components/Model.hpp"
#include "entity/components/Projector.hpp"
#include "entity/components/Prop.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/command/CommandDispatcher.hpp"
#include "entity/command/Commands.hpp"
#include "entity/core/Engine.hpp"
#include "entity/core/ClipPlaybackDiagnostics.hpp"
#include "entity/project/ProjectManager.hpp"
#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cctype>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace entity {

namespace {

// Map a clip entity back to (trackIndex, clipIndex) — the address commands
// use. Returns nullopt if the clip isn't actually on any track.
std::optional<std::pair<int, int>>
findClipIndices(Timeline* timeline, entt::entity clipEntity) {
    if (!timeline || clipEntity == entt::null) return std::nullopt;
    auto& registry = timeline->getRegistry();
    const auto& tracks = timeline->getTracks();
    for (size_t ti = 0; ti < tracks.size(); ++ti) {
        auto* track = registry.try_get<TimelineTrack>(tracks[ti]);
        if (!track) continue;
        for (size_t ci = 0; ci < track->layers.size(); ++ci) {
            if (track->layers[ci] == clipEntity) {
                return std::make_pair(static_cast<int>(ti), static_cast<int>(ci));
            }
        }
    }
    return std::nullopt;
}

// Mirrors the gate in PropertyWindow::updateKeyframeOnValueChange — the
// drag-side rewrite of keyframes only fires when (a) the property has
// existing keyframes and (b) the playhead is inside the clip. The
// drag-start snapshot must use the same condition or undo will try to
// restore a keyframe write that never happened.
bool clipFrameFromPlayhead(Timeline* timeline, entt::entity clipEntity, FrameNumber& outFrame) {
    if (!timeline || clipEntity == entt::null) return false;
    auto& registry = timeline->getRegistry();
    const auto* clip = registry.try_get<Clip>(clipEntity);
    if (!clip) return false;
    FrameNumber clipFrame = timeline->getCurrentFrame() - clip->startFrame;
    if (clipFrame < 0 || clipFrame >= clip->duration) return false;
    outFrame = clipFrame;
    return true;
}

} // namespace

PropertyWindow::PropertyWindow(Timeline* timeline)
    : m_timeline(timeline)
{
}

void PropertyWindow::render() {
    if (!m_timeline) {
        ImGui::Text("No timeline available");
        return;
    }

    auto& registry = m_timeline->getRegistry();

    // Cue selection wins when set — it's mutually exclusive with the
    // entt-based selections, so no need to dance around projector/screen.
    if (m_timeline->getSelectedCueNumber().has_value()) {
        renderCueProperties();
        return;
    }

    // Section-break selection: same idea as cues, mutually exclusive.
    if (m_timeline->getSelectedSectionBreak().has_value()) {
        renderSectionBreakProperties();
        return;
    }

    // Check for selected projector first (takes priority over screen/clip)
    entt::entity selectedProjector = m_timeline->getSelectedProjector();
    if (selectedProjector != entt::null && registry.valid(selectedProjector)) {
        renderProjectorProperties();
        return;
    }

    // Check for selected screen
    entt::entity selectedScreen = m_timeline->getSelectedScreen();
    if (selectedScreen != entt::null && registry.valid(selectedScreen)) {
        renderScreenProperties();
        return;
    }

    // Check for selected prop
    entt::entity selectedProp = m_timeline->getSelectedProp();
    if (selectedProp != entt::null && registry.valid(selectedProp)) {
        renderPropProperties();
        return;
    }

    // Check for selected clip
    entt::entity selectedClip = m_timeline->getSelectedClip();

    if (selectedClip == entt::null) {
        // No clip selected - show timeline properties
        renderTimelineProperties();
        return;
    }

    if (!registry.valid(selectedClip)) {
        ImGui::TextDisabled("Invalid selection");
        return;
    }

    // OA layer path: entity has ObjectAnimationLayer but no Clip component
    if (registry.all_of<ObjectAnimationLayer>(selectedClip) &&
        !registry.all_of<Clip>(selectedClip)) {
        renderOALayerProperties(selectedClip);
        return;
    }

    // Generative layer path: entity has GenerativeLayer but no Clip component
    if (registry.all_of<GenerativeLayer>(selectedClip) &&
        !registry.all_of<Clip>(selectedClip)) {
        renderGenerativeLayerProperties(selectedClip);
        return;
    }

    // Signal Output Layer path: entity has SignalLayer but no Clip component
    if (registry.all_of<SignalLayer>(selectedClip) &&
        !registry.all_of<Clip>(selectedClip)) {
        renderSignalLayerProperties(selectedClip);
        return;
    }

    // CRITICAL: Push unique ID for this clip to prevent ImGui widget state collision
    // Without this, all clips share the same widget IDs and state bleeds between them
    ImGui::PushID(static_cast<int>(selectedClip));

    // Show clip name at top
    const auto* clip = registry.try_get<Clip>(selectedClip);
    if (clip) {
        // Extract filename from path
        size_t lastSlash = clip->filepath.find_last_of("/\\");
        std::string filename = (lastSlash != std::string::npos)
            ? clip->filepath.substr(lastSlash + 1)
            : clip->filepath;
        ImGui::Text("Clip: %s", filename.c_str());
        ImGui::Separator();
    }

    // Transform section
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        renderTransformSection();
    }

    // Layer/Opacity section
    if (ImGui::CollapsingHeader("Layer", ImGuiTreeNodeFlags_DefaultOpen)) {
        renderLayerSection();
    }

    // Content Routing section — own header so it aligns with the canonical
    // section order across all content-layer kinds. The picker call handles
    // the combo, optimistic ContentRoutingRef write, kind badge, and
    // Edit... link; the post-change block dispatches SetClipTargetScreenCommand
    // for the legacy alias (undo/scripts, L1 only per ADR-0022).
    if (ImGui::CollapsingHeader("Content Routing", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto* clipRW = registry.try_get<Clip>(selectedClip);
        if (clipRW) {
            entt::entity prevLegacyScreen = clipRW->targetScreen;
            std::string prevName = "All Screens";
            if (prevLegacyScreen != entt::null) {
                if (auto* prev = registry.try_get<Screen>(prevLegacyScreen))
                    prevName = prev->name;
            }
            if (renderContentRoutingPicker(selectedClip, "contentRouting")) {
                const auto* newRef = registry.try_get<ContentRoutingRef>(selectedClip);
                const entt::entity newAsset = newRef ? newRef->asset : entt::null;
                const auto* newAssetData = (newAsset != entt::null)
                    ? registry.try_get<ContentRoutingAsset>(newAsset) : nullptr;
                entt::entity legacyScreen = entt::null;
                if (newAssetData &&
                    newAssetData->kind == RouteMode::Direct &&
                    newAssetData->targets.size() == 1) {
                    legacyScreen = newAssetData->targets.front().screen;
                }
                std::string newScreenName = "All Screens";
                if (legacyScreen != entt::null) {
                    if (auto* s = registry.try_get<Screen>(legacyScreen))
                        newScreenName = s->name;
                }
                clipRW->targetScreen = legacyScreen;
                std::cout << "[PropertyWindow] Content routing -> '"
                          << newScreenName << "'" << std::endl;
                if (m_dispatcher && newAssetData &&
                    newAssetData->kind == RouteMode::Direct) {
                    if (auto idx = findClipIndices(m_timeline, selectedClip)) {
                        auto cmd = std::make_unique<SetClipTargetScreenCommand>(
                            idx->first, idx->second, newScreenName);
                        cmd->setPreviousScreenName(prevName);
                        m_dispatcher->enqueue(std::move(cmd));
                    }
                }
            }
        }
    }

    // Playback section
    if (ImGui::CollapsingHeader("Playback", ImGuiTreeNodeFlags_DefaultOpen)) {
        renderPlaybackSection();
    }

    // Clip Info section
    if (ImGui::CollapsingHeader("Clip Info")) {
        renderClipInfo();
    }

    // Effects section (issue #54). Always rendered so the user can
    // discover the "Add Effect" affordance even when the chain is empty.
    if (ImGui::CollapsingHeader("Effects", ImGuiTreeNodeFlags_DefaultOpen)) {
        renderEffectsSection(selectedClip);
    }

    // Audio section — only shown for clips that carry audio. Sits after
    // Effects so the canonical order matches: kind-specific -> Effects ->
    // Audio -> Remote Control.
    {
        auto& reg = m_timeline->getRegistry();
        const auto* src = reg.try_get<AudioSource>(selectedClip);
        if (src && src->hasAudioStream) {
            if (ImGui::CollapsingHeader("Audio", ImGuiTreeNodeFlags_DefaultOpen)) {
                renderAudioSection(selectedClip);
            }
        }
    }

    // Remote Control section (ADR-0028). Shows Patch / ID / Armed /
    // live Engaged toggle + value readout for any content layer.
    if (ImGui::CollapsingHeader("Remote Control")) {
        renderRemoteControlSection(selectedClip);
    }

    // Pop the clip-specific ID
    ImGui::PopID();
}

void PropertyWindow::renderTransformSection() {
    entt::entity selectedClip = m_timeline->getSelectedClip();
    if (selectedClip == entt::null) return;

    auto& registry = m_timeline->getRegistry();
    auto* transform = registry.try_get<Transform>(selectedClip);

    if (!transform) {
        ImGui::TextDisabled("No transform component");
        return;
    }

    // Position X with keyframe controls
    renderKeyframeControls(AnimatableProperty::PositionX, "Position X", transform->position.x);
    ImGui::SameLine();
    ImGui::Text("Position X");
    ImGui::SetNextItemWidth(-1);
    float posX = transform->position.x;
    if (entity::ui::DragFloat("##posX", &posX, 0.01f, -2.0f, 2.0f, "%.3f")) {
        transform->setPosition(glm::vec3(posX, transform->position.y, transform->position.z));
        updateKeyframeOnValueChange(AnimatableProperty::PositionX, posX);
    }

    // Position Y with keyframe controls
    renderKeyframeControls(AnimatableProperty::PositionY, "Position Y", transform->position.y);
    ImGui::SameLine();
    ImGui::Text("Position Y");
    ImGui::SetNextItemWidth(-1);
    float posY = transform->position.y;
    if (entity::ui::DragFloat("##posY", &posY, 0.01f, -2.0f, 2.0f, "%.3f")) {
        transform->setPosition(glm::vec3(transform->position.x, posY, transform->position.z));
        updateKeyframeOnValueChange(AnimatableProperty::PositionY, posY);
    }

    ImGui::Spacing();

    // Rotation Z with keyframe controls
    renderKeyframeControls(AnimatableProperty::Rotation, "Rotation", transform->rotation.z);
    ImGui::SameLine();
    ImGui::Text("Rotation");
    ImGui::SetNextItemWidth(-1);
    float rotZ = transform->rotation.z;
    bool rotChanged = entity::ui::DragFloat("##rotZ", &rotZ, 0.5f, -360.0f, 360.0f, "%.1f deg");
    if (ImGui::IsItemActivated()) {
        m_preEditRotZ.scalarValue = transform->rotation.z;
        m_preEditRotZ.wasKeyframed = false;
        m_preEditRotZ.keyframeValue.reset();
        auto* ap = registry.try_get<AnimatedProperties>(selectedClip);
        FrameNumber clipFrame;
        if (ap && ap->getTrack(AnimatableProperty::Rotation) &&
            ap->getTrack(AnimatableProperty::Rotation)->hasKeyframes() &&
            clipFrameFromPlayhead(m_timeline, selectedClip, clipFrame)) {
            m_preEditRotZ.wasKeyframed = true;
            m_preEditRotZ.keyframeFrame = clipFrame;
            const Keyframe* existing = ap->getTrack(AnimatableProperty::Rotation)->getKeyframeAt(clipFrame);
            m_preEditRotZ.keyframeValue = existing ? std::optional<float>(existing->value) : std::nullopt;
        }
    }
    if (rotChanged) {
        transform->setRotation(glm::vec3(transform->rotation.x, transform->rotation.y, rotZ));
        updateKeyframeOnValueChange(AnimatableProperty::Rotation, rotZ);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && m_dispatcher) {
        if (auto idx = findClipIndices(m_timeline, selectedClip)) {
            if (m_preEditRotZ.wasKeyframed) {
                auto cmd = std::make_unique<UpsertKeyframeCommand>(
                    idx->first, idx->second,
                    AnimatableProperty::Rotation,
                    m_preEditRotZ.keyframeFrame,
                    transform->rotation.z);
                cmd->setPreviousValue(m_preEditRotZ.keyframeValue);
                m_dispatcher->enqueue(std::move(cmd));
            } else {
                auto cmd = std::make_unique<SetClipRotationCommand>(
                    idx->first, idx->second,
                    transform->rotation.x, transform->rotation.y, transform->rotation.z);
                cmd->setPreviousRotation(transform->rotation.x, transform->rotation.y, m_preEditRotZ.scalarValue);
                m_dispatcher->enqueue(std::move(cmd));
            }
        }
    }

    ImGui::Spacing();

    // Scale section
    ImGui::Text("Scale");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Scale factor. 1.0 = fullscreen, 0.5 = half size");
    }

    // Uniform scale lock lives on the entity (ScaleLock) so the timeline's
    // twirl-down scale rows honor the same flag and it survives a project reload.
    bool uniformScale = registry.get_or_emplace<ScaleLock>(selectedClip).uniform;
    if (ImGui::Checkbox("Uniform Scale", &uniformScale) && m_dispatcher) {
        m_dispatcher->enqueue(
            std::make_unique<SetScaleLockCommand>(selectedClip, uniformScale));
    }

    // Scale X with keyframe controls
    renderKeyframeControls(AnimatableProperty::ScaleX, "Scale X", transform->scale.x);
    ImGui::SameLine();
    ImGui::Text("Scale X");
    ImGui::SetNextItemWidth(-1);
    float scaleX = transform->scale.x;
    float prevScaleX = scaleX;
    if (entity::ui::DragFloat("##scaleX", &scaleX, 0.01f, 0.01f, 10.0f, "%.3f")) {
        if (uniformScale && prevScaleX > 0.0001f) {
            float ratio = scaleX / prevScaleX;
            float newScaleY = transform->scale.y * ratio;
            transform->setScale(glm::vec3(scaleX, newScaleY, transform->scale.z * ratio));
            updateKeyframeOnValueChange(AnimatableProperty::ScaleX, scaleX);
            updateKeyframeOnValueChange(AnimatableProperty::ScaleY, newScaleY);
        } else {
            transform->setScale(glm::vec3(scaleX, transform->scale.y, transform->scale.z));
            updateKeyframeOnValueChange(AnimatableProperty::ScaleX, scaleX);
        }
    }

    // Scale Y with keyframe controls
    renderKeyframeControls(AnimatableProperty::ScaleY, "Scale Y", transform->scale.y);
    ImGui::SameLine();
    ImGui::Text("Scale Y");
    ImGui::SetNextItemWidth(-1);
    float scaleY = transform->scale.y;
    float prevScaleY = scaleY;
    if (entity::ui::DragFloat("##scaleY", &scaleY, 0.01f, 0.01f, 10.0f, "%.3f")) {
        if (uniformScale && prevScaleY > 0.0001f) {
            float ratio = scaleY / prevScaleY;
            float newScaleX = transform->scale.x * ratio;
            transform->setScale(glm::vec3(newScaleX, scaleY, transform->scale.z * ratio));
            updateKeyframeOnValueChange(AnimatableProperty::ScaleX, newScaleX);
            updateKeyframeOnValueChange(AnimatableProperty::ScaleY, scaleY);
        } else {
            transform->setScale(glm::vec3(transform->scale.x, scaleY, transform->scale.z));
            updateKeyframeOnValueChange(AnimatableProperty::ScaleY, scaleY);
        }
    }

    ImGui::Spacing();

    // Reset button
    if (ImGui::Button("Reset Transform")) {
        transform->setPosition(glm::vec3(0.0f));
        transform->setRotation(glm::vec3(0.0f));
        transform->setScale(glm::vec3(1.0f));
    }
}

void PropertyWindow::renderLayerSection() {
    entt::entity selectedClip = m_timeline->getSelectedClip();
    if (selectedClip == entt::null) return;

    auto& registry = m_timeline->getRegistry();
    auto* layer = registry.try_get<MediaLayer>(selectedClip);

    if (!layer) {
        ImGui::TextDisabled("No layer component");
        return;
    }

    // Opacity with keyframe controls
    renderKeyframeControls(AnimatableProperty::Opacity, "Opacity", layer->opacity);
    ImGui::SameLine();
    ImGui::Text("Opacity");
    ImGui::SetNextItemWidth(-1);
    float opacity = layer->opacity;
    bool opacityChanged = entity::ui::SliderFloat("##opacity", &opacity, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemActivated()) {
        m_preEditOpacity.scalarValue = layer->opacity;
        m_preEditOpacity.wasKeyframed = false;
        m_preEditOpacity.keyframeValue.reset();
        auto* ap = registry.try_get<AnimatedProperties>(selectedClip);
        FrameNumber clipFrame;
        if (ap && ap->getTrack(AnimatableProperty::Opacity) &&
            ap->getTrack(AnimatableProperty::Opacity)->hasKeyframes() &&
            clipFrameFromPlayhead(m_timeline, selectedClip, clipFrame)) {
            m_preEditOpacity.wasKeyframed = true;
            m_preEditOpacity.keyframeFrame = clipFrame;
            const Keyframe* existing = ap->getTrack(AnimatableProperty::Opacity)->getKeyframeAt(clipFrame);
            m_preEditOpacity.keyframeValue = existing ? std::optional<float>(existing->value) : std::nullopt;
        }
    }
    if (opacityChanged) {
        layer->opacity = opacity;
        updateKeyframeOnValueChange(AnimatableProperty::Opacity, opacity);
    }
    // On drag end: emit either a keyframe-upsert (when the drag rewrote a
    // keyframe) or a scalar-opacity command. The scalar command is useless
    // on keyframed properties because AnimationSystem would overwrite
    // layer->opacity next tick from the (now-reverted-to-pre-drag) keyframe.
    if (ImGui::IsItemDeactivatedAfterEdit() && m_dispatcher) {
        if (auto idx = findClipIndices(m_timeline, selectedClip)) {
            if (m_preEditOpacity.wasKeyframed) {
                auto cmd = std::make_unique<UpsertKeyframeCommand>(
                    idx->first, idx->second,
                    AnimatableProperty::Opacity,
                    m_preEditOpacity.keyframeFrame,
                    layer->opacity);
                cmd->setPreviousValue(m_preEditOpacity.keyframeValue);
                m_dispatcher->enqueue(std::move(cmd));
            } else {
                auto cmd = std::make_unique<SetClipOpacityCommand>(
                    idx->first, idx->second, layer->opacity);
                cmd->setPreviousOpacity(m_preEditOpacity.scalarValue);
                m_dispatcher->enqueue(std::move(cmd));
            }
        }
    }

    // Visibility toggle
    ImGui::Checkbox("Visible", &layer->visible);

    // Z-Order (read-only for now, determined by track)
    ImGui::Text("Z-Order: %d", layer->zOrder);
    ImGui::SameLine();
    ImGui::TextDisabled("(determined by track)");

    // Blend mode combo. Order must match the BlendMode enum in
    // include/entity/core/Types.hpp; the combo index is static_cast'd straight
    // to BlendMode.
    static const char* blendModes[] = {
        "Normal", "Add", "Multiply", "Screen", "Overlay",
        "Soft Light", "Hard Light", "Color Dodge", "Color Burn",
        "Darken", "Lighten", "Difference", "Exclusion"
    };
    int currentBlendMode = static_cast<int>(layer->blendMode);
    ImGui::Text("Blend Mode");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##blendmode", &currentBlendMode, blendModes, IM_ARRAYSIZE(blendModes))) {
        BlendMode prev = layer->blendMode;
        BlendMode next = static_cast<BlendMode>(currentBlendMode);
        layer->blendMode = next;
        if (m_dispatcher) {
            if (auto idx = findClipIndices(m_timeline, selectedClip)) {
                auto cmd = std::make_unique<SetClipBlendModeCommand>(idx->first, idx->second, next);
                cmd->setPreviousMode(prev);
                m_dispatcher->enqueue(std::move(cmd));
            }
        }
    }
}

bool PropertyWindow::renderContentRoutingPicker(entt::entity entity, const char* idSuffix) {
    // Content Routing — Plane A library binding (ADR-0022). Replaces the
    // legacy "Target Screen" dropdown. The dropdown surfaces every
    // ContentRoutingAsset in the library, with the leading "Default (All
    // visible)" sentinel for layers that should render to every screen.
    // Auto-direct assets list first (one per Screen, alphabetical by
    // Screen name), then user-created routings (alphabetical by name).
    auto& registry = m_timeline->getRegistry();

    ImGui::Spacing();
    ImGui::Text("Content Routing");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Select a routing from the Content Routing library.\n"
                          "Default routes the clip to all visible screens.\n"
                          "Each Screen auto-generates a Direct routing; create\n"
                          "Tiled / Feed Map routings in the Content Routing window.");
    }

    // Build (entity, displayName) pairs. Auto-direct first, then user.
    struct AssetPick { entt::entity asset; std::string display; };
    std::vector<AssetPick> picks;
    picks.push_back({entt::null, "Default (All visible)"});

    std::vector<AssetPick> autoDirect;
    std::vector<AssetPick> userCreated;
    for (auto [aEntity, asset] : registry.view<ContentRoutingAsset>().each()) {
        AssetPick p{aEntity, asset.name};
        if (asset.autoBoundScreen != entt::null) autoDirect.push_back(std::move(p));
        else userCreated.push_back(std::move(p));
    }
    auto byName = [](const AssetPick& a, const AssetPick& b) { return a.display < b.display; };
    std::sort(autoDirect.begin(), autoDirect.end(), byName);
    std::sort(userCreated.begin(), userCreated.end(), byName);
    picks.insert(picks.end(), autoDirect.begin(), autoDirect.end());
    picks.insert(picks.end(), userCreated.begin(), userCreated.end());

    std::vector<const char*> pickNameCstrs;
    pickNameCstrs.reserve(picks.size());
    for (const auto& p : picks) pickNameCstrs.push_back(p.display.c_str());

    const auto* currentRef = registry.try_get<ContentRoutingRef>(entity);
    const entt::entity currentAsset = currentRef ? currentRef->asset : entt::null;

    int currentIdx = 0;
    for (size_t i = 0; i < picks.size(); ++i) {
        if (picks[i].asset == currentAsset) {
            currentIdx = static_cast<int>(i);
            break;
        }
    }

    // Combo ID is "##" + idSuffix to avoid widget-ID collision between
    // the clip call site ("contentRouting") and the generative call site
    // ("gencontentrouting").
    std::string comboId = std::string("##") + idSuffix;
    ImGui::SetNextItemWidth(-1);
    bool changed = ImGui::Combo(comboId.c_str(), &currentIdx,
                                pickNameCstrs.data(),
                                static_cast<int>(pickNameCstrs.size()));
    if (changed) {
        const entt::entity newAsset = picks[currentIdx].asset;
        // Optimistic ref write so the next snapshot bake sees coherent state
        // before the dispatcher drains (clip call site enqueues the legacy
        // command after this returns).
        auto& ref = registry.get_or_emplace<ContentRoutingRef>(entity);
        ref.asset = newAsset;
    }

    // Kind badge + edit link.
    {
        const auto* asset = routing::tryGetAsset(registry, entity);
        const char* kindLabel = "Default (All)";
        ImVec4 kindColor{0.7f, 0.7f, 0.7f, 1.0f};
        if (asset) {
            switch (asset->kind) {
                case RouteMode::Direct:
                    kindLabel = "Direct";
                    kindColor = ImVec4(0.6f, 0.85f, 0.6f, 1.0f);
                    break;
                case RouteMode::Tiled:
                    kindLabel = "Tiled";
                    kindColor = ImVec4(1.0f, 0.8f, 0.2f, 1.0f);
                    break;
                case RouteMode::FeedMap:
                    kindLabel = "Feed Map";
                    kindColor = ImVec4(0.6f, 0.7f, 1.0f, 1.0f);
                    break;
            }
        }
        ImGui::TextColored(kindColor, "%s", kindLabel);
        ImGui::SameLine();
        std::string editBtnId = std::string("Edit...##") + idSuffix;
        if (ImGui::SmallButton(editBtnId.c_str())) {
            ImGui::SetWindowFocus("Content Routing");
        }
    }

    return changed;
}

void PropertyWindow::renderPlaybackSection() {
    entt::entity selectedClip = m_timeline->getSelectedClip();
    if (selectedClip == entt::null) return;

    auto& registry = m_timeline->getRegistry();
    auto* clip = registry.try_get<Clip>(selectedClip);

    if (!clip) {
        ImGui::TextDisabled("No clip component");
        return;
    }

    // Playback mode dropdown
    ImGui::Text("Extend Mode");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Determines what happens when clip is extended\nbeyond its source media duration.\n\n"
                          "Freeze: Hold on last frame\n"
                          "Loop: Restart from beginning\n"
                          "Ping-Pong: Play forward then backward");
    }

    static const char* playbackModes[] = { "Freeze", "Loop", "Ping-Pong" };
    int currentMode = static_cast<int>(clip->playbackMode);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##playbackMode", &currentMode, playbackModes, IM_ARRAYSIZE(playbackModes))) {
        PlaybackMode prev = clip->playbackMode;
        PlaybackMode next = static_cast<PlaybackMode>(currentMode);
        clip->playbackMode = next;
        if (m_dispatcher) {
            if (auto idx = findClipIndices(m_timeline, selectedClip)) {
                auto cmd = std::make_unique<SetClipPlaybackModeCommand>(idx->first, idx->second, next);
                cmd->setPreviousMode(prev);
                m_dispatcher->enqueue(std::move(cmd));
            }
        }
    }

    // Media — pick a different source file for this clip from the
    // project media library. Hidden when no Engine pointer is wired
    // (headless tests) or the library is empty.
    //
    // Custom BeginCombo (instead of plain ImGui::Combo) so we can host an
    // InputTextWithHint filter inside the dropdown — typing narrows the
    // list as the user types. Case-insensitive substring match on the
    // filename leaf.
    if (m_engine && m_engine->getProjectManager()) {
        const auto& mediaFiles = m_engine->getLoadedMediaFiles();
        if (!mediaFiles.empty()) {
            ImGui::Spacing();
            ImGui::Text("Media");

            struct MediaPick {
                std::string display;
                std::string path;
            };
            std::vector<MediaPick> picks;
            picks.reserve(mediaFiles.size());
            for (const auto& entry : mediaFiles) {
                std::filesystem::path p{entry.originalPath};
                picks.push_back({p.filename().string(), entry.originalPath});
            }
            std::sort(picks.begin(), picks.end(),
                       [](const MediaPick& a, const MediaPick& b) {
                           return a.display < b.display;
                       });

            // Preview = leaf of current clip filepath, or "(none)".
            std::string previewLabel = "(none)";
            for (const auto& p : picks) {
                if (p.path == clip->filepath) {
                    previewLabel = p.display;
                    break;
                }
            }

            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##clipMedia", previewLabel.c_str())) {
                // Auto-focus the filter when the dropdown opens so the
                // user can just start typing.
                if (ImGui::IsWindowAppearing()) {
                    ImGui::SetKeyboardFocusHere();
                    m_mediaFilterBuf[0] = '\0';
                }

                ImGui::SetNextItemWidth(-1);
                ImGui::InputTextWithHint("##mediaFilter", "Search...",
                                          m_mediaFilterBuf,
                                          sizeof(m_mediaFilterBuf));

                ImGui::Separator();

                auto icontains = [](std::string_view hay, std::string_view needle) {
                    if (needle.empty()) return true;
                    if (needle.size() > hay.size()) return false;
                    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
                        bool match = true;
                        for (size_t j = 0; j < needle.size(); ++j) {
                            if (std::tolower(static_cast<unsigned char>(hay[i + j])) !=
                                std::tolower(static_cast<unsigned char>(needle[j]))) {
                                match = false;
                                break;
                            }
                        }
                        if (match) return true;
                    }
                    return false;
                };

                const std::string_view filter{m_mediaFilterBuf};
                int visibleCount = 0;
                if (ImGui::BeginChild("##mediaList", ImVec2(0, 220), false,
                                       ImGuiWindowFlags_HorizontalScrollbar)) {
                    for (const auto& p : picks) {
                        if (!icontains(p.display, filter)) continue;
                        ++visibleCount;
                        const bool isCurrent = (p.path == clip->filepath);
                        if (ImGui::Selectable(p.display.c_str(), isCurrent)) {
                            if (!isCurrent) {
                                if (auto idx = findClipIndices(m_timeline, selectedClip)) {
                                    if (m_dispatcher) {
                                        m_dispatcher->enqueue(std::make_unique<SetClipMediaCommand>(
                                            idx->first, idx->second, p.path));
                                    }
                                }
                            }
                            ImGui::CloseCurrentPopup();
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", p.path.c_str());
                        }
                    }
                    if (visibleCount == 0) {
                        ImGui::TextDisabled("(no matches)");
                    }
                }
                ImGui::EndChild();

                ImGui::EndCombo();
            }
        }
    }

    // Show source vs timeline duration info
    ImGui::Spacing();
    ImGui::Text("Source Frames: %d", clip->totalMediaFrames);
    ImGui::Text("Timeline Frames: %d", clip->duration);

    // Show if clip is extended
    if (clip->duration > clip->totalMediaFrames && clip->totalMediaFrames > 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Clip extended beyond source");
        FrameNumber extraFrames = clip->duration - clip->totalMediaFrames;
        ImGui::Text("Extra frames: %d", extraFrames);
    }

    // Section behavior policy. Normal: clip continues per its PlaybackMode
    // at section breaks (Loop / PingPong wraparound keeps cycling while the
    // playhead is parked). Locked: clip freezes with the playhead at any
    // break, regardless of PlaybackMode.
    ImGui::Spacing();
    ImGui::Text("Section Behavior");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Behavior at section breaks.\n\n"
                          "Normal: clip continues per its PlaybackMode at section breaks.\n"
                          "Locked: clip freezes with the playhead at any break.");
    }
    static const char* sectionBehaviorNames[] = { "Normal", "Locked" };
    int currentBehavior = static_cast<int>(clip->sectionBehavior);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##sectionBehavior", &currentBehavior, sectionBehaviorNames, IM_ARRAYSIZE(sectionBehaviorNames))) {
        SectionBehavior prev = clip->sectionBehavior;
        SectionBehavior next = static_cast<SectionBehavior>(currentBehavior);
        clip->sectionBehavior = next;
        if (m_dispatcher) {
            if (auto idx = findClipIndices(m_timeline, selectedClip)) {
                auto cmd = std::make_unique<SetClipSectionBehaviorCommand>(
                    idx->first, idx->second, next);
                cmd->setPreviousBehavior(prev);
                m_dispatcher->enqueue(std::move(cmd));
            }
        }
    }
}

void PropertyWindow::renderClipInfo() {
    entt::entity selectedClip = m_timeline->getSelectedClip();
    if (selectedClip == entt::null) return;

    auto& registry = m_timeline->getRegistry();
    auto* clip = registry.try_get<Clip>(selectedClip);

    if (!clip) {
        ImGui::TextDisabled("No clip component");
        return;
    }

    // File path
    ImGui::Text("File:");
    ImGui::TextWrapped("%s", clip->filepath.c_str());

    ImGui::Separator();

    // Timing — read-only context lines (timeline footprint, source rate).
    ImGui::Text("Timeline Start: %d", clip->startFrame);
    ImGui::Text("Frame Rate: %.2f fps", clip->framerate);

    // In-point (mediaStartFrame) — slip edit. Leaves duration alone, so
    // the timeline footprint stays put; the source-window slides. Capped
    // at totalMediaFrames - 1 (>= 0). totalMediaFrames is in source frames.
    const int totalMediaInt = static_cast<int>(clip->totalMediaFrames);
    const int inPointMin = 0;
    const int inPointMax = totalMediaInt > 0 ? totalMediaInt - 1 : 0;
    int inPoint = static_cast<int>(clip->mediaStartFrame);
    bool inPointChanged = entity::ui::DragInt("In Point", &inPoint, 1.0f, inPointMin, inPointMax,
                                         "%d frames", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemActivated()) {
        m_preEditMediaStartFrame = clip->mediaStartFrame;
    }
    if (inPointChanged) {
        clip->mediaStartFrame = static_cast<FrameNumber>(inPoint);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && m_dispatcher) {
        if (auto idx = findClipIndices(m_timeline, selectedClip)) {
            auto cmd = std::make_unique<SetClipMediaStartFrameCommand>(
                idx->first, idx->second, clip->mediaStartFrame);
            cmd->setPreviousMediaStartFrame(m_preEditMediaStartFrame);
            m_dispatcher->enqueue(std::move(cmd));
        }
    }

    // Out-point — explicit source-frame index (INCLUSIVE — the last frame
    // played, matching the standard NLE / video-editing convention).
    // Decoupled from `duration`: editing it does NOT change the clip's
    // timeline footprint. Past the out-point, the clip's playbackMode
    // (Freeze / Loop / PingPong) decides what plays inside the remaining
    // timeline frames. Clamps to [mediaStartFrame, totalMediaFrames - 1].
    const int outPointMin = static_cast<int>(clip->mediaStartFrame);
    const int outPointMax = totalMediaInt > 0 ? totalMediaInt - 1 : outPointMin;
    // Display value: mediaOutFrame if set, else last source frame (the
    // sentinel -1 case for clips that haven't had an explicit out-point
    // assigned yet — typical for fresh imports / pre-migration projects).
    int outPoint = clip->mediaOutFrame >= clip->mediaStartFrame
        ? static_cast<int>(clip->mediaOutFrame)
        : outPointMax;
    if (outPoint < outPointMin) outPoint = outPointMin;
    if (outPoint > outPointMax) outPoint = outPointMax;
    bool outPointChanged = entity::ui::DragInt("Out Point", &outPoint, 1.0f, outPointMin, outPointMax,
                                          "%d frames", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemActivated()) {
        m_preEditMediaOutFrame = clip->mediaOutFrame;
    }
    if (outPointChanged) {
        clip->mediaOutFrame = static_cast<FrameNumber>(outPoint);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && m_dispatcher) {
        if (auto idx = findClipIndices(m_timeline, selectedClip)) {
            auto cmd = std::make_unique<SetClipMediaOutFrameCommand>(
                idx->first, idx->second, clip->mediaOutFrame);
            cmd->setPreviousMediaOutFrame(m_preEditMediaOutFrame);
            m_dispatcher->enqueue(std::move(cmd));
        }
    }

    // Duration — clip's timeline footprint in timeline frames. Editable here
    // and via right-edge trim drag on the timeline.
    int duration = static_cast<int>(clip->duration);
    bool durationChanged = entity::ui::DragInt("Duration", &duration, 1.0f, 1,
                                               std::numeric_limits<int>::max(),
                                               "%d frames", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemActivated()) {
        m_preEditDuration = clip->duration;
    }
    if (durationChanged) {
        clip->duration = static_cast<FrameNumber>(duration);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && m_dispatcher) {
        if (auto idx = findClipIndices(m_timeline, selectedClip)) {
            auto cmd = std::make_unique<SetClipDurationCommand>(
                idx->first, idx->second, clip->duration);
            cmd->setPreviousDuration(m_preEditDuration);
            m_dispatcher->enqueue(std::move(cmd));
        }
    }

    // Duration in seconds (informational).
    float durationSec = static_cast<float>(clip->duration) / static_cast<float>(clip->framerate);
    ImGui::Text("(%.2f seconds)", durationSec);

    ImGui::Separator();

    // Media info
    ImGui::Text("Resolution: %dx%d", clip->width, clip->height);
    ImGui::Text("Has Alpha: %s", clip->hasAlpha ? "Yes" : "No");

    // Media type (use helper function from Types.hpp)
    ImGui::Text("Media Type: %s", MediaTypeToString(clip->mediaType));

    renderClipPlaybackReadout(selectedClip, *clip);
}

// Live transport readout for the selected clip. The point of this panel is
// the Mapped-vs-Presented pair: "Mapped" is the source frame the engine has
// decided the clip should be showing right now, "Presented" is the one that
// actually reached its GPU texture. They track each other in steady state.
// When they diverge the picture is frozen (or stale) even though every
// upstream number keeps advancing — which is invisible from the mapped frame
// alone, and is the failure this readout exists to make legible.
void PropertyWindow::renderClipPlaybackReadout(entt::entity clipEntity,
                                               const Clip& clip) {
    ImGui::Separator();
    ImGui::TextDisabled("Playback (live)");

    if (!m_engine) {
        ImGui::TextDisabled("(engine not bound)");
        return;
    }

    const ClipPlaybackDiagnostics d =
        gatherClipPlaybackDiagnostics(*m_engine, clipEntity, clip);

    ImGui::Text("Timeline: %lld  (clip-local %lld)",
                static_cast<long long>(d.timelineFrame),
                static_cast<long long>(d.localFrame));

    // Mapped - the source frame the engine wants on screen.
    if (d.mapped >= 0) {
        ImGui::Text("Mapped:   %lld / %lld",
                    static_cast<long long>(d.mapped),
                    static_cast<long long>(d.sourceLength));
    } else {
        ImGui::TextDisabled("Mapped:   n/a");
    }

    // Presented - the frame that actually reached the texture. Divergence from
    // Mapped is the frozen-picture signature; a small lag is normal (see
    // ClipPlaybackDiagnostics::kStaleLagFrames).
    if (!d.everPresented()) {
        ImGui::TextDisabled("Presented: (never uploaded)");
    } else if (d.stale()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
                           "Presented: %lld / %lld   STALE (%+lld)",
                           static_cast<long long>(d.presented),
                           static_cast<long long>(d.sourceLength),
                           static_cast<long long>(-d.lag()));
    } else {
        ImGui::Text("Presented: %lld / %lld   (lag %+lld)%s",
                    static_cast<long long>(d.presented),
                    static_cast<long long>(d.sourceLength),
                    static_cast<long long>(-d.lag()),
                    d.clipActive ? "" : "  [inactive]");
    }

    // Decoder + cache. If Presented is stuck, these say why: either the worker
    // isn't being steered toward the mapped frame, or it is and the frame just
    // isn't in the cache yet.
    if (d.hasWorker) {
        ImGui::Text("Decoder:  decoded %lld  target %lld%s",
                    static_cast<long long>(d.decoderFrame),
                    static_cast<long long>(d.decoderTarget),
                    d.seeking ? "  (seeking)" : "");
    } else {
        ImGui::TextDisabled("Decoder:  (no worker)");
    }
    if (d.mapped >= 0) {
        ImGui::Text("Cache:    mapped frame %s", d.cacheHit ? "HIT" : "MISS");
    }

    // Section-break continuation. While parked at a break a Normal clip keeps
    // walking sourcePhaseFrames in wall-clock; a Locked one holds.
    if (d.inContinuation) {
        ImGui::Text("Continuation: ON  phase %.1f", d.sourcePhaseFrames);
    } else {
        ImGui::TextDisabled("Continuation: off");
    }

    ImGui::Text("Extend: %s   Section: %s",
                clip.playbackMode == PlaybackMode::Freeze ? "Freeze"
                    : clip.playbackMode == PlaybackMode::Loop ? "Loop" : "Ping-Pong",
                clip.sectionBehavior == SectionBehavior::Locked ? "Locked" : "Normal");
}

void PropertyWindow::renderEffectsSection(entt::entity layerEntity) {
    if (!m_timeline || layerEntity == entt::null) return;
    auto& registry = m_timeline->getRegistry();
    if (!registry.valid(layerEntity)) return;

    // "Add Effect" button. Opens a popup populated from the registry.
    if (ImGui::Button("+ Add Effect", ImVec2(-1, 0))) {
        ImGui::OpenPopup("AddEffectPopup");
    }
    if (ImGui::BeginPopup("AddEffectPopup")) {
        if (!m_effectKindRegistry) {
            ImGui::TextDisabled("(EffectKindRegistry not bound)");
        } else {
            // Group by category, stable order.
            const auto& kinds = m_effectKindRegistry->kinds();
            std::vector<const effects::EffectKind*> sorted;
            sorted.reserve(kinds.size());
            for (const auto& [_, k] : kinds) sorted.push_back(&k);
            std::sort(sorted.begin(), sorted.end(),
                [](const effects::EffectKind* a, const effects::EffectKind* b) {
                    if (a->category != b->category) return a->category < b->category;
                    return a->displayName < b->displayName;
                });

            std::string lastCategory;
            for (const effects::EffectKind* k : sorted) {
                if (k->category != lastCategory) {
                    if (!lastCategory.empty()) ImGui::Separator();
                    ImGui::TextDisabled("%s", k->category.c_str());
                    lastCategory = k->category;
                }
                if (ImGui::MenuItem(k->displayName.c_str())) {
                    if (m_dispatcher) {
                        m_dispatcher->enqueue(std::make_unique<AddEffectCommand>(
                            layerEntity, k->stableId));
                    }
                }
            }
            if (sorted.empty()) {
                ImGui::TextDisabled("(no effect kinds registered)");
            }
        }
        ImGui::EndPopup();
    }

    auto* chain = registry.try_get<EffectChain>(layerEntity);
    if (!chain || chain->nodes.empty()) {
        ImGui::TextDisabled("No effects on this layer.");
        return;
    }

    ImGui::Spacing();

    // Walk the chain and render one collapsible header per effect.
    // Iterating by index so we can capture entities for delete commands
    // without invalidating during the render pass — commands enqueue
    // and apply on the next dispatcher tick.
    for (std::size_t i = 0; i < chain->nodes.size(); ++i) {
        const entt::entity fxEnt = chain->nodes[i];
        if (!registry.valid(fxEnt)) continue;
        auto* fx     = registry.try_get<Effect>(fxEnt);
        auto* params = registry.try_get<EffectParameters>(fxEnt);
        if (!fx) continue;

        ImGui::PushID(static_cast<int>(fxEnt));

        const effects::EffectKind* kind = m_effectKindRegistry
            ? m_effectKindRegistry->find(fx->kindId)
            : nullptr;
        const char* displayName = kind ? kind->displayName.c_str()
                                       : "Unknown effect kind";

        // Per-effect header row: enable checkbox + name + remove button.
        bool enabled = fx->enabled;
        if (ImGui::Checkbox("##fx_enabled", &enabled) && m_dispatcher) {
            auto cmd = std::make_unique<SetEffectEnabledCommand>(fxEnt, enabled);
            cmd->setPreviousEnabled(fx->enabled);
            m_dispatcher->enqueue(std::move(cmd));
        }
        ImGui::SameLine();
        // SetNextItemAllowOverlap lets the X SmallButton below claim its
        // own click instead of having it eaten by CollapsingHeader's
        // full-row toggle hitbox (issue 2026-05-25 — X was twirling the
        // header instead of removing the effect).
        ImGui::SetNextItemAllowOverlap();
        const bool open = ImGui::CollapsingHeader(displayName,
                                                   ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 28.0f);
        const bool deleteClicked = ImGui::SmallButton("X");

        if (open) {
            if (!kind) {
                ImGui::TextDisabled("Kind 0x%08x not registered", fx->kindId);
            } else if (!params) {
                ImGui::TextDisabled("No EffectParameters component");
            } else {
                // Param sliders, positional w.r.t. the kind's schema.
                for (std::size_t slot = 0; slot < kind->params.size(); ++slot) {
                    const auto& schema = kind->params[slot];

                    // Only Float params are editable in v1 (the only type
                    // ParamSchema-defines for built-in effects today). When
                    // other types ship the dispatch widens here.
                    if (schema.type != ParamValue::Type::Float) {
                        ImGui::TextDisabled("%s: (non-Float types not editable yet)",
                                            schema.displayName.c_str());
                        continue;
                    }

                    float v = (slot < params->values.size())
                                  ? params->values[slot].f4[0]
                                  : schema.defaultValue.f4[0];
                    ImGui::PushID(static_cast<int>(slot));

                    // Keyframe controls row + label, matching the clip-
                    // Transform idiom (renderKeyframeControls + SameLine +
                    // Text + full-width hidden-label slider).
                    renderEffectKeyframeControls(fxEnt, schema, v);
                    ImGui::SameLine();
                    ImGui::Text("%s", schema.displayName.c_str());
                    ImGui::SetNextItemWidth(-1);
                    const bool changed = entity::ui::SliderFloat("##param",
                                                            &v, schema.min, schema.max, "%.3f");

                    // Resolve the layer-local frame from fx->ownerLayer so
                    // the live keyframe writes below can land at the right
                    // place. -1 = playhead outside the layer window.
                    FrameNumber localFrame = -1;
                    if (fx->ownerLayer != entt::null && registry.valid(fx->ownerLayer)) {
                        FrameNumber layerStart = 0;
                        bool haveLayer = false;
                        if (const auto* clip = registry.try_get<Clip>(fx->ownerLayer)) {
                            layerStart = clip->startFrame;
                            haveLayer = true;
                        } else if (const auto* lay = registry.try_get<Layer>(fx->ownerLayer)) {
                            layerStart = lay->startFrame;
                            haveLayer = true;
                        }
                        if (haveLayer) {
                            const FrameNumber currentFrame = m_timeline
                                ? m_timeline->getCurrentFrame() : 0;
                            localFrame = currentFrame - layerStart;
                        }
                    }

                    // Detect whether this param is currently keyframed.
                    const std::uint32_t paramHash = effects::fnv1a32(schema.name);
                    const auto* anim = registry.try_get<EffectAnimatedParameters>(fxEnt);
                    bool isKeyframed = false;
                    std::optional<float> existingKfValue;
                    if (anim) {
                        for (const auto& t : anim->tracks) {
                            if (t.paramKeyHash == paramHash) {
                                if (!t.keyframes.empty()) isKeyframed = true;
                                if (localFrame >= 0) {
                                    for (const auto& kf : t.keyframes) {
                                        if (kf.frame == localFrame) {
                                            existingKfValue = kf.value;
                                            break;
                                        }
                                    }
                                }
                                break;
                            }
                        }
                    }

                    if (ImGui::IsItemActivated()) {
                        m_preEditEffect.scalarValue = (slot < params->values.size())
                            ? params->values[slot].f4[0]
                            : schema.defaultValue.f4[0];
                        m_preEditEffect.wasKeyframed   = isKeyframed && localFrame >= 0;
                        m_preEditEffect.keyframeFrame  = localFrame;
                        m_preEditEffect.keyframeValue  = existingKfValue;
                        m_preEditEffect.paramName      = schema.name;
                        m_preEditEffect.effectEntity   = fxEnt;
                    }
                    if (changed) {
                        // Live-update the slot so the slider tracks the
                        // user's drag without waiting for the dispatched
                        // command to land on next tick.
                        if (slot >= params->values.size()) {
                            params->values.resize(kind->params.size(), {});
                            for (std::size_t i = 0; i < kind->params.size(); ++i) {
                                if (params->values[i].type == ParamValue::Type::Float &&
                                    params->values[i].f4[0] == 0.0f &&
                                    params->values[i].i == 0)
                                {
                                    params->values[i] = kind->params[i].defaultValue;
                                }
                            }
                        }
                        params->values[slot] = ParamValue::makeFloat(v);

                        // AE-style auto-keyframe — when the param is
                        // keyframed, every slider tick upserts a kf at
                        // the current layer-local frame so the live
                        // motion is keyframed-not-scalar. AnimationSystem
                        // would otherwise overwrite the slot from the
                        // existing track on the next tick.
                        if (m_preEditEffect.wasKeyframed && localFrame >= 0) {
                            auto& a = registry.get_or_emplace<EffectAnimatedParameters>(fxEnt);
                            EffectAnimatedParameters::NamedTrack* t = nullptr;
                            for (auto& nt : a.tracks) {
                                if (nt.paramKeyHash == paramHash) { t = &nt; break; }
                            }
                            if (!t) {
                                a.tracks.push_back({});
                                a.tracks.back().paramKeyHash = paramHash;
                                t = &a.tracks.back();
                            }
                            // Binary-search insert / replace at localFrame.
                            auto it = std::lower_bound(t->keyframes.begin(), t->keyframes.end(),
                                localFrame,
                                [](const Keyframe& k, FrameNumber f) { return k.frame < f; });
                            if (it != t->keyframes.end() && it->frame == localFrame) {
                                it->value = v;
                            } else {
                                t->keyframes.insert(it, Keyframe{localFrame, v, InterpolationType::Linear});
                            }
                        }
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit() && m_dispatcher) {
                        if (m_preEditEffect.wasKeyframed &&
                            m_preEditEffect.effectEntity == fxEnt &&
                            m_preEditEffect.keyframeFrame >= 0)
                        {
                            // Keyframed edit — wrap the live in-place
                            // upsert in an undoable command. setPreviousValue
                            // carries the pre-drag kf state (nullopt = no
                            // kf existed at this frame; undo removes the
                            // one the drag created).
                            auto cmd = std::make_unique<UpsertEffectKeyframeCommand>(
                                fxEnt, m_preEditEffect.paramName,
                                m_preEditEffect.keyframeFrame, v);
                            cmd->setPreviousValue(m_preEditEffect.keyframeValue);
                            m_dispatcher->enqueue(std::move(cmd));
                        } else {
                            auto cmd = std::make_unique<SetEffectFloatParamCommand>(
                                fxEnt, schema.name, v);
                            cmd->setPreviousValue(m_preEditEffect.scalarValue);
                            m_dispatcher->enqueue(std::move(cmd));
                        }
                    }
                    ImGui::PopID();
                }
            }
        }

        if (deleteClicked && m_dispatcher) {
            m_dispatcher->enqueue(
                std::make_unique<RemoveEffectCommand>(layerEntity, fxEnt));
        }

        ImGui::PopID();
    }
}

void PropertyWindow::renderTimelineProperties() {
    ImGui::Text("Timeline Properties");
    ImGui::Separator();
    ImGui::Spacing();

    // Duration editor
    ImGui::Text("Duration");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Total timeline duration in minutes.\nClips can extend this automatically if placed beyond the end.");
    }

    // Convert microseconds to minutes for editing
    Timecode currentDuration = m_timeline->getDuration();
    float durationMinutes = static_cast<float>(currentDuration) / 60000000.0f;  // 60 * 1,000,000

    ImGui::SetNextItemWidth(100);
    if (entity::ui::DragFloat("##duration_minutes", &durationMinutes, 0.1f, 0.5f, 120.0f, "%.1f min")) {
        // Convert back to microseconds
        Timecode newDuration = static_cast<Timecode>(durationMinutes * 60000000.0);
        m_timeline->setDuration(newDuration);
    }

    // Show duration in different formats
    double durationSeconds = currentDuration / 1000000.0;
    int hours = static_cast<int>(durationSeconds) / 3600;
    int minutes = (static_cast<int>(durationSeconds) % 3600) / 60;
    int seconds = static_cast<int>(durationSeconds) % 60;
    ImGui::Text("(%02d:%02d:%02d)", hours, minutes, seconds);

    ImGui::Spacing();

    // Quick presets
    ImGui::Text("Presets:");
    if (ImGui::Button("1 min")) {
        m_timeline->setDuration(60000000);
    }
    ImGui::SameLine();
    if (ImGui::Button("5 min")) {
        m_timeline->setDuration(300000000);
    }
    ImGui::SameLine();
    if (ImGui::Button("10 min")) {
        m_timeline->setDuration(600000000);
    }
    ImGui::SameLine();
    if (ImGui::Button("30 min")) {
        m_timeline->setDuration(1800000000);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Frame rate (read-only for now)
    ImGui::Text("Frame Rate: %.2f fps", m_timeline->getFrameRate());

    // Track count
    ImGui::Text("Tracks: %zu", m_timeline->getTrackCount());

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("Select a clip to edit its properties");
}

int PropertyWindow::getCurrentClipFrame() const {
    if (!m_timeline) return -1;

    entt::entity selectedClip = m_timeline->getSelectedClip();
    if (selectedClip == entt::null) return -1;

    auto& registry = m_timeline->getRegistry();

    // Clip-backed entities source placement from Clip; Generative content
    // layers (Layer, no Clip) source it from Layer. Both store start frame
    // and duration in timeline frames.
    FrameNumber startFrame = 0;
    FrameNumber duration = 0;
    if (const auto* clip = registry.try_get<Clip>(selectedClip)) {
        startFrame = clip->startFrame;
        duration = clip->duration;
    } else if (const auto* lay = registry.try_get<Layer>(selectedClip)) {
        startFrame = lay->startFrame;
        duration = lay->duration;
    } else {
        return -1;
    }

    // Convert to clip-relative frame. Routes through Timeline::getCurrentFrame()
    // which uses std::round, matching Timeline::seekToFrame()'s rounding so a
    // frame number round-trips cleanly. Truncating here was the cause of the
    // "edit at keyframe inserts new keyframe one frame off" bug.
    FrameNumber clipFrame = m_timeline->getCurrentFrame() - startFrame;

    // Check if playhead is within the layer's bounds
    if (clipFrame < 0 || clipFrame >= duration) {
        return -1;  // Playhead outside the layer
    }

    return static_cast<int>(clipFrame);
}

int PropertyWindow::getCurrentOAFrame(entt::entity oaEntity) const {
    if (!m_timeline) return -1;
    auto& registry = m_timeline->getRegistry();
    const auto* lay = registry.try_get<Layer>(oaEntity);
    if (!lay) return -1;
    FrameNumber currentFrame = m_timeline->getCurrentFrame();
    if (currentFrame < lay->startFrame || currentFrame >= lay->startFrame + lay->duration)
        return -1;
    return static_cast<int>(currentFrame - lay->startFrame);
}

void PropertyWindow::renderOAKeyframeControls(entt::entity oaEntity,
                                              AnimatableProperty property,
                                              const char* propertyName) {
    if (!m_timeline) return;
    auto& registry = m_timeline->getRegistry();

    auto* animProps = registry.try_get<AnimatedProperties>(oaEntity);
    const KeyframeTrack* track = animProps ? animProps->getTrack(property) : nullptr;

    bool hasKeyframes = track && track->hasKeyframes();
    int oaFrame = getCurrentOAFrame(oaEntity);
    bool hasKeyframeAtCurrentFrame = false;

    if (track && oaFrame >= 0) {
        hasKeyframeAtCurrentFrame = (track->getKeyframeAt(static_cast<FrameNumber>(oaFrame)) != nullptr);
    }

    ImGui::PushID(static_cast<int>(property) + 10000);

    // Stopwatch button
    ImU32 stopwatchColor = hasKeyframes ? IM_COL32(255, 180, 50, 255) : IM_COL32(128, 128, 128, 255);
    ImGui::PushStyleColor(ImGuiCol_Text, stopwatchColor);
    if (ImGui::SmallButton(hasKeyframes ? "(*)" : "( )")) {
        if (oaFrame >= 0) {
            if (!animProps) {
                registry.emplace<AnimatedProperties>(oaEntity);
                animProps = registry.try_get<AnimatedProperties>(oaEntity);
            }
            KeyframeTrack& mutableTrack = animProps->getOrCreateTrack(property);
            const Keyframe* existingKf = mutableTrack.getKeyframeAt(static_cast<FrameNumber>(oaFrame));
            if (existingKf) {
                mutableTrack.removeKeyframe(static_cast<FrameNumber>(oaFrame));
            } else {
                float currentVal = track ? track->evaluate(static_cast<FrameNumber>(oaFrame)) : 0.0f;
                mutableTrack.addKeyframe(static_cast<FrameNumber>(oaFrame), currentVal);
            }
        }
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(hasKeyframes ? "Animation enabled - click to toggle keyframe at current frame"
                                       : "Click to add keyframe at current frame");
    }

    ImGui::SameLine();

    // Previous keyframe button
    bool canGoPrev = hasKeyframes && oaFrame > 0;
    if (!canGoPrev) ImGui::BeginDisabled();
    if (ImGui::SmallButton("<")) {
        if (track && !track->keyframes.empty()) {
            const auto* lay = registry.try_get<Layer>(oaEntity);
            FrameNumber prevFrame = FrameNumber(-1);
            for (const auto& kf : track->keyframes) {
                if (kf.frame < oaFrame) prevFrame = kf.frame;
                else break;
            }
            if (prevFrame != FrameNumber(-1) && lay)
                m_timeline->seekToFrame(lay->startFrame + prevFrame);
        }
    }
    if (!canGoPrev) ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Go to previous keyframe");

    ImGui::SameLine();

    // Diamond add/remove button
    ImU32 diamondColor = hasKeyframeAtCurrentFrame ? IM_COL32(255, 180, 50, 255) : IM_COL32(128, 128, 128, 255);
    ImGui::PushStyleColor(ImGuiCol_Text, diamondColor);
    if (ImGui::SmallButton("<>")) {
        if (oaFrame >= 0) {
            if (!animProps) {
                registry.emplace<AnimatedProperties>(oaEntity);
                animProps = registry.try_get<AnimatedProperties>(oaEntity);
            }
            KeyframeTrack& mutableTrack = animProps->getOrCreateTrack(property);
            const Keyframe* existingKf = mutableTrack.getKeyframeAt(static_cast<FrameNumber>(oaFrame));
            if (existingKf) {
                mutableTrack.removeKeyframe(static_cast<FrameNumber>(oaFrame));
            } else {
                float currentVal = track ? track->evaluate(static_cast<FrameNumber>(oaFrame)) : 0.0f;
                mutableTrack.addKeyframe(static_cast<FrameNumber>(oaFrame), currentVal);
            }
        }
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(hasKeyframeAtCurrentFrame ? "Remove keyframe at current frame"
                                                    : "Add keyframe at current frame");
    }

    ImGui::SameLine();

    // Next keyframe button
    if (!hasKeyframes) ImGui::BeginDisabled();
    if (ImGui::SmallButton(">")) {
        if (track && !track->keyframes.empty()) {
            const auto* lay = registry.try_get<Layer>(oaEntity);
            for (const auto& kf : track->keyframes) {
                if (kf.frame > oaFrame && lay) {
                    m_timeline->seekToFrame(lay->startFrame + kf.frame);
                    break;
                }
            }
        }
    }
    if (!hasKeyframes) ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Go to next keyframe");

    ImGui::SameLine();
    ImGui::TextDisabled("%s", propertyName);

    ImGui::PopID();
}

void PropertyWindow::renderOAValueRow(entt::entity oaEntity,
                                      AnimatableProperty property,
                                      const char* label,
                                      float dragStep,
                                      float minVal, float maxVal,
                                      const char* format) {
    if (!m_timeline) return;
    auto& registry = m_timeline->getRegistry();

    // Stopwatch + nav buttons + label (reuses the existing 4-button helper).
    // Pushes its own ID stack frame internally.
    renderOAKeyframeControls(oaEntity, property, label);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);

    // Push a unique ID so DragFloat widgets across the 9 OA channels don't
    // share ImGui state.
    ImGui::PushID(static_cast<int>(property) + 20000);

    const int oaFrame = getCurrentOAFrame(oaEntity);

    // Read current evaluated value. The DragFloat shows what the operator
    // is *editing* — the value at the current layer-local frame.
    const auto* animProps = registry.try_get<AnimatedProperties>(oaEntity);
    const KeyframeTrack* track = animProps ? animProps->getTrack(property) : nullptr;
    float currentVal;
    if (track && track->hasKeyframes()) {
        const FrameNumber f = (oaFrame >= 0) ? static_cast<FrameNumber>(oaFrame) : FrameNumber{0};
        currentVal = track->evaluate(f);
    } else {
        const bool isScale = (property == AnimatableProperty::ScaleX ||
                              property == AnimatableProperty::ScaleY ||
                              property == AnimatableProperty::ScaleZ);
        currentVal = isScale ? 1.0f : 0.0f;
    }

    // Playhead outside the OA window → drag is disabled. Mirrors the
    // diamond's behavior in renderOAKeyframeControls and telegraphs why
    // editing has no effect there.
    const bool dragEnabled = (oaFrame >= 0);
    if (!dragEnabled) ImGui::BeginDisabled();

    const bool changed = entity::ui::DragFloat("##oa_val", &currentVal, dragStep,
                                          minVal, maxVal, format);

    // Capture pre-edit state on activation so the drag-end deactivate
    // branch can emit an UpsertKeyframeCommand keyed to the original
    // (frame, prior value) pair. Mirrors the Clip Rotation/Opacity pattern.
    if (ImGui::IsItemActivated() && dragEnabled) {
        m_preEditOA.property = property;
        m_preEditOA.frame    = static_cast<FrameNumber>(oaFrame);
        m_preEditOA.keyframeValue.reset();
        if (track) {
            if (const Keyframe* existing = track->getKeyframeAt(m_preEditOA.frame)) {
                m_preEditOA.keyframeValue = existing->value;
            }
        }
        m_preEditOA.valid = true;
    }

    if (changed && dragEnabled) {
        // Auto-create / upsert keyframe at the current layer-local frame
        // (ADR-0020): OA layers have no static value — every value is a
        // keyframe, so dragging is *always* a keyframe write.
        auto& animPropsMut = registry.get_or_emplace<AnimatedProperties>(oaEntity);
        KeyframeTrack& mutableTrack = animPropsMut.getOrCreateTrack(property);
        mutableTrack.addKeyframe(static_cast<FrameNumber>(oaFrame), currentVal);
    }

    // On drag-end, commit one undoable UpsertKeyframeCommand. The command
    // re-applies the same write its own execute() path performs, so the
    // intermediate drag-frame writes are coalesced into a single undo step.
    if (ImGui::IsItemDeactivatedAfterEdit() && m_preEditOA.valid && m_dispatcher) {
        if (auto idx = findClipIndices(m_timeline, oaEntity)) {
            auto cmd = std::make_unique<UpsertKeyframeCommand>(
                idx->first, idx->second,
                property, m_preEditOA.frame, currentVal);
            cmd->setPreviousValue(m_preEditOA.keyframeValue);
            m_dispatcher->enqueue(std::move(cmd));
        }
        m_preEditOA.valid = false;
    }

    if (!dragEnabled) ImGui::EndDisabled();

    ImGui::PopID();
}

void PropertyWindow::goToPreviousKeyframe(AnimatableProperty property) {
    if (!m_timeline) return;

    entt::entity selectedClip = m_timeline->getSelectedClip();
    if (selectedClip == entt::null) return;

    auto& registry = m_timeline->getRegistry();
    const auto* animProps = registry.try_get<AnimatedProperties>(selectedClip);
    if (!animProps) return;

    // Start frame for clip-local -> timeline conversion. Clip-backed entities
    // source it from Clip; Generative layers from Layer.
    FrameNumber layerStart = 0;
    if (const auto* clip = registry.try_get<Clip>(selectedClip)) {
        layerStart = clip->startFrame;
    } else if (const auto* lay = registry.try_get<Layer>(selectedClip)) {
        layerStart = lay->startFrame;
    } else {
        return;
    }

    const KeyframeTrack* track = animProps->getTrack(property);
    if (!track || track->keyframes.empty()) return;

    // Get current clip frame
    int currentClipFrame = getCurrentClipFrame();
    if (currentClipFrame < 0) {
        // If outside the layer, go to last keyframe
        m_timeline->seekToFrame(layerStart + track->keyframes.back().frame);
        return;
    }

    // Find previous keyframe
    FrameNumber prevFrame = -1;
    for (const auto& kf : track->keyframes) {
        if (kf.frame < currentClipFrame) {
            prevFrame = kf.frame;
        } else {
            break;  // Keyframes are sorted, stop when we pass current
        }
    }

    if (prevFrame >= 0) {
        m_timeline->seekToFrame(layerStart + prevFrame);
    }
}

void PropertyWindow::goToNextKeyframe(AnimatableProperty property) {
    if (!m_timeline) return;

    entt::entity selectedClip = m_timeline->getSelectedClip();
    if (selectedClip == entt::null) return;

    auto& registry = m_timeline->getRegistry();
    const auto* animProps = registry.try_get<AnimatedProperties>(selectedClip);
    if (!animProps) return;

    // Start frame for clip-local -> timeline conversion. Clip-backed entities
    // source it from Clip; Generative layers from Layer.
    FrameNumber layerStart = 0;
    if (const auto* clip = registry.try_get<Clip>(selectedClip)) {
        layerStart = clip->startFrame;
    } else if (const auto* lay = registry.try_get<Layer>(selectedClip)) {
        layerStart = lay->startFrame;
    } else {
        return;
    }

    const KeyframeTrack* track = animProps->getTrack(property);
    if (!track || track->keyframes.empty()) return;

    // Get current clip frame
    int currentClipFrame = getCurrentClipFrame();
    if (currentClipFrame < 0) {
        // If outside the layer, go to first keyframe
        m_timeline->seekToFrame(layerStart + track->keyframes.front().frame);
        return;
    }

    // Find next keyframe
    for (const auto& kf : track->keyframes) {
        if (kf.frame > currentClipFrame) {
            m_timeline->seekToFrame(layerStart + kf.frame);
            return;
        }
    }
}

void PropertyWindow::toggleKeyframeAtCurrentFrame(AnimatableProperty property, float currentValue) {
    if (!m_timeline) return;

    entt::entity selectedClip = m_timeline->getSelectedClip();
    if (selectedClip == entt::null) return;

    int clipFrame = getCurrentClipFrame();
    if (clipFrame < 0) return;  // Playhead not on clip

    auto& registry = m_timeline->getRegistry();

    // Get or create AnimatedProperties component
    auto* animProps = registry.try_get<AnimatedProperties>(selectedClip);
    if (!animProps) {
        registry.emplace<AnimatedProperties>(selectedClip);
        animProps = registry.try_get<AnimatedProperties>(selectedClip);
    }

    KeyframeTrack& track = animProps->getOrCreateTrack(property);

    // Check if keyframe exists at this frame
    const Keyframe* existingKf = track.getKeyframeAt(static_cast<FrameNumber>(clipFrame));
    if (existingKf) {
        // Remove existing keyframe
        track.removeKeyframe(static_cast<FrameNumber>(clipFrame));
    } else {
        // Add new keyframe with current value
        track.addKeyframe(static_cast<FrameNumber>(clipFrame), currentValue);
    }
}

void PropertyWindow::updateKeyframeOnValueChange(AnimatableProperty property, float newValue) {
    if (!m_timeline) return;

    entt::entity selectedClip = m_timeline->getSelectedClip();
    if (selectedClip == entt::null) return;

    int clipFrame = getCurrentClipFrame();
    if (clipFrame < 0) return;  // Playhead not on clip

    auto& registry = m_timeline->getRegistry();

    // Check if property has animation (keyframes)
    auto* animProps = registry.try_get<AnimatedProperties>(selectedClip);
    if (!animProps) return;

    const KeyframeTrack* track = animProps->getTrack(property);
    if (!track || !track->hasKeyframes()) return;

    // Property has keyframes - auto-update/add keyframe at current frame
    KeyframeTrack& mutableTrack = animProps->getOrCreateTrack(property);
    mutableTrack.addKeyframe(static_cast<FrameNumber>(clipFrame), newValue);
}

void PropertyWindow::renderKeyframeControls(AnimatableProperty property, const char* propertyName,
                                            float currentValue, std::function<void(float)> onValueChanged) {
    if (!m_timeline) return;

    entt::entity selectedClip = m_timeline->getSelectedClip();
    if (selectedClip == entt::null) return;

    auto& registry = m_timeline->getRegistry();
    const auto* animProps = registry.try_get<AnimatedProperties>(selectedClip);
    const KeyframeTrack* track = animProps ? animProps->getTrack(property) : nullptr;

    bool hasKeyframes = track && track->hasKeyframes();
    int clipFrame = getCurrentClipFrame();
    bool hasKeyframeAtCurrentFrame = false;

    if (track && clipFrame >= 0) {
        hasKeyframeAtCurrentFrame = (track->getKeyframeAt(static_cast<FrameNumber>(clipFrame)) != nullptr);
    }

    // Push unique ID for this property's controls
    ImGui::PushID(static_cast<int>(property));

    // Stopwatch button (filled if has keyframes)
    ImU32 stopwatchColor = hasKeyframes ? IM_COL32(255, 180, 50, 255) : IM_COL32(128, 128, 128, 255);
    ImGui::PushStyleColor(ImGuiCol_Text, stopwatchColor);
    if (ImGui::SmallButton(hasKeyframes ? "(*)" : "( )")) {
        // Toggle animation - if no keyframes, add one at current frame
        // If has keyframes, we could clear them (but let's just add at current frame for now)
        if (clipFrame >= 0) {
            toggleKeyframeAtCurrentFrame(property, currentValue);
        }
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(hasKeyframes ? "Animation enabled - click to toggle keyframe at current frame"
                                       : "Click to add keyframe at current frame");
    }

    ImGui::SameLine();

    // Previous keyframe button
    bool canGoPrev = hasKeyframes && clipFrame > 0;
    if (!canGoPrev) ImGui::BeginDisabled();
    if (ImGui::SmallButton("<")) {
        goToPreviousKeyframe(property);
    }
    if (!canGoPrev) ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Go to previous keyframe");
    }

    ImGui::SameLine();

    // Add/remove keyframe diamond button
    ImU32 diamondColor = hasKeyframeAtCurrentFrame ? IM_COL32(255, 180, 50, 255) : IM_COL32(128, 128, 128, 255);
    ImGui::PushStyleColor(ImGuiCol_Text, diamondColor);
    if (ImGui::SmallButton(hasKeyframeAtCurrentFrame ? "<>" : "<>")) {
        if (clipFrame >= 0) {
            toggleKeyframeAtCurrentFrame(property, currentValue);
        }
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(hasKeyframeAtCurrentFrame ? "Remove keyframe at current frame"
                                                    : "Add keyframe at current frame");
    }

    ImGui::SameLine();

    // Next keyframe button
    bool canGoNext = hasKeyframes;
    if (!canGoNext) ImGui::BeginDisabled();
    if (ImGui::SmallButton(">")) {
        goToNextKeyframe(property);
    }
    if (!canGoNext) ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Go to next keyframe");
    }

    ImGui::PopID();
}

void PropertyWindow::renderEffectKeyframeControls(
    entt::entity effectEntity,
    const effects::ParamSchema& schema,
    float currentValue)
{
    if (!m_timeline) return;
    auto& reg = m_timeline->getRegistry();
    if (!reg.valid(effectEntity)) return;
    const auto* fx = reg.try_get<Effect>(effectEntity);
    if (!fx) return;

    // Layer-local frame from fx->ownerLayer. Disable all the controls
    // (greyed-out) when the playhead is outside the layer window — the
    // diamond / arrows have no well-defined target frame there.
    FrameNumber layerStart = 0;
    bool haveLayer = false;
    if (fx->ownerLayer != entt::null && reg.valid(fx->ownerLayer)) {
        if (const auto* clip = reg.try_get<Clip>(fx->ownerLayer)) {
            layerStart = clip->startFrame; haveLayer = true;
        } else if (const auto* lay = reg.try_get<Layer>(fx->ownerLayer)) {
            layerStart = lay->startFrame; haveLayer = true;
        }
    }
    const FrameNumber currentFrame = m_timeline->getCurrentFrame();
    const FrameNumber localFrame = haveLayer ? (currentFrame - layerStart) : -1;

    // Find the track + keyframe-at-current-frame status.
    const std::uint32_t hash = effects::fnv1a32(schema.name);
    const auto* anim = reg.try_get<EffectAnimatedParameters>(effectEntity);
    const EffectAnimatedParameters::NamedTrack* track = nullptr;
    if (anim) {
        for (const auto& t : anim->tracks) {
            if (t.paramKeyHash == hash) { track = &t; break; }
        }
    }
    const bool hasKeyframes = track && !track->keyframes.empty();
    bool hasKeyframeAtCurrentFrame = false;
    if (track && localFrame >= 0) {
        for (const auto& kf : track->keyframes) {
            if (kf.frame == localFrame) { hasKeyframeAtCurrentFrame = true; break; }
        }
    }

    ImGui::PushID(static_cast<int>(hash));

    // Stopwatch — toggles a keyframe at the current frame (add if none
    // exist, or remove if one exists at this exact frame).
    ImU32 stopwatchColor = hasKeyframes ? IM_COL32(255, 180, 50, 255)
                                        : IM_COL32(128, 128, 128, 255);
    ImGui::PushStyleColor(ImGuiCol_Text, stopwatchColor);
    if (ImGui::SmallButton(hasKeyframes ? "(*)" : "( )")) {
        if (localFrame >= 0 && m_dispatcher) {
            if (hasKeyframeAtCurrentFrame) {
                m_dispatcher->enqueue(std::make_unique<RemoveEffectKeyframeCommand>(
                    effectEntity, schema.name, localFrame));
            } else {
                m_dispatcher->enqueue(std::make_unique<UpsertEffectKeyframeCommand>(
                    effectEntity, schema.name, localFrame, currentValue));
            }
        }
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(hasKeyframes
            ? "Toggle keyframe at current frame"
            : "Click to add keyframe at current frame");
    }

    ImGui::SameLine();

    // Prev keyframe arrow
    const bool canGoPrev = hasKeyframes && localFrame > 0;
    if (!canGoPrev) ImGui::BeginDisabled();
    if (ImGui::SmallButton("<")) {
        if (track) {
            FrameNumber prevFrame = -1;
            for (const auto& kf : track->keyframes) {
                if (kf.frame < localFrame && kf.frame > prevFrame) prevFrame = kf.frame;
            }
            if (prevFrame >= 0) {
                m_timeline->seekToFrame(prevFrame + layerStart);
            }
        }
    }
    if (!canGoPrev) ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Go to previous keyframe");

    ImGui::SameLine();

    // Diamond — add/remove keyframe at current frame
    ImU32 diamondColor = hasKeyframeAtCurrentFrame ? IM_COL32(255, 180, 50, 255)
                                                   : IM_COL32(128, 128, 128, 255);
    ImGui::PushStyleColor(ImGuiCol_Text, diamondColor);
    if (ImGui::SmallButton("<>")) {
        if (localFrame >= 0 && m_dispatcher) {
            if (hasKeyframeAtCurrentFrame) {
                m_dispatcher->enqueue(std::make_unique<RemoveEffectKeyframeCommand>(
                    effectEntity, schema.name, localFrame));
            } else {
                m_dispatcher->enqueue(std::make_unique<UpsertEffectKeyframeCommand>(
                    effectEntity, schema.name, localFrame, currentValue));
            }
        }
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(hasKeyframeAtCurrentFrame
            ? "Remove keyframe at current frame"
            : "Add keyframe at current frame");
    }

    ImGui::SameLine();

    // Next keyframe arrow
    const bool canGoNext = hasKeyframes;
    if (!canGoNext) ImGui::BeginDisabled();
    if (ImGui::SmallButton(">")) {
        if (track) {
            FrameNumber nextFrame = std::numeric_limits<FrameNumber>::max();
            for (const auto& kf : track->keyframes) {
                if (kf.frame > localFrame && kf.frame < nextFrame) nextFrame = kf.frame;
            }
            if (nextFrame != std::numeric_limits<FrameNumber>::max()) {
                m_timeline->seekToFrame(nextFrame + layerStart);
            }
        }
    }
    if (!canGoNext) ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Go to next keyframe");

    ImGui::PopID();
}

void PropertyWindow::renderScreenProperties() {
    if (!m_timeline) return;

    entt::entity selectedScreen = m_timeline->getSelectedScreen();
    if (selectedScreen == entt::null) return;

    auto& registry = m_timeline->getRegistry();
    Screen* screen = registry.try_get<Screen>(selectedScreen);

    if (!screen) {
        ImGui::TextDisabled("Invalid screen");
        return;
    }

    // Push unique ID for this screen
    ImGui::PushID(static_cast<int>(selectedScreen));

    ImGui::Text("Screen: %s", screen->name.c_str());
    ImGui::Separator();

    // Name
    if (ImGui::CollapsingHeader("Identity", ImGuiTreeNodeFlags_DefaultOpen)) {
        char nameBuf[256];
        strncpy(nameBuf, screen->name.c_str(), sizeof(nameBuf) - 1);
        nameBuf[sizeof(nameBuf) - 1] = '\0';
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) {
            screen->name = nameBuf;
        }
    }

    // Transform — moved before Type/Geometry so the stage-object trio
    // (Screen, Prop, Projector) all share Identity -> Transform -> kind-specific.
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Position");
        ImGui::SetNextItemWidth(-1);
        entity::ui::DragFloat3("##pos", screen->position.data(), 0.1f);

        ImGui::Text("Rotation");
        ImGui::SetNextItemWidth(-1);
        entity::ui::DragFloat3("##rot", screen->rotation.data(), 1.0f, -180.0f, 180.0f);

        ImGui::Text("Size (m)");
        ImGui::SetNextItemWidth(-1);
        entity::ui::DragFloat3("##size", screen->size.data(), 0.01f, 0.0f, 1000.0f, "%.2f m");

        if (ImGui::Button("Reset Transform")) {
            screen->position = {0.0f, 0.0f, 0.0f};
            screen->rotation = {0.0f, 0.0f, 0.0f};
            // Size reset honors the current model — resetting a screen with a
            // 3D mesh to {4, 2.25, 0} would flatten the mesh's Z to zero.
            const Model* resetModel = nullptr;
            if (screen->modelEntity != entt::null && registry.valid(screen->modelEntity)) {
                resetModel = registry.try_get<Model>(screen->modelEntity);
            }
            applyModelToScreen(*screen, screen->modelEntity, resetModel);
        }
    }

    // Type
    if (ImGui::CollapsingHeader("Type", ImGuiTreeNodeFlags_DefaultOpen)) {
        int typeInt = static_cast<int>(screen->type);
        ImGui::RadioButton("LED Wall", &typeInt, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Projection Surface", &typeInt, 1);
        screen->type = static_cast<ScreenType>(typeInt);
        if (screen->type == ScreenType::ProjectionSurface) {
            ImGui::TextDisabled("Add projectors in the Screens panel.");
        }
    }

    // Geometry
    if (ImGui::CollapsingHeader("Geometry", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Model picker combo
        {
            std::string currentModelName = "None (flat quad)";
            if (screen->modelEntity != entt::null && registry.valid(screen->modelEntity)) {
                if (const Model* m = registry.try_get<Model>(screen->modelEntity))
                    currentModelName = m->name;
                else
                    applyModelToScreen(*screen, entt::null, nullptr);  // stale reference — clear it
            }
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##model_picker", currentModelName.c_str())) {
                if (ImGui::Selectable("None (flat quad)", screen->modelEntity == entt::null))
                    applyModelToScreen(*screen, entt::null, nullptr);
                for (auto [e, model] : registry.view<Model>().each()) {
                    bool sel = (screen->modelEntity == e);
                    if (ImGui::Selectable(model.name.c_str(), sel))
                        applyModelToScreen(*screen, e, &model);
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("Model");
        }

        // Show mesh stats for assigned model
        if (screen->modelEntity != entt::null && registry.valid(screen->modelEntity)) {
            if (const Model* model = registry.try_get<Model>(screen->modelEntity)) {
                ImGui::TextDisabled("%zu verts  %zu tris",
                    model->mesh.vertices.size(), model->mesh.indices.size() / 3);
                if (ImGui::SmallButton("Clear")) {
                    applyModelToScreen(*screen, entt::null, nullptr);
                }
            }
        } else {
            ImGui::TextDisabled("Drag a model from Model Bin, or pick above.");
            // Accept drag-drop as well
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MODEL_ENTITY")) {
                    uint32_t modelEntityId = *static_cast<const uint32_t*>(payload->Data);
                    auto droppedEntity = static_cast<entt::entity>(modelEntityId);
                    const Model* droppedModel = registry.valid(droppedEntity)
                        ? registry.try_get<Model>(droppedEntity)
                        : nullptr;
                    applyModelToScreen(*screen, droppedEntity, droppedModel);
                }
                ImGui::EndDragDropTarget();
            }
        }
    }

    // Resolution
    if (ImGui::CollapsingHeader("Resolution", ImGuiTreeNodeFlags_DefaultOpen)) {
        int resolution[2] = {static_cast<int>(screen->width), static_cast<int>(screen->height)};
        ImGui::SetNextItemWidth(-1);
        // Defer commit until Enter or focus-loss (#31). Default InputInt2
        // commits on every keystroke, which churns the compositor's
        // resize path for every digit typed; with the in-place resize
        // landing in this same fix that's no longer corrupting state but
        // it is still wasteful (waitForGpu + texture realloc per
        // keystroke).
        entity::ui::InputInt2("Size", resolution, ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            screen->width  = static_cast<uint32_t>(std::max(1, resolution[0]));
            screen->height = static_cast<uint32_t>(std::max(1, resolution[1]));
        }

        // Resolution presets
        if (ImGui::Button("1920x1080")) { screen->width = 1920; screen->height = 1080; }
        ImGui::SameLine();
        if (ImGui::Button("3840x2160")) { screen->width = 3840; screen->height = 2160; }
        ImGui::SameLine();
        if (ImGui::Button("1080x1920")) { screen->width = 1080; screen->height = 1920; }
    }

    // Display
    if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Visible", &screen->visible);
        ImGui::Text("Opacity");
        ImGui::SetNextItemWidth(-1);
        entity::ui::SliderFloat("##screen_opacity", &screen->opacity, 0.0f, 1.0f);
        ImGui::TextDisabled("Stage view only; does not affect output.");
        ImGui::SetNextItemWidth(-1);
        entity::ui::DragInt("Z-Order", &screen->zOrder);
    }

    ImGui::PopID();
}

void PropertyWindow::renderPropProperties() {
    if (!m_timeline) return;

    entt::entity selectedProp = m_timeline->getSelectedProp();
    if (selectedProp == entt::null) return;

    auto& registry = m_timeline->getRegistry();
    Prop* prop = registry.try_get<Prop>(selectedProp);
    if (!prop) {
        ImGui::TextDisabled("Invalid prop");
        return;
    }

    ImGui::PushID(static_cast<int>(selectedProp));

    ImGui::Text("Prop: %s", prop->name.c_str());
    ImGui::TextDisabled("Pre-vis only \xe2\x80\x94 not in projector output");
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Identity", ImGuiTreeNodeFlags_DefaultOpen)) {
        char nameBuf[256];
        strncpy(nameBuf, prop->name.c_str(), sizeof(nameBuf) - 1);
        nameBuf[sizeof(nameBuf) - 1] = '\0';
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) {
            prop->name = nameBuf;
        }
    }

    // Transform — moved before Geometry so Prop matches the
    // Identity -> Transform -> kind-specific -> Display order.
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Position");
        ImGui::SetNextItemWidth(-1);
        entity::ui::DragFloat3("##prop_pos", prop->position.data(), 0.1f);

        ImGui::Text("Rotation");
        ImGui::SetNextItemWidth(-1);
        entity::ui::DragFloat3("##prop_rot", prop->rotation.data(), 1.0f, -180.0f, 180.0f);

        ImGui::Text("Size (m)");
        ImGui::SetNextItemWidth(-1);
        entity::ui::DragFloat3("##prop_size", prop->size.data(), 0.01f, 0.0f, 1000.0f, "%.2f m");

        if (ImGui::Button("Reset Transform")) {
            prop->position = {0.0f, 0.0f, 0.0f};
            prop->rotation = {0.0f, 0.0f, 0.0f};
            prop->size = {1.0f, 1.0f, 1.0f};
        }
    }

    if (ImGui::CollapsingHeader("Geometry", ImGuiTreeNodeFlags_DefaultOpen)) {
        std::string currentModelName = "None";
        if (prop->modelEntity != entt::null && registry.valid(prop->modelEntity)) {
            if (const Model* m = registry.try_get<Model>(prop->modelEntity))
                currentModelName = m->name;
            else
                applyModelToProp(*prop, entt::null, nullptr);  // stale reference
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##prop_model_picker", currentModelName.c_str())) {
            if (ImGui::Selectable("None", prop->modelEntity == entt::null))
                applyModelToProp(*prop, entt::null, nullptr);
            for (auto [e, model] : registry.view<Model>().each()) {
                bool sel = (prop->modelEntity == e);
                if (ImGui::Selectable(model.name.c_str(), sel))
                    applyModelToProp(*prop, e, &model);
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Model");

        if (prop->modelEntity != entt::null && registry.valid(prop->modelEntity)) {
            if (const Model* model = registry.try_get<Model>(prop->modelEntity)) {
                ImGui::TextDisabled("%zu verts  %zu tris",
                    model->mesh.vertices.size(), model->mesh.indices.size() / 3);
                if (ImGui::SmallButton("Clear")) {
                    applyModelToProp(*prop, entt::null, nullptr);
                }
            }
        } else {
            ImGui::TextDisabled("Drag a model from Model Bin, or pick above.");
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MODEL_ENTITY")) {
                    uint32_t modelEntityId = *static_cast<const uint32_t*>(payload->Data);
                    auto droppedEntity = static_cast<entt::entity>(modelEntityId);
                    const Model* droppedModel = registry.valid(droppedEntity)
                        ? registry.try_get<Model>(droppedEntity)
                        : nullptr;
                    applyModelToProp(*prop, droppedEntity, droppedModel);
                }
                ImGui::EndDragDropTarget();
            }
        }
    }

    if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Visible", &prop->visible);
        ImGui::Text("Opacity");
        ImGui::SetNextItemWidth(-1);
        entity::ui::SliderFloat("##prop_opacity", &prop->opacity, 0.0f, 1.0f);
        ImGui::TextDisabled("Stage view only; does not affect output.");
        ImGui::SetNextItemWidth(-1);
        ImGui::ColorEdit4("Tint", prop->displayColor.data(),
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar);
        ImGui::TextDisabled("Tint shades the matte fill in the 3D stage view.");
    }

    ImGui::PopID();
}

void PropertyWindow::renderProjectorProperties() {
    entt::entity selectedProjector = m_timeline->getSelectedProjector();
    if (selectedProjector == entt::null) return;

    auto& registry = m_timeline->getRegistry();
    Projector* proj = registry.try_get<Projector>(selectedProjector);
    if (!proj) {
        ImGui::TextDisabled("Invalid projector");
        return;
    }

    ImGui::PushID(static_cast<int>(selectedProjector));
    ImGui::Text("Projector: %s", proj->name.c_str());
    ImGui::Separator();

    // Identity
    if (ImGui::CollapsingHeader("Identity", ImGuiTreeNodeFlags_DefaultOpen)) {
        char nameBuf[256];
        strncpy(nameBuf, proj->name.c_str(), sizeof(nameBuf) - 1);
        nameBuf[sizeof(nameBuf) - 1] = '\0';
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf)))
            proj->name = nameBuf;
        ImGui::Checkbox("Enabled", &proj->enabled);
    }

    // Transform
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Position");
        ImGui::SetNextItemWidth(-1);
        entity::ui::DragFloat3("##ppos", proj->position.data(), 0.1f);

        ImGui::Text("Rotation  (pitch X, yaw Y, roll Z)");
        ImGui::SetNextItemWidth(-1);
        entity::ui::DragFloat3("##prot", proj->rotation.data(), 1.0f, -180.0f, 180.0f);

        if (ImGui::Button("Reset Transform")) {
            proj->position = {0.f, 3.f, 5.f};
            proj->rotation = {-20.f, 0.f, 0.f};
        }
    }

    // Optics
    if (ImGui::CollapsingHeader("Optics", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(-1);
        entity::ui::SliderFloat("FOV##fov", &proj->fovDegrees, 5.0f, 120.0f, "%.1f°");
        ImGui::SetNextItemWidth(-1);
        entity::ui::DragFloat("Near Clip##near", &proj->nearClip, 0.01f, 0.01f, 10.0f, "%.2f");
        ImGui::SetNextItemWidth(-1);
        entity::ui::DragFloat("Far Clip##far",  &proj->farClip,  1.0f,  1.0f,  500.0f, "%.1f");
        if (ImGui::SmallButton("Reset Optics")) {
            proj->fovDegrees = 50.f;
            proj->nearClip   = 0.1f;
            proj->farClip    = 50.f;
        }
    }

    // Target Surfaces
    if (ImGui::CollapsingHeader("Target Surfaces", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (proj->targetSurfaceCount == 0) {
            ImGui::TextDisabled("All Projection Surfaces (default)");
            if (ImGui::SmallButton("Override")) {
                // Switch to explicit mode: start with all current projection surfaces checked
                proj->targetSurfaceCount = 0;
                for (auto [e, scr] : registry.view<Screen>().each()) {
                    if (scr.type == ScreenType::ProjectionSurface &&
                        proj->targetSurfaceCount < Projector::MAX_TARGETS) {
                        proj->targetSurfaces[proj->targetSurfaceCount++] = e;
                    }
                }
                if (proj->targetSurfaceCount == 0)
                    proj->targetSurfaceCount = -1;  // explicit empty list
            }
        } else {
            ImGui::TextDisabled("Explicit target list:");
            // List all ProjectionSurface screens with checkboxes
            for (auto [e, scr] : registry.view<Screen>().each()) {
                if (scr.type != ScreenType::ProjectionSurface) continue;

                bool inList = false;
                int foundIdx = -1;
                for (int i = 0; i < proj->targetSurfaceCount; ++i) {
                    if (proj->targetSurfaces[i] == e) { inList = true; foundIdx = i; break; }
                }

                bool checked = inList;
                if (ImGui::Checkbox(scr.name.c_str(), &checked)) {
                    if (checked && !inList && proj->targetSurfaceCount < Projector::MAX_TARGETS) {
                        proj->targetSurfaces[proj->targetSurfaceCount++] = e;
                    } else if (!checked && inList && foundIdx >= 0) {
                        // Remove by shifting
                        for (int i = foundIdx; i < proj->targetSurfaceCount - 1; ++i)
                            proj->targetSurfaces[i] = proj->targetSurfaces[i + 1];
                        proj->targetSurfaceCount--;
                        if (proj->targetSurfaceCount < 0) proj->targetSurfaceCount = 0;
                    }
                }
            }
            if (ImGui::SmallButton("Reset to All")) {
                proj->targetSurfaceCount = 0;
            }
        }
    }

    ImGui::PopID();
}

// Phase A — Cue properties panel. Shows up when Timeline::setSelectedCueNumber
// is set (mutually exclusive with clip/screen/projector selection). Each edit
// commits via EditCueCommand with the captured pre-state so undo restores the
// original cue cleanly even when the cue's number changed.
void PropertyWindow::renderCueProperties() {
    auto selectedCueOpt = m_timeline->getSelectedCueNumber();
    if (!selectedCueOpt.has_value()) {
        ImGui::TextDisabled("No cue selected");
        return;
    }
    const double selectedNumber = *selectedCueOpt;
    const CueTag* live = m_timeline->findCueTag(selectedNumber);
    if (!live) {
        ImGui::TextDisabled("Cue %.2f no longer exists", selectedNumber);
        m_timeline->setSelectedCueNumber(std::nullopt);
        return;
    }

    ImGui::PushID(static_cast<int>(selectedNumber * 100.0));

    ImGui::Text("Cue %.2f", selectedNumber);
    ImGui::Separator();

    // Editable values are kept in local UI state so we only enqueue an
    // EditCueCommand on IsItemDeactivatedAfterEdit (mirrors the scalar
    // command pattern elsewhere in this window).
    static double editNumber = 0.0;
    static char editLabel[256] = {0};
    static long long editFrame = 0;
    static double trackedNumber = 0.0;  // detect selection change for re-init
    static CueTag preEditSnapshot{};
    static bool snapshotValid = false;

    if (trackedNumber != selectedNumber) {
        trackedNumber = selectedNumber;
        editNumber = live->number;
        std::strncpy(editLabel, live->label.c_str(), sizeof(editLabel) - 1);
        editLabel[sizeof(editLabel) - 1] = '\0';
        editFrame = static_cast<long long>(live->frame);
        snapshotValid = false;
    }

    auto captureSnapshot = [&]() {
        if (snapshotValid) return;
        if (const CueTag* cur = m_timeline->findCueTag(selectedNumber)) {
            preEditSnapshot = *cur;
            snapshotValid = true;
        }
    };

    auto commitEdit = [&]() {
        if (!m_dispatcher) return;
        const FrameNumber newFrame = static_cast<FrameNumber>(editFrame);
        auto cmd = std::make_unique<EditCueCommand>(
            selectedNumber, editNumber, newFrame, std::string(editLabel));
        if (snapshotValid) cmd->setPreviousState(preEditSnapshot);
        m_dispatcher->enqueue(std::move(cmd));
        snapshotValid = false;

        // The cue's number may have changed; track the new key so the panel
        // keeps showing the same cue after the dispatcher applies the edit.
        if (editNumber != selectedNumber) {
            m_timeline->setSelectedCueNumber(editNumber);
            trackedNumber = editNumber;
        }
    };

    entity::ui::InputDouble("Number", &editNumber, 0.1, 1.0, "%.2f");
    if (ImGui::IsItemActivated()) captureSnapshot();
    if (ImGui::IsItemDeactivatedAfterEdit()) commitEdit();

    ImGui::InputText("Label", editLabel, sizeof(editLabel));
    if (ImGui::IsItemActivated()) captureSnapshot();
    if (ImGui::IsItemDeactivatedAfterEdit()) commitEdit();

    entity::ui::InputScalar("Frame", ImGuiDataType_S64, &editFrame);
    if (ImGui::IsItemActivated()) captureSnapshot();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (editFrame < 0) editFrame = 0;
        commitEdit();
    }

    Timecode previewTs = m_timeline->frameToTime(static_cast<FrameNumber>(editFrame));
    double seconds = previewTs / 1000000.0;
    int totalSeconds = static_cast<int>(seconds);
    int hours = totalSeconds / 3600;
    int minutes = (totalSeconds % 3600) / 60;
    int secs = totalSeconds % 60;
    int frames = static_cast<int>((seconds - totalSeconds) * m_timeline->getFrameRate());
    ImGui::TextDisabled("%02d:%02d:%02d:%02d", hours, minutes, secs, frames);

    ImGui::Separator();

    if (ImGui::Button("Fire Cue") && m_dispatcher) {
        m_dispatcher->enqueue(std::make_unique<FireCueCommand>(selectedNumber));
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete Cue") && m_dispatcher) {
        m_dispatcher->enqueue(std::make_unique<RemoveCueCommand>(selectedNumber));
        m_timeline->setSelectedCueNumber(std::nullopt);
    }

    ImGui::PopID();
}

// Phase B — Section break properties panel. Active when
// Timeline::setSelectedSectionBreak is set. Frame / color edits commit
// via EditSectionBreakCommand; fadeSeconds is shown disabled until Phase D.
void PropertyWindow::renderSectionBreakProperties() {
    auto sel = m_timeline->getSelectedSectionBreak();
    if (!sel.has_value()) {
        ImGui::TextDisabled("No section break selected");
        return;
    }
    const FrameNumber selFrame = *sel;
    const Timeline::Section* live = m_timeline->findSectionBreakNear(selFrame, 0);
    if (!live) {
        ImGui::TextDisabled("Section break at %lld no longer exists",
                            static_cast<long long>(selFrame));
        m_timeline->setSelectedSectionBreak(std::nullopt);
        return;
    }

    ImGui::PushID(static_cast<int>(selFrame & 0x7fffffff));

    ImGui::Text("Section Break");
    ImGui::Separator();

    static FrameNumber trackedFrame = -1;
    static long long editFrame = 0;
    static float editColor[4] = {0, 0, 0, 0};
    static double editFadeSeconds = 0.0;
    static Timeline::Section preEditSnapshot{};
    static bool snapshotValid = false;

    if (trackedFrame != selFrame) {
        trackedFrame = selFrame;
        editFrame = static_cast<long long>(live->breakFrame);
        editColor[0] = ((live->color >> 0)  & 0xFFu) / 255.0f;
        editColor[1] = ((live->color >> 8)  & 0xFFu) / 255.0f;
        editColor[2] = ((live->color >> 16) & 0xFFu) / 255.0f;
        editColor[3] = ((live->color >> 24) & 0xFFu) / 255.0f;
        editFadeSeconds = live->fadeSeconds;
        snapshotValid = false;
    }

    auto packColor = [&]() -> uint32_t {
        ImU32 r = static_cast<ImU32>(editColor[0] * 255.0f) & 0xFFu;
        ImU32 g = static_cast<ImU32>(editColor[1] * 255.0f) & 0xFFu;
        ImU32 b = static_cast<ImU32>(editColor[2] * 255.0f) & 0xFFu;
        ImU32 a = static_cast<ImU32>(editColor[3] * 255.0f) & 0xFFu;
        return (a << 24) | (b << 16) | (g << 8) | r;
    };

    auto captureSnapshot = [&]() {
        if (snapshotValid) return;
        if (const Timeline::Section* cur = m_timeline->findSectionBreakNear(selFrame, 0)) {
            preEditSnapshot = *cur;
            snapshotValid = true;
        }
    };

    auto commitEdit = [&]() {
        if (!m_dispatcher) return;
        const FrameNumber newFrame = static_cast<FrameNumber>(editFrame);
        auto cmd = std::make_unique<EditSectionBreakCommand>(
            selFrame, newFrame, packColor(), editFadeSeconds);
        if (snapshotValid) cmd->setPreviousState(preEditSnapshot);
        m_dispatcher->enqueue(std::move(cmd));
        snapshotValid = false;
        if (newFrame != selFrame) {
            m_timeline->setSelectedSectionBreak(newFrame);
            trackedFrame = newFrame;
        }
    };

    entity::ui::InputScalar("Frame", ImGuiDataType_S64, &editFrame);
    if (ImGui::IsItemActivated()) captureSnapshot();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (editFrame < 0) editFrame = 0;
        commitEdit();
    }

    if (ImGui::ColorEdit4("Color", editColor, ImGuiColorEditFlags_NoInputs)) {
        captureSnapshot();
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) commitEdit();

    entity::ui::InputDouble("Fade (seconds)", &editFadeSeconds, 0.05, 0.5, "%.2f");
    if (ImGui::IsItemActivated()) captureSnapshot();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (editFadeSeconds < 0.0) editFadeSeconds = 0.0;
        commitEdit();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Auto-fade duration applied to clips that start or end at this break.");
    }

    ImGui::Separator();
    if (ImGui::Button("Jump to Break") && m_dispatcher) {
        m_dispatcher->enqueue(std::make_unique<SeekCommand>(selFrame));
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete Break") && m_dispatcher) {
        m_dispatcher->enqueue(std::make_unique<RemoveSectionBreakCommand>(selFrame));
        m_timeline->setSelectedSectionBreak(std::nullopt);
    }

    ImGui::PopID();
}

void PropertyWindow::renderOALayerProperties(entt::entity entity) {
    if (!m_timeline) return;
    auto& registry = m_timeline->getRegistry();

    auto* oal   = registry.try_get<ObjectAnimationLayer>(entity);
    auto* lay   = registry.try_get<Layer>(entity);
    auto* animP = registry.try_get<AnimatedProperties>(entity);
    if (!oal || !lay) return;

    ImGui::PushID(static_cast<int>(entity));

    ImGui::Text("Object Animation Layer");
    ImGui::Separator();

    // Prominent no-target banner. Sits above every other section so it can't
    // be hidden by a collapsed Target header — the most common reason
    // "dragging a slider does nothing" is the layer not pointing at the
    // screen the operator thinks it is.
    if (oal->target == entt::null) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.20f, 1.0f));
        ImGui::TextWrapped("WARNING: No target assigned. Pick a Screen or Prop "
                           "in the Target section below — keyframes have no "
                           "visible effect until a target is set.");
        ImGui::PopStyleColor();
        ImGui::Separator();
    } else if (!registry.valid(oal->target)) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.20f, 1.0f));
        ImGui::TextWrapped("WARNING: Target entity is gone (deleted?). Pick a "
                           "new Screen or Prop in the Target section below.");
        ImGui::PopStyleColor();
        ImGui::Separator();
    } else {
        // Show what the layer is actually driving — answers the operator's
        // "is this animating what I expect?" question without scrolling.
        std::string targetLabel = "Target: ";
        if (registry.all_of<Screen>(oal->target)) {
            targetLabel += "Screen \"";
            targetLabel += registry.get<Screen>(oal->target).name;
            targetLabel += "\"";
        } else if (registry.all_of<Prop>(oal->target)) {
            targetLabel += "Prop \"";
            targetLabel += registry.get<Prop>(oal->target).name;
            targetLabel += "\"";
        } else {
            targetLabel += "(unknown kind)";
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.8f, 1.0f, 1.0f));
        ImGui::TextUnformatted(targetLabel.c_str());
        ImGui::PopStyleColor();
        ImGui::Separator();
    }

    // Identity
    if (ImGui::CollapsingHeader("Identity", ImGuiTreeNodeFlags_DefaultOpen)) {
        char nameBuf[128];
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", lay->name.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) {
            lay->name = nameBuf;
        }

        // Color swatch
        float col[4] = {lay->color[0], lay->color[1], lay->color[2], lay->color[3]};
        if (ImGui::ColorEdit4("Color", col, ImGuiColorEditFlags_NoAlpha)) {
            lay->color = {col[0], col[1], col[2], col[3]};
        }
    }

    // Timing — start frame and duration editable via DragInt with the
    // canonical IsItemActivated -> live write -> IsItemDeactivatedAfterEdit
    // -> enqueue pattern (same as Clip In Point editor).
    if (ImGui::CollapsingHeader("Timing", ImGuiTreeNodeFlags_DefaultOpen)) {
        int oaStart = static_cast<int>(lay->startFrame);
        bool oaStartChanged = entity::ui::DragInt("Start Frame##oaTiming", &oaStart, 1.0f, 0,
                                                  std::numeric_limits<int>::max(),
                                                  "%d", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemActivated()) {
            m_preEditLayerStartFrame = lay->startFrame;
        }
        if (oaStartChanged) {
            lay->startFrame = static_cast<FrameNumber>(oaStart);
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && m_dispatcher) {
            if (auto idx = findClipIndices(m_timeline, entity)) {
                auto cmd = std::make_unique<SetLayerStartFrameCommand>(
                    idx->first, idx->second, lay->startFrame);
                cmd->setPreviousStartFrame(m_preEditLayerStartFrame);
                m_dispatcher->enqueue(std::move(cmd));
            }
        }

        int oaDur = static_cast<int>(lay->duration);
        bool oaDurChanged = entity::ui::DragInt("Duration##oaTiming", &oaDur, 1.0f, 1,
                                                std::numeric_limits<int>::max(),
                                                "%d frames", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemActivated()) {
            m_preEditLayerDuration = lay->duration;
        }
        if (oaDurChanged) {
            lay->duration = static_cast<FrameNumber>(oaDur);
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && m_dispatcher) {
            if (auto idx = findClipIndices(m_timeline, entity)) {
                auto cmd = std::make_unique<SetLayerDurationCommand>(
                    idx->first, idx->second, lay->duration);
                cmd->setPreviousDuration(m_preEditLayerDuration);
                m_dispatcher->enqueue(std::move(cmd));
            }
        }

        const char* kEndBehaviorLabels[] = {"Hold last value", "Reset to base"};
        int ebIdx = static_cast<int>(oal->endBehavior);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("End behavior", &ebIdx, kEndBehaviorLabels, 2)) {
            oal->endBehavior =
                static_cast<ObjectAnimationLayer::EndBehavior>(ebIdx);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Hold: target stays at last evaluated values after the\n"
                "layer ends (recommended).\n\n"
                "Reset: target snaps back to its Stage-configured base\n"
                "the frame the layer ends.");
        }
    }

    // Target picker — Screen + Prop entities. Sits after Timing so the
    // canonical section order (Identity -> Timing -> kind-specific) is
    // respected across all content-producing layer kinds.
    if (ImGui::CollapsingHeader("Target", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextDisabled("Screen or Prop to animate:");

        // Build target list: None + all Screen entities + all Prop entities
        std::vector<entt::entity> targets;
        std::vector<std::string>  targetLabels;
        targets.push_back(entt::null);
        targetLabels.push_back("(none)");

        auto screenView = registry.view<Screen>();
        for (auto e : screenView) {
            targets.push_back(e);
            const auto& s = screenView.get<Screen>(e);
            targetLabels.push_back("Screen: " + s.name);
        }

        auto propView = registry.view<Prop>();
        for (auto e : propView) {
            targets.push_back(e);
            targetLabels.push_back("Prop: " + std::to_string(static_cast<uint32_t>(e)));
        }

        // Build c-string array for Combo
        std::vector<const char*> targetCstrs;
        targetCstrs.reserve(targetLabels.size());
        for (const auto& l : targetLabels) targetCstrs.push_back(l.c_str());

        int currentIdx = 0;
        for (size_t i = 0; i < targets.size(); ++i) {
            if (targets[i] == oal->target) { currentIdx = static_cast<int>(i); break; }
        }

        ImGui::SetNextItemWidth(-1);
        if (ImGui::Combo("##oatarget", &currentIdx, targetCstrs.data(), static_cast<int>(targetCstrs.size()))) {
            oal->target = targets[static_cast<size_t>(currentIdx)];
        }

        if (oal->target == entt::null) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                               "No target — animation has no effect until a target is chosen.");
        }
    }

    // Interactive keyframe + value-edit controls for each animatable OA axis.
    // Each row pairs renderOAKeyframeControls (stopwatch / prev / diamond /
    // next) with a DragFloat that auto-creates a keyframe at the current
    // layer-local frame on drag. Playhead outside the layer window disables
    // the slider — telegraphs why the drag has no effect.
    if (ImGui::CollapsingHeader("Keyframes", ImGuiTreeNodeFlags_DefaultOpen)) {
        int oaFrame = getCurrentOAFrame(entity);
        if (oaFrame < 0) {
            ImGui::TextDisabled("Move playhead inside the layer to add keyframes.");
        } else {
            ImGui::TextDisabled("Frame %d (layer-local)", oaFrame);
        }
        ImGui::Spacing();

        renderOAValueRow(entity, AnimatableProperty::PositionX, "Position X",
                         0.01f, -50.0f, 50.0f, "%.3f");
        renderOAValueRow(entity, AnimatableProperty::PositionY, "Position Y",
                         0.01f, -50.0f, 50.0f, "%.3f");
        renderOAValueRow(entity, AnimatableProperty::PositionZ, "Position Z",
                         0.01f, -50.0f, 50.0f, "%.3f");
        renderOAValueRow(entity, AnimatableProperty::RotationX, "Rotation X",
                         0.5f,  -360.0f, 360.0f, "%.1f deg");
        renderOAValueRow(entity, AnimatableProperty::RotationY, "Rotation Y",
                         0.5f,  -360.0f, 360.0f, "%.1f deg");
        renderOAValueRow(entity, AnimatableProperty::RotationZ, "Rotation Z",
                         0.5f,  -360.0f, 360.0f, "%.1f deg");
        renderOAValueRow(entity, AnimatableProperty::ScaleX,    "Scale X",
                         0.01f, 0.01f, 10.0f, "%.3f");
        renderOAValueRow(entity, AnimatableProperty::ScaleY,    "Scale Y",
                         0.01f, 0.01f, 10.0f, "%.3f");
        renderOAValueRow(entity, AnimatableProperty::ScaleZ,    "Scale Z",
                         0.01f, 0.01f, 10.0f, "%.3f");
    }

    ImGui::PopID();
}

void PropertyWindow::renderGenerativeLayerProperties(entt::entity entity) {
    if (!m_timeline) return;
    auto& registry = m_timeline->getRegistry();

    auto* gen   = registry.try_get<GenerativeLayer>(entity);
    auto* lay   = registry.try_get<Layer>(entity);
    auto* media = registry.try_get<MediaLayer>(entity);
    if (!gen || !lay) return;

    ImGui::PushID(static_cast<int>(entity));

    // Sub-kind label resolved by composition (MunchersGameState => "Muncher",
    // TextLayerState => "Text", etc.).
    const char* subKind = "Generative";
    if (registry.all_of<MunchersGameState>(entity)) subKind = "Muncher";
    if (registry.all_of<TextLayerState>(entity))    subKind = "Text";
    ImGui::Text("%s Layer", subKind);
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Identity", ImGuiTreeNodeFlags_DefaultOpen)) {
        char nameBuf[128];
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", lay->name.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) {
            lay->name = nameBuf;
        }

        float col[4] = {lay->color[0], lay->color[1], lay->color[2], lay->color[3]};
        if (ImGui::ColorEdit4("Color", col, ImGuiColorEditFlags_NoAlpha)) {
            lay->color = {col[0], col[1], col[2], col[3]};
        }
    }

    if (ImGui::CollapsingHeader("Timing")) {
        int genStart = static_cast<int>(lay->startFrame);
        bool genStartChanged = entity::ui::DragInt("Start Frame##genTiming", &genStart, 1.0f, 0,
                                                   std::numeric_limits<int>::max(),
                                                   "%d", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemActivated()) {
            m_preEditLayerStartFrame = lay->startFrame;
        }
        if (genStartChanged) {
            lay->startFrame = static_cast<FrameNumber>(genStart);
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && m_dispatcher) {
            if (auto idx = findClipIndices(m_timeline, entity)) {
                auto cmd = std::make_unique<SetLayerStartFrameCommand>(
                    idx->first, idx->second, lay->startFrame);
                cmd->setPreviousStartFrame(m_preEditLayerStartFrame);
                m_dispatcher->enqueue(std::move(cmd));
            }
        }

        int genDur = static_cast<int>(lay->duration);
        bool genDurChanged = entity::ui::DragInt("Duration##genTiming", &genDur, 1.0f, 1,
                                                 std::numeric_limits<int>::max(),
                                                 "%d frames", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemActivated()) {
            m_preEditLayerDuration = lay->duration;
        }
        if (genDurChanged) {
            lay->duration = static_cast<FrameNumber>(genDur);
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && m_dispatcher) {
            if (auto idx = findClipIndices(m_timeline, entity)) {
                auto cmd = std::make_unique<SetLayerDurationCommand>(
                    idx->first, idx->second, lay->duration);
                cmd->setPreviousDuration(m_preEditLayerDuration);
                m_dispatcher->enqueue(std::move(cmd));
            }
        }
    }

    // UV-space Transform — same axes as Clip's transform (position in screen
    // NDC, rotation Z, scale around screen center). Each channel carries the
    // stopwatch / keyframe-nav controls; AnimationSystem evaluates the tracks
    // for Generative layers via the Layer-based clip-local frame. Same widgets
    // and keyframe wiring as Clip's renderTransformSection.
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (auto* t = registry.try_get<Transform>(entity)) {
            renderKeyframeControls(AnimatableProperty::PositionX, "Position X", t->position.x);
            ImGui::SameLine();
            ImGui::Text("Position X");
            ImGui::SetNextItemWidth(-1);
            float posX = t->position.x;
            if (entity::ui::DragFloat("##genPosX", &posX, 0.01f, -2.0f, 2.0f, "%.3f")) {
                t->setPosition(glm::vec3(posX, t->position.y, t->position.z));
                updateKeyframeOnValueChange(AnimatableProperty::PositionX, posX);
            }

            renderKeyframeControls(AnimatableProperty::PositionY, "Position Y", t->position.y);
            ImGui::SameLine();
            ImGui::Text("Position Y");
            ImGui::SetNextItemWidth(-1);
            float posY = t->position.y;
            if (entity::ui::DragFloat("##genPosY", &posY, 0.01f, -2.0f, 2.0f, "%.3f")) {
                t->setPosition(glm::vec3(t->position.x, posY, t->position.z));
                updateKeyframeOnValueChange(AnimatableProperty::PositionY, posY);
            }

            ImGui::Spacing();
            renderKeyframeControls(AnimatableProperty::Rotation, "Rotation", t->rotation.z);
            ImGui::SameLine();
            ImGui::Text("Rotation");
            ImGui::SetNextItemWidth(-1);
            float rotZ = t->rotation.z;
            if (entity::ui::DragFloat("##genRotZ", &rotZ, 0.5f, -360.0f, 360.0f, "%.1f deg")) {
                t->setRotation(glm::vec3(t->rotation.x, t->rotation.y, rotZ));
                updateKeyframeOnValueChange(AnimatableProperty::Rotation, rotZ);
            }

            ImGui::Spacing();
            bool uniformScale = registry.get_or_emplace<ScaleLock>(entity).uniform;
            if (ImGui::Checkbox("Uniform Scale", &uniformScale) && m_dispatcher) {
                m_dispatcher->enqueue(
                    std::make_unique<SetScaleLockCommand>(entity, uniformScale));
            }

            renderKeyframeControls(AnimatableProperty::ScaleX, "Scale X", t->scale.x);
            ImGui::SameLine();
            ImGui::Text("Scale X");
            ImGui::SetNextItemWidth(-1);
            float scaleX = t->scale.x;
            float prevScaleX = scaleX;
            if (entity::ui::DragFloat("##genScaleX", &scaleX, 0.01f, 0.01f, 10.0f, "%.3f")) {
                if (uniformScale && prevScaleX > 0.0001f) {
                    float ratio = scaleX / prevScaleX;
                    float newScaleY = t->scale.y * ratio;
                    t->setScale(glm::vec3(scaleX, newScaleY, t->scale.z * ratio));
                    updateKeyframeOnValueChange(AnimatableProperty::ScaleX, scaleX);
                    updateKeyframeOnValueChange(AnimatableProperty::ScaleY, newScaleY);
                } else {
                    t->setScale(glm::vec3(scaleX, t->scale.y, t->scale.z));
                    updateKeyframeOnValueChange(AnimatableProperty::ScaleX, scaleX);
                }
            }

            renderKeyframeControls(AnimatableProperty::ScaleY, "Scale Y", t->scale.y);
            ImGui::SameLine();
            ImGui::Text("Scale Y");
            ImGui::SetNextItemWidth(-1);
            float scaleY = t->scale.y;
            float prevScaleY = scaleY;
            if (entity::ui::DragFloat("##genScaleY", &scaleY, 0.01f, 0.01f, 10.0f, "%.3f")) {
                if (uniformScale && prevScaleY > 0.0001f) {
                    float ratio = scaleY / prevScaleY;
                    float newScaleX = t->scale.x * ratio;
                    t->setScale(glm::vec3(newScaleX, scaleY, t->scale.z * ratio));
                    updateKeyframeOnValueChange(AnimatableProperty::ScaleX, newScaleX);
                    updateKeyframeOnValueChange(AnimatableProperty::ScaleY, scaleY);
                } else {
                    t->setScale(glm::vec3(t->scale.x, scaleY, t->scale.z));
                    updateKeyframeOnValueChange(AnimatableProperty::ScaleY, scaleY);
                }
            }

            ImGui::Spacing();
            if (ImGui::Button("Reset Transform")) {
                t->setPosition(glm::vec3(0.0f));
                t->setRotation(glm::vec3(0.0f));
                t->setScale(glm::vec3(1.0f));
            }
        } else {
            ImGui::TextDisabled("No transform component");
        }
    }

    if (ImGui::CollapsingHeader("Layer", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (media) {
            renderKeyframeControls(AnimatableProperty::Opacity, "Opacity", media->opacity);
            ImGui::SameLine();
            ImGui::Text("Opacity");
            ImGui::SetNextItemWidth(-1);
            float opacity = media->opacity;
            if (entity::ui::SliderFloat("##genOpacity", &opacity, 0.0f, 1.0f, "%.2f")) {
                media->opacity = opacity;
                updateKeyframeOnValueChange(AnimatableProperty::Opacity, opacity);
            }

            int zOrder = static_cast<int>(media->zOrder);
            ImGui::SetNextItemWidth(-1);
            if (entity::ui::DragInt("Z-Order", &zOrder, 1.0f, 0, 999)) {
                media->zOrder = static_cast<uint32_t>(std::max(0, zOrder));
            }
        } else {
            ImGui::TextDisabled("No layer component");
        }
    }

    if (ImGui::CollapsingHeader("Content Routing", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Shared picker (ADR-0022). renderContentRoutingPicker handles the
        // combo, ContentRoutingRef write, kind badge, and Edit link.
        // Generative side additionally keeps gen->targetScreen in sync with
        // the selected asset's single-screen target (legacy alias).
        if (renderContentRoutingPicker(entity, "gencontentrouting")) {
            const auto* newRef = registry.try_get<ContentRoutingRef>(entity);
            const entt::entity newAsset = newRef ? newRef->asset : entt::null;
            const auto* newAssetData = (newAsset != entt::null)
                ? registry.try_get<ContentRoutingAsset>(newAsset) : nullptr;

            entt::entity legacyScreen = entt::null;
            if (newAssetData &&
                newAssetData->kind == RouteMode::Direct &&
                newAssetData->targets.size() == 1) {
                legacyScreen = newAssetData->targets.front().screen;
            }
            gen->targetScreen = legacyScreen;
        }

        if (gen->targetScreen == entt::null) {
            const auto* asset = routing::tryGetAsset(registry, entity);
            if (!asset) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                                   "No target -- generated output has no destination.");
            }
        }
    }

    if (ImGui::CollapsingHeader("Render Target")) {
        const std::uint32_t prevW = gen->renderWidth;
        const std::uint32_t prevH = gen->renderHeight;
        int w = static_cast<int>(gen->renderWidth);
        int h = static_cast<int>(gen->renderHeight);
        ImGui::SetNextItemWidth(-1);
        if (entity::ui::DragInt("Width",  &w, 1.0f, 16, 7680)) {
            gen->renderWidth = static_cast<std::uint32_t>(std::max(16, w));
            if (auto* tls = registry.try_get<TextLayerState>(entity)) tls->dirty = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && m_dispatcher &&
            gen->renderWidth != prevW) {
            auto cmd = std::make_unique<SetGenerativeRenderSizeCommand>(
                entity, gen->renderWidth, gen->renderHeight);
            cmd->setPreviousSize(prevW, gen->renderHeight);
            m_dispatcher->enqueue(std::move(cmd));
        }
        ImGui::SetNextItemWidth(-1);
        if (entity::ui::DragInt("Height", &h, 1.0f, 16, 4320)) {
            gen->renderHeight = static_cast<std::uint32_t>(std::max(16, h));
            if (auto* tls = registry.try_get<TextLayerState>(entity)) tls->dirty = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && m_dispatcher &&
            gen->renderHeight != prevH) {
            auto cmd = std::make_unique<SetGenerativeRenderSizeCommand>(
                entity, gen->renderWidth, gen->renderHeight);
            cmd->setPreviousSize(gen->renderWidth, prevH);
            m_dispatcher->enqueue(std::move(cmd));
        }
        ImGui::TextDisabled("Slot: %d", gen->renderTargetSlot);
    }

    // ---- Text layer properties (Text sub-kind only) ----
    if (auto* tls = registry.try_get<TextLayerState>(entity)) {
        if (ImGui::CollapsingHeader("Text", ImGuiTreeNodeFlags_DefaultOpen)) {

            // Content
            {
                ImGui::TextDisabled("Content");
                char textBuf[4096];
                std::snprintf(textBuf, sizeof(textBuf), "%s", tls->text.c_str());
                ImGui::SetNextItemWidth(-1);
                const bool changed = ImGui::InputTextMultiline(
                    "##textcontent", textBuf, sizeof(textBuf),
                    ImVec2(-1.0f, ImGui::GetTextLineHeightWithSpacing() * 5));
                if (ImGui::IsItemActivated()) m_preEditTextContent = tls->text;
                if (changed) {
                    tls->text  = textBuf;
                    tls->dirty = true;
                }
                if (ImGui::IsItemDeactivatedAfterEdit() && m_dispatcher) {
                    auto cmd = std::make_unique<SetTextContentCommand>(entity, tls->text);
                    cmd->setPreviousText(m_preEditTextContent);
                    m_dispatcher->enqueue(std::move(cmd));
                }
            }

            ImGui::Spacing();

            // Font family dropdown — populated once from enumerateSystemFonts().
            // Cache is populated on first open; ~50ms one-shot cost is acceptable.
            static std::vector<std::string> s_fontNames;
            static std::vector<const char*> s_fontCStrs;
            if (s_fontNames.empty()) {
                TextRasterizer tmpRasterizer;
                s_fontNames = tmpRasterizer.enumerateSystemFonts();
                s_fontCStrs.clear();
                s_fontCStrs.reserve(s_fontNames.size());
                for (const auto& n : s_fontNames) s_fontCStrs.push_back(n.c_str());
            }

            if (!s_fontCStrs.empty()) {
                int fontIdx = 0;
                for (int i = 0; i < static_cast<int>(s_fontNames.size()); ++i) {
                    if (s_fontNames[i] == tls->fontFamily) { fontIdx = i; break; }
                }
                ImGui::TextDisabled("Font Family");
                ImGui::SetNextItemWidth(-1);
                if (ImGui::Combo("##fontfamily", &fontIdx,
                                  s_fontCStrs.data(),
                                  static_cast<int>(s_fontCStrs.size()))) {
                    const std::string prevFont = tls->fontFamily;
                    tls->fontFamily = s_fontNames[fontIdx];
                    tls->dirty      = true;
                    if (m_dispatcher) {
                        auto cmd = std::make_unique<SetTextFontCommand>(
                            entity, tls->fontFamily);
                        cmd->setPreviousFont(prevFont);
                        m_dispatcher->enqueue(std::move(cmd));
                    }
                }
            }

            ImGui::Spacing();

            // Font size
            {
                ImGui::SetNextItemWidth(-1);
                if (entity::ui::DragFloat("Font Size", &tls->fontSize, 1.0f, 8.0f, 512.0f, "%.1f pt")) {
                    tls->dirty = true;
                }
                if (ImGui::IsItemActivated()) m_preEditTextFontSize = tls->fontSize;
                if (ImGui::IsItemDeactivatedAfterEdit() && m_dispatcher) {
                    auto cmd = std::make_unique<SetTextFontSizeCommand>(entity, tls->fontSize);
                    cmd->setPreviousFontSize(m_preEditTextFontSize);
                    m_dispatcher->enqueue(std::move(cmd));
                }
            }

            ImGui::Spacing();

            // Color
            {
                float col[4] = {tls->color.r, tls->color.g, tls->color.b, tls->color.a};
                ImGui::SetNextItemWidth(-1);
                if (ImGui::ColorEdit4("##textcolor", col)) {
                    tls->color = {col[0], col[1], col[2], col[3]};
                    tls->dirty = true;
                }
                if (ImGui::IsItemActivated()) {
                    m_preEditTextColorR = tls->color.r; m_preEditTextColorG = tls->color.g;
                    m_preEditTextColorB = tls->color.b; m_preEditTextColorA = tls->color.a;
                }
                if (ImGui::IsItemDeactivatedAfterEdit() && m_dispatcher) {
                    auto cmd = std::make_unique<SetTextColorCommand>(
                        entity, tls->color.r, tls->color.g, tls->color.b, tls->color.a);
                    cmd->setPreviousColor(
                        m_preEditTextColorR, m_preEditTextColorG,
                        m_preEditTextColorB, m_preEditTextColorA);
                    m_dispatcher->enqueue(std::move(cmd));
                }
                ImGui::SameLine();
                ImGui::TextDisabled("Color");
            }

            ImGui::Spacing();

            // Alignment combo
            {
                static const char* alignLabels[] = { "Left", "Center", "Right" };
                int alignIdx = static_cast<int>(tls->alignment);
                ImGui::TextDisabled("Alignment");
                ImGui::SetNextItemWidth(-1);
                if (ImGui::Combo("##textalign", &alignIdx, alignLabels, 3)) {
                    const uint8_t prev = static_cast<uint8_t>(tls->alignment);
                    tls->alignment     = static_cast<TextAlignment>(alignIdx);
                    tls->dirty         = true;
                    if (m_dispatcher) {
                        auto cmd = std::make_unique<SetTextAlignmentCommand>(
                            entity, static_cast<uint8_t>(alignIdx));
                        cmd->setPreviousAlignment(prev);
                        m_dispatcher->enqueue(std::move(cmd));
                    }
                }
            }

            ImGui::Spacing();

            // Bold / Italic checkboxes
            {
                bool bold = tls->bold;
                if (ImGui::Checkbox("Bold", &bold)) {
                    tls->bold  = bold;
                    tls->dirty = true;
                    if (m_dispatcher) {
                        auto cmd = std::make_unique<SetTextBoldCommand>(entity, bold);
                        cmd->setPreviousBold(!bold);
                        m_dispatcher->enqueue(std::move(cmd));
                    }
                }
                ImGui::SameLine();
                bool italic = tls->italic;
                if (ImGui::Checkbox("Italic", &italic)) {
                    tls->italic = italic;
                    tls->dirty  = true;
                    if (m_dispatcher) {
                        auto cmd = std::make_unique<SetTextItalicCommand>(entity, italic);
                        cmd->setPreviousItalic(!italic);
                        m_dispatcher->enqueue(std::move(cmd));
                    }
                }
            }

            ImGui::Spacing();

            // Bake info (read-only)
            if (tls->bakedWidth > 0 && tls->bakedHeight > 0) {
                ImGui::TextDisabled("Baked: %u x %u  slot %d",
                                    tls->bakedWidth, tls->bakedHeight,
                                    tls->textureSlot);
            } else {
                ImGui::TextDisabled("Not yet baked");
            }
        }
    }

    // Remote Control section (ADR-0028).
    if (ImGui::CollapsingHeader("Remote Control")) {
        renderRemoteControlSection(entity);
    }

    ImGui::PopID();
}

// ============================================================================
// Signal Output Layer properties
// ============================================================================

void PropertyWindow::renderSignalLayerProperties(entt::entity entity) {
    if (!m_timeline) return;
    auto& registry = m_timeline->getRegistry();

    auto* sig = registry.try_get<SignalLayer>(entity);
    auto* lay = registry.try_get<Layer>(entity);
    if (!sig || !lay) return;

    ImGui::PushID(static_cast<int>(entity));

    ImGui::Text("Signal Layer");
    ImGui::Separator();

    // -- Identity ----------------------------------------------------------
    if (ImGui::CollapsingHeader("Identity", ImGuiTreeNodeFlags_DefaultOpen)) {
        char nameBuf[128];
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", lay->name.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf)))
            lay->name = nameBuf;

        float col[4] = {lay->color[0], lay->color[1], lay->color[2], lay->color[3]};
        if (ImGui::ColorEdit4("Color", col, ImGuiColorEditFlags_NoAlpha))
            lay->color = {col[0], col[1], col[2], col[3]};
    }

    // -- Timing ----------------------------------------------------------------
    if (ImGui::CollapsingHeader("Timing")) {
        int sigStart = static_cast<int>(lay->startFrame);
        bool sigStartChanged = entity::ui::DragInt("Start Frame##sigTiming", &sigStart, 1.0f, 0,
                                                   std::numeric_limits<int>::max(),
                                                   "%d", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemActivated()) {
            m_preEditLayerStartFrame = lay->startFrame;
        }
        if (sigStartChanged) {
            lay->startFrame = static_cast<FrameNumber>(sigStart);
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && m_dispatcher) {
            if (auto idx = findClipIndices(m_timeline, entity)) {
                auto cmd = std::make_unique<SetLayerStartFrameCommand>(
                    idx->first, idx->second, lay->startFrame);
                cmd->setPreviousStartFrame(m_preEditLayerStartFrame);
                m_dispatcher->enqueue(std::move(cmd));
            }
        }

        int sigDur = static_cast<int>(lay->duration);
        bool sigDurChanged = entity::ui::DragInt("Duration##sigTiming", &sigDur, 1.0f, 1,
                                                 std::numeric_limits<int>::max(),
                                                 "%d frames", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemActivated()) {
            m_preEditLayerDuration = lay->duration;
        }
        if (sigDurChanged) {
            lay->duration = static_cast<FrameNumber>(sigDur);
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && m_dispatcher) {
            if (auto idx = findClipIndices(m_timeline, entity)) {
                auto cmd = std::make_unique<SetLayerDurationCommand>(
                    idx->first, idx->second, lay->duration);
                cmd->setPreviousDuration(m_preEditLayerDuration);
                m_dispatcher->enqueue(std::move(cmd));
            }
        }
    }

    // -- Signal parameters -------------------------------------------------
    if (ImGui::CollapsingHeader("Signal", ImGuiTreeNodeFlags_DefaultOpen)) {

        // Mode combo
        {
            const char* modeItems[] = {"Momentary", "Continuous"};
            int modeIdx = sig->mode == SignalLayer::Mode::Momentary ? 0 : 1;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("Mode", &modeIdx, modeItems, 2))
                sig->mode = (modeIdx == 0) ? SignalLayer::Mode::Momentary
                                           : SignalLayer::Mode::Continuous;
            // Carry-forward hint (Phase 5 review): momentary fires on forward crossing.
            // ASCII-only tooltip per project rules.
            if (sig->mode == SignalLayer::Mode::Momentary) {
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Cue fires when the playhead crosses the layer start\n"
                        "during forward play. Seek before the marker to arm.");
            }
        }

        // Transport combo (OSC only in v1; reserved for MIDI etc.)
        {
            const char* transportItems[] = {"OSC"};
            int tIdx = 0; // only OSC exists
            ImGui::SetNextItemWidth(-1);
            ImGui::Combo("Transport", &tIdx, transportItems, 1);
            sig->transport = SignalLayer::Transport::OSC;
        }

        // OSC address
        {
            char addrBuf[256];
            std::snprintf(addrBuf, sizeof(addrBuf), "%s", sig->address.c_str());
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("OSC Address", addrBuf, sizeof(addrBuf)))
                sig->address = addrBuf;
        }

        // Continuous-only: value source + range mapping
        if (sig->mode == SignalLayer::Mode::Continuous) {
            ImGui::Separator();
            ImGui::TextDisabled("Value Source");

            const char* vsItems[] = {"Progress", "Keyframe"};
            int vsIdx = sig->valueSource == SignalLayer::ValueSource::Progress ? 0 : 1;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("Value Source", &vsIdx, vsItems, 2))
                sig->valueSource = (vsIdx == 0) ? SignalLayer::ValueSource::Progress
                                                : SignalLayer::ValueSource::Keyframe;

            ImGui::Separator();
            ImGui::TextDisabled("Range Mapping  src -> out");

            float srcRange[2] = {sig->srcMin, sig->srcMax};
            ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat2("Src Min/Max", srcRange, 0.01f)) {
                sig->srcMin = srcRange[0];
                sig->srcMax = srcRange[1];
            }
            float outRange[2] = {sig->outMin, sig->outMax};
            ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat2("Out Min/Max", outRange, 0.01f)) {
                sig->outMin = outRange[0];
                sig->outMax = outRange[1];
            }

            const char* outTypeItems[] = {"Int", "Float", "String"};
            int otIdx = static_cast<int>(sig->outType);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("Out Type", &otIdx, outTypeItems, 3))
                sig->outType = static_cast<SignalLayer::Arg::Type>(otIdx);
        }
    }

    // -- Arg list editor ---------------------------------------------------
    if (ImGui::CollapsingHeader("Args", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* argTypeNames[] = {"Int", "Float", "String"};
        const bool isContinuous = (sig->mode == SignalLayer::Mode::Continuous);

        for (int ai = 0; ai < static_cast<int>(sig->args.size()); ++ai) {
            auto& arg = sig->args[ai];
            ImGui::PushID(ai);

            // Type combo
            int typeIdx = static_cast<int>(arg.type);
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::Combo("##type", &typeIdx, argTypeNames, 3))
                arg.type = static_cast<SignalLayer::Arg::Type>(typeIdx);

            ImGui::SameLine();

            // Value field, width adjusted for Remove button
            ImGui::SetNextItemWidth(-140.0f);
            switch (arg.type) {
                case SignalLayer::Arg::Type::Int: {
                    ImGui::DragInt("##ival", &arg.i);
                    break;
                }
                case SignalLayer::Arg::Type::Float: {
                    ImGui::DragFloat("##fval", &arg.f, 0.01f);
                    break;
                }
                case SignalLayer::Arg::Type::String: {
                    char sbuf[256];
                    std::snprintf(sbuf, sizeof(sbuf), "%s", arg.s.c_str());
                    if (ImGui::InputText("##sval", sbuf, sizeof(sbuf)))
                        arg.s = sbuf;
                    break;
                }
            }

            // Value-bound checkbox (continuous only; at most one per layer)
            if (isContinuous) {
                ImGui::SameLine();
                bool bound = arg.isValueBound;
                if (ImGui::Checkbox("Bound", &bound)) {
                    if (bound) {
                        // Clear all others first (enforce at-most-one invariant)
                        for (auto& other : sig->args) other.isValueBound = false;
                    }
                    arg.isValueBound = bound;
                }
            }

            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) {
                sig->args.erase(sig->args.begin() + ai);
                ImGui::PopID();
                break; // iterator invalidated -- restart next frame
            }

            ImGui::PopID();
        }

        if (ImGui::SmallButton("+ Add Arg"))
            sig->args.push_back(SignalLayer::Arg{});
    }

    ImGui::PopID();
}

// ============================================================================
// Audio section
// ============================================================================

void PropertyWindow::renderAudioSection(entt::entity clipEntity) {
    auto& registry = m_timeline->getRegistry();
    auto* src = registry.try_get<AudioSource>(clipEntity);
    if (!src) return;

    // Gain — show in dB; clamp linear to [0, 2] (~+6 dBFS headroom).
    // -60 dB floor avoids log(0). SliderFloat operates in dB space.
    constexpr float kMinDb = -60.0f;
    constexpr float kMaxDb =  6.0f;

    auto linearToDb = [&](float g) -> float {
        return g <= 0.0f ? kMinDb : std::max(kMinDb, 20.0f * std::log10(g));
    };
    auto dbToLinear = [&](float db) -> float {
        return std::pow(10.0f, db / 20.0f);
    };

    float gainDb = linearToDb(src->gain);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 60.0f);
    bool gainChanged = ImGui::SliderFloat("##audiogain", &gainDb, kMinDb, kMaxDb, "%.1f dB");
    if (ImGui::IsItemActivated()) {
        m_preEditAudioGain = src->gain;
    }
    if (gainChanged) {
        src->gain = dbToLinear(gainDb);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && m_dispatcher) {
        auto cmd = std::make_unique<SetAudioSourceGainCommand>(clipEntity, src->gain);
        cmd->setPreviousGain(m_preEditAudioGain);
        m_dispatcher->enqueue(std::move(cmd));
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("Gain");

    // Mute / Solo toggles on one row
    bool mute = src->mute;
    if (ImGui::Checkbox("Mute##audiomute", &mute) && m_dispatcher) {
        auto cmd = std::make_unique<SetAudioSourceMuteCommand>(clipEntity, mute);
        cmd->setPreviousMute(src->mute);
        src->mute = mute;
        m_dispatcher->enqueue(std::move(cmd));
    }
    ImGui::SameLine();
    bool solo = src->solo;
    if (ImGui::Checkbox("Solo##audiosolo", &solo) && m_dispatcher) {
        auto cmd = std::make_unique<SetAudioSourceSoloCommand>(clipEntity, solo);
        cmd->setPreviousSolo(src->solo);
        src->solo = solo;
        m_dispatcher->enqueue(std::move(cmd));
    }
}

// ============================================================================
// Remote Control section (ADR-0028)
// ============================================================================

void PropertyWindow::renderRemoteControlSection(entt::entity layerEntity) {
    if (!m_timeline || layerEntity == entt::null) return;
    auto& registry = m_timeline->getRegistry();
    if (!registry.valid(layerEntity)) return;

    auto* rp  = registry.try_get<RemotePatch>(layerEntity);
    const auto idx = findClipIndices(m_timeline, layerEntity);

    if (!rp) {
        const bool canPatch = idx.has_value() && m_dispatcher;
        if (ImGui::Button("Patch to Remote Control", ImVec2(-1, 0)) && canPatch) {
            m_dispatcher->enqueue(std::make_unique<PatchLayerRemoteCommand>(
                idx->first, idx->second));
        }
        return;
    }

    // Address preview
    ImGui::Text("Address: /entity/layer/%s/...", rp->patchId.c_str());

    // ID InputText — RenameRemotePatch fires on deactivate-after-edit.
    char idBuf[64];
    std::snprintf(idBuf, sizeof(idBuf), "%s", rp->patchId.c_str());
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##patch_id", idBuf, sizeof(idBuf));
    if (ImGui::IsItemDeactivatedAfterEdit() && m_dispatcher && idx) {
        std::string newId(idBuf);
        if (newId != rp->patchId) {
            m_dispatcher->enqueue(std::make_unique<RenameRemotePatchCommand>(
                idx->first, idx->second, std::move(newId)));
        }
    }

    // Armed-by-default (undoable config, persisted in project).
    bool armed = rp->armedByDefault;
    if (ImGui::Checkbox("Armed by default", &armed) && m_dispatcher && idx) {
        m_dispatcher->enqueue(std::make_unique<SetRemotePatchArmedCommand>(
            idx->first, idx->second, armed));
    }

    // Live plane: engage toggle writes the store directly — it is show-
    // control signal, not a document edit. Not undoable, not persisted.
    if (m_remoteStore && rp->storeSlot >= 0) {
        bool engaged = m_remoteStore->engaged(rp->storeSlot);
        if (ImGui::Checkbox("Remote engaged (live)", &engaged)) {
            m_remoteStore->setEngaged(rp->storeSlot, engaged);
        }

        const auto s = m_remoteStore->sample(rp->storeSlot);
        if (s.engaged) {
            // Build a compact live-value readout. %.2f per present param;
            // "-" when param has never been sent. ASCII-only (ImGui default font).
            // Each value needs its own storage: the six fmtVal calls are
            // unsequenced arguments to one snprintf, so a shared static
            // buffer would make every %s point at the last value written.
            auto fmtVal = [&](remote::RemoteParam p, char* out, std::size_t n) {
                if (s.has(p)) std::snprintf(out, n, "%.2f", s.get(p));
                else          std::snprintf(out, n, "-");
            };
            char op[16], px[16], py[16], sx[16], sy[16], rot[16];
            fmtVal(remote::RemoteParam::Opacity,  op,  sizeof(op));
            fmtVal(remote::RemoteParam::PosX,     px,  sizeof(px));
            fmtVal(remote::RemoteParam::PosY,     py,  sizeof(py));
            fmtVal(remote::RemoteParam::ScaleX,   sx,  sizeof(sx));
            fmtVal(remote::RemoteParam::ScaleY,   sy,  sizeof(sy));
            fmtVal(remote::RemoteParam::Rotation, rot, sizeof(rot));
            char liveBuf[160];
            std::snprintf(liveBuf, sizeof(liveBuf),
                "live: op %s  x %s  y %s  sx %s  sy %s  rot %s",
                op, px, py, sx, sy, rot);
            ImGui::TextDisabled("%s", liveBuf);
        }
    } else if (rp->storeSlot < 0) {
        ImGui::TextDisabled("(slot unbound -- reload project)");
    }

    if (ImGui::Button("Unpatch", ImVec2(-1, 0)) && m_dispatcher && idx) {
        m_dispatcher->enqueue(std::make_unique<UnpatchLayerRemoteCommand>(
            idx->first, idx->second));
    }
}

} // namespace entity
