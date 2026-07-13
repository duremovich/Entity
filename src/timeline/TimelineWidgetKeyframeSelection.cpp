// Keyframe multi-selection for the timeline widget.
//
// Selection was single-only: one (clip, property, frame) triple. This file holds
// the set-based replacement — membership, the screen-space glyph cache the
// hit-test and box-select both read, the command-layer address mapping, and the
// delta clamp that keeps a group drag from stepping on keyframes it didn't select.

#include "entity/timeline/TimelineWidget.hpp"

#include "entity/command/CommandDispatcher.hpp"
#include "entity/command/Commands.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/EffectAnimatedParameters.hpp"
#include "entity/components/Layer.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/timeline/Timeline.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

namespace entity {

bool TimelineWidget::isKeyframeSelected(const KeyframeRef& ref) const {
    return std::find(m_selectedKeyframes.begin(), m_selectedKeyframes.end(), ref)
        != m_selectedKeyframes.end();
}

void TimelineWidget::selectOnlyKeyframe(const KeyframeRef& ref) {
    m_selectedKeyframes.clear();
    m_selectedKeyframes.push_back(ref);
}

void TimelineWidget::toggleKeyframeSelection(const KeyframeRef& ref) {
    auto it = std::find(m_selectedKeyframes.begin(), m_selectedKeyframes.end(), ref);
    if (it != m_selectedKeyframes.end()) {
        m_selectedKeyframes.erase(it);
    } else {
        m_selectedKeyframes.push_back(ref);
    }
}

void TimelineWidget::clearKeyframeSelection() {
    m_selectedKeyframes.clear();
}

std::vector<TimelineWidget::KeyframeRef>
TimelineWidget::keyframesInRect(const ImVec2& a, const ImVec2& b) const {
    const float minX = std::min(a.x, b.x);
    const float maxX = std::max(a.x, b.x);
    const float minY = std::min(a.y, b.y);
    const float maxY = std::max(a.y, b.y);

    std::vector<KeyframeRef> hits;
    for (const auto& g : m_keyframeGlyphs) {
        // Glyph counts as covered when its drawn extent overlaps the rect, so
        // clipping a diamond's edge selects it — matches how the clip box-select
        // treats partial overlap.
        if (g.center.x + g.radius < minX || g.center.x - g.radius > maxX) continue;
        if (g.center.y + g.radius < minY || g.center.y - g.radius > maxY) continue;
        hits.push_back(g.ref);
    }
    return hits;
}

std::optional<TimelineWidget::KeyframeRef>
TimelineWidget::findKeyframeGlyphAt(ImVec2 mousePos) const {
    // A few px of slack so the diamond's points aren't the only clickable part.
    constexpr float kHitPad = 3.0f;

    // Front-to-back would be arbitrary here (rows don't overlap), so nearest-center
    // wins — it does the right thing when two glyphs sit close together at low zoom.
    const KeyframeGlyph* best = nullptr;
    float bestDistSq = std::numeric_limits<float>::max();

    for (const auto& g : m_keyframeGlyphs) {
        const float reach = g.radius + kHitPad;
        const float dx = mousePos.x - g.center.x;
        const float dy = mousePos.y - g.center.y;
        if (std::fabs(dx) > reach || std::fabs(dy) > reach) continue;

        const float distSq = dx * dx + dy * dy;
        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            best = &g;
        }
    }
    if (!best) return std::nullopt;
    return best->ref;
}

std::optional<KeyframeAddr>
TimelineWidget::keyframeAddrFor(const KeyframeRef& ref) const {
    KeyframeAddr addr;
    addr.frame = ref.frame;

    if (ref.isEffect) {
        addr.isEffect     = true;
        addr.effectEntity = ref.effectEntity;
        // The command layer addresses effect params by NAME (wire-stable);
        // the widget carries the hash. Recover the name from the row list.
        for (const auto& def : propertyListForEntity(ref.clip)) {
            if (def.source == TimelinePropertyDef::Source::EffectParam &&
                def.effectEntity == ref.effectEntity &&
                def.paramHash == ref.paramHash)
            {
                addr.paramName = def.paramName;
                break;
            }
        }
        if (addr.paramName.empty()) return std::nullopt;  // row is gone
        return addr;
    }

    // Transform rows are addressed by (trackIndex, clipIndex).
    if (!m_timeline) return std::nullopt;
    auto& registry = m_timeline->getRegistry();
    const auto& tracks = m_timeline->getTracks();
    for (std::size_t ti = 0; ti < tracks.size(); ++ti) {
        const auto* track = registry.try_get<TimelineTrack>(tracks[ti]);
        if (!track) continue;
        for (std::size_t ci = 0; ci < track->layers.size(); ++ci) {
            if (track->layers[ci] != ref.clip) continue;
            addr.trackIndex = static_cast<int>(ti);
            addr.clipIndex  = static_cast<int>(ci);
            addr.property   = ref.prop;
            return addr;
        }
    }
    return std::nullopt;  // layer no longer in any track
}

