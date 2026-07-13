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
#include <limits>

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

} // namespace entity