std::vector<KeyframeAddr> TimelineWidget::selectedKeyframeAddrs() const {
    std::vector<KeyframeAddr> addrs;
    addrs.reserve(m_selectedKeyframes.size());
    for (const auto& ref : m_selectedKeyframes) {
        if (auto addr = keyframeAddrFor(ref)) addrs.push_back(*addr);
    }
    return addrs;
}

TimelineWidget::KeyframeSpacing
TimelineWidget::keyframeSpacingFor(const KeyframeRef& ref, FrameNumber atFrame) const {
    KeyframeSpacing spacing;
    if (!m_timeline) return spacing;
    auto& registry = m_timeline->getRegistry();

    const std::vector<Keyframe>* kfs = nullptr;
    if (ref.isEffect) {
        if (!registry.valid(ref.effectEntity)) return spacing;
        if (const auto* eap = registry.try_get<EffectAnimatedParameters>(ref.effectEntity)) {
            for (const auto& nt : eap->tracks) {
                if (nt.paramKeyHash == ref.paramHash) { kfs = &nt.keyframes; break; }
            }
        }
    } else {
        if (!registry.valid(ref.clip)) return spacing;
        if (const auto* animProps = registry.try_get<AnimatedProperties>(ref.clip)) {
            if (const KeyframeTrack* t = animProps->getTrack(ref.prop)) kfs = &t->keyframes;
        }
    }
    if (!kfs) return spacing;

    FrameNumber bestPrev = std::numeric_limits<FrameNumber>::min();
    FrameNumber bestNext = std::numeric_limits<FrameNumber>::max();

    for (const auto& kf : *kfs) {
        KeyframeRef neighbour = ref;
        neighbour.frame = kf.frame;
        if (neighbour == ref) continue;
        if (isKeyframeSelected(neighbour)) continue;  // moves with us; gap is fixed

        if (kf.frame < atFrame)      bestPrev = std::max(bestPrev, kf.frame);
        else if (kf.frame > atFrame) bestNext = std::min(bestNext, kf.frame);
    }

    if (bestPrev != std::numeric_limits<FrameNumber>::min()) {
        spacing.hasPrev = true;
        spacing.prevGap = atFrame - bestPrev;
    }
    if (bestNext != std::numeric_limits<FrameNumber>::max()) {
        spacing.hasNext = true;
        spacing.nextGap = bestNext - atFrame;
    }
    return spacing;
}

void TimelineWidget::deleteSelectedKeyframes() {
    if (m_selectedKeyframes.empty() || !m_commandDispatcher) return;

    auto addrs = selectedKeyframeAddrs();
    if (addrs.empty()) return;

    m_commandDispatcher->enqueue(std::make_unique<RemoveKeyframesCommand>(addrs));
    clearKeyframeSelection();
}

void TimelineWidget::computeDragDeltaBounds() {
    m_dragDeltaMin = std::numeric_limits<FrameNumber>::min();
    m_dragDeltaMax = std::numeric_limits<FrameNumber>::max();

    if (!m_timeline || m_selectedKeyframes.empty()) {
        m_dragDeltaMin = 0;
        m_dragDeltaMax = 0;
        return;
    }
    auto& registry = m_timeline->getRegistry();

    // Every selected keyframe must stay strictly between its nearest neighbours
    // that are NOT themselves selected (a selected neighbour moves by the same
    // delta, so it can never be collided with). The tightest of those per-keyframe
    // windows is the delta window for the whole drag.
    for (const auto& ref : m_selectedKeyframes) {
        const std::vector<Keyframe>* kfs = nullptr;

        if (ref.isEffect) {
            if (!registry.valid(ref.effectEntity)) continue;
            const auto* eap = registry.try_get<EffectAnimatedParameters>(ref.effectEntity);
            if (!eap) continue;
            for (const auto& nt : eap->tracks) {
                if (nt.paramKeyHash == ref.paramHash) { kfs = &nt.keyframes; break; }
            }
        } else {
            if (!registry.valid(ref.clip)) continue;
            const auto* animProps = registry.try_get<AnimatedProperties>(ref.clip);
            if (!animProps) continue;
            if (const KeyframeTrack* t = animProps->getTrack(ref.prop)) kfs = &t->keyframes;
        }
        if (!kfs) continue;

        FrameNumber prevUnselected = -1;  // frame 0 is the floor, so -1 = "none"
        FrameNumber nextUnselected = std::numeric_limits<FrameNumber>::max();
        for (const auto& kf : *kfs) {
            if (kf.frame == ref.frame) continue;

            KeyframeRef neighbour = ref;
            neighbour.frame = kf.frame;
            if (isKeyframeSelected(neighbour)) continue;  // moves with us

            if (kf.frame < ref.frame) {
                prevUnselected = std::max(prevUnselected, kf.frame);
            } else {
                nextUnselected = std::min(nextUnselected, kf.frame);
            }
        }

        // Keyframes may not cross a neighbour or land on it, and may not go
        // negative. Bounds are inclusive, hence the -1 / +1.
        const FrameNumber lowFloor = prevUnselected + 1;             // >= 0 always
        m_dragDeltaMin = std::max(m_dragDeltaMin, lowFloor - ref.frame);
        if (nextUnselected != std::numeric_limits<FrameNumber>::max()) {
            m_dragDeltaMax = std::min(m_dragDeltaMax, (nextUnselected - 1) - ref.frame);
        }
    }

    // Nothing resolved (all selections stale) — pin the drag rather than let it
    // run with the sentinel bounds.
    if (m_dragDeltaMin > m_dragDeltaMax) {
        m_dragDeltaMin = 0;
        m_dragDeltaMax = 0;
    }
}

namespace {

// "1.40s (42f)" — the two units a user actually thinks in when spacing keyframes.
std::string formatGap(FrameNumber frames, double fps) {
    const double seconds = (fps > 0.0) ? static_cast<double>(frames) / fps : 0.0;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2fs (%lldf)", seconds,
                  static_cast<long long>(frames));
    return buf;
}

}  // namespace

void TimelineWidget::renderKeyframeDragReadout() {
    if (!m_timeline) return;
    if (m_selectedKeyframes.empty()) return;

    // Anchor's live (previewed) frame, and where it's drawn.
    const FrameNumber liveFrame = m_dragKeyframeOriginalFrame + m_dragDelta;

    ImVec2 anchorPos{0, 0};
    bool found = false;
    for (const auto& g : m_keyframeGlyphs) {
        if (g.ref == m_dragAnchor) { anchorPos = g.center; found = true; break; }
    }
    if (!found) return;

    const double fps = m_timeline->getFrameRate();
    const KeyframeSpacing spacing = keyframeSpacingFor(m_dragAnchor, liveFrame);

    // The number the user is actually reaching for: how far this keyframe now sits
    // from the one before it. The gap to the next is the other half of the picture.
    std::string line;
    if (spacing.hasPrev) {
        line = "prev " + formatGap(spacing.prevGap, fps);
    } else {
        line = "start " + formatGap(liveFrame, fps);
    }
    if (spacing.hasNext) {
        line += "   next " + formatGap(spacing.nextGap, fps);
    }
    if (m_selectedKeyframes.size() > 1) {
        line += "   [" + std::to_string(m_selectedKeyframes.size()) + " selected]";
    }
    line += "   TAB to type";

    // Floating chip above the keyframe. Foreground draw list so it isn't clipped
    // by the property row it's sitting on.
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 textSize = ImGui::CalcTextSize(line.c_str());
    const ImVec2 pad(6.0f, 3.0f);
    const ImVec2 boxMin(anchorPos.x + 10.0f, anchorPos.y - textSize.y - 14.0f);
    const ImVec2 boxMax(boxMin.x + textSize.x + pad.x * 2.0f,
                        boxMin.y + textSize.y + pad.y * 2.0f);

    dl->AddRectFilled(boxMin, boxMax, IM_COL32(20, 22, 28, 235), 3.0f);
    dl->AddRect(boxMin, boxMax, IM_COL32(255, 200, 50, 200), 3.0f);
    dl->AddText(ImVec2(boxMin.x + pad.x, boxMin.y + pad.y),
                IM_COL32(235, 235, 235, 255), line.c_str());
}

void TimelineWidget::beginKeyframeOffsetEntry() {
    if (!m_timeline || m_selectedKeyframes.empty()) return;

    // Fresh entry (no drag handed one off): adopt the first selected keyframe as
    // the anchor and work out how far it may travel.
    if (m_dragAnchor.clip == entt::null || !isKeyframeSelected(m_dragAnchor)) {
        m_dragAnchor                = m_selectedKeyframes.front();
        m_dragKeyframeOriginalFrame = m_dragAnchor.frame;
        m_dragDelta                 = 0;
        computeDragDeltaBounds();
    }

    const double fps = m_timeline->getFrameRate();
    const FrameNumber liveFrame = m_dragKeyframeOriginalFrame + m_dragDelta;
    const KeyframeSpacing spacing = keyframeSpacingFor(m_dragAnchor, liveFrame);

    // Seed with the offset the keyframe currently has, so TAB-then-Enter is a
    // no-op and the field reads as "here's the number you're editing".
    const FrameNumber seedFrames = spacing.hasPrev ? spacing.prevGap : liveFrame;
    const double seedSeconds = (fps > 0.0) ? static_cast<double>(seedFrames) / fps : 0.0;
    std::snprintf(m_kfOffsetEntryBuf, sizeof(m_kfOffsetEntryBuf), "%.3f", seedSeconds);

    for (const auto& g : m_keyframeGlyphs) {
        if (g.ref == m_dragAnchor) { m_kfOffsetEntryPos = g.center; break; }
    }

    m_kfOffsetEntryActive  = true;
    m_kfOffsetFocusPending = true;
}

void TimelineWidget::renderKeyframeOffsetEntry() {
    if (!m_timeline) return;

    const double fps = m_timeline->getFrameRate();

    // Reference point the typed offset is measured FROM: the previous unselected
    // keyframe, or the clip start when there isn't one.
    const FrameNumber liveFrame = m_dragKeyframeOriginalFrame + m_dragDelta;
    const KeyframeSpacing spacing = keyframeSpacingFor(m_dragAnchor, liveFrame);
    const FrameNumber baseFrame = spacing.hasPrev ? (liveFrame - spacing.prevGap) : 0;

    ImGui::SetCursorScreenPos(ImVec2(m_kfOffsetEntryPos.x + 10.0f,
                                     m_kfOffsetEntryPos.y + 8.0f));
    ImGui::SetNextItemWidth(90.0f);
    if (m_kfOffsetFocusPending) {
        ImGui::SetKeyboardFocusHere();
        m_kfOffsetFocusPending = false;
    }

    const bool committed = ImGui::InputText(
        "##kfOffsetEntry", m_kfOffsetEntryBuf, sizeof(m_kfOffsetEntryBuf),
        ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue |
        ImGuiInputTextFlags_AutoSelectAll);

    ImGui::SameLine();
    ImGui::TextUnformatted("s from prev");

    // Live preview as they type: parse, convert to a delta, clamp it the same way
    // a mouse drag would be, and let the renderer draw the selection there.
    double typedSeconds = 0.0;
    const bool parsed = (std::sscanf(m_kfOffsetEntryBuf, "%lf", &typedSeconds) == 1);
    if (parsed && fps > 0.0) {
        const FrameNumber target =
            baseFrame + static_cast<FrameNumber>(std::llround(typedSeconds * fps));
        const FrameNumber wanted = target - m_dragKeyframeOriginalFrame;
        m_dragDelta = std::clamp(wanted, m_dragDeltaMin, m_dragDeltaMax);
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        m_dragDelta           = 0;   // snap the preview back
        m_kfOffsetEntryActive = false;
        m_dragAnchor          = KeyframeRef{};
        return;
    }

    if (committed) {
        if (m_dragDelta != 0 && m_commandDispatcher) {
            auto addrs = selectedKeyframeAddrs();
            if (!addrs.empty()) {
                m_commandDispatcher->enqueue(
                    std::make_unique<MoveKeyframesCommand>(addrs, m_dragDelta));
                for (auto& ref : m_selectedKeyframes) ref.frame += m_dragDelta;
            }
        }
        m_dragDelta           = 0;
        m_kfOffsetEntryActive = false;
        m_dragAnchor          = KeyframeRef{};
    }
}

} // namespace entity
