/**
 * TimelineWidget core: constructor, top-level render() dispatch, and the small
 * coordinate/hit-test helpers shared by both the render and input halves.
 *
 * Split layout (Phase B #16):
 *   - TimelineWidget.cpp         (this file) — ctor, render(), timeToPixel,
 *                                               pixelToTime, findTrackAtY,
 *                                               checkClipCollision
 *   - TimelineWidgetRender.cpp   — all render*() draw code
 *   - TimelineWidgetInput.cpp    — all handle*() / findClip*() interaction code
 */

#include "entity/timeline/TimelineWidget.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/Layer.hpp"
#include "entity/components/ObjectAnimationLayer.hpp"
#include "entity/components/GenerativeLayer.hpp"
#include "entity/components/SignalLayer.hpp"
#include "entity/components/TrackUiState.hpp"
#include "entity/core/Engine.hpp"
#include "entity/audio/AudioEngine.hpp"
#include "entity/command/CommandDispatcher.hpp"
#include "entity/command/Commands.hpp"
#include <sstream>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <vector>
#include <algorithm>

namespace entity {

TimelineWidget::TimelineWidget(Timeline* timeline)
    : m_timeline(timeline)
{
}

int TimelineWidget::framesPerTick() const {
    const ZoomStop& stop = ZOOM_STOPS[std::clamp(m_zoomIndex, 0, ZOOM_LEVEL_COUNT - 1)];
    if (stop.unit == ZoomStop::Unit::Frames) return stop.count;

    // Seconds stop: resolve against the project frame rate. Rounded, so 1s at
    // 29.97fps is a 30-frame division — the grid stays on whole frames and the
    // labels stay honest, which is what counting seconds by eye needs.
    const double fps = m_timeline ? m_timeline->getFrameRate() : 30.0;
    const int frames = static_cast<int>(std::lround(fps * stop.count));
    return std::max(1, frames);  // a 0 stride would hang every tick loop
}

void TimelineWidget::applyZoomIndex() {
    if (!m_timeline) return;
    // Tick spacing is fixed at TICK_PX. Zoom changes how much TIME a tick
    // represents (framesPerTick), which scales pxPerSec accordingly.
    const float pxPerFrame = TICK_PX / static_cast<float>(framesPerTick());
    m_pixelsPerSecond = pxPerFrame * static_cast<float>(m_timeline->getFrameRate());
}

void TimelineWidget::setZoomIndex(int idx) {
    const int newIdx = std::clamp(idx, 0, ZOOM_LEVEL_COUNT - 1);
    if (newIdx == m_zoomIndex) return;

    // Re-anchor scroll so a reference time stays at the same screen position
    // across the zoom change — otherwise scroll stays at the old absolute pixel
    // value and the viewport jumps to a completely different time when zoom
    // (and therefore pxPerSec) changes.
    const bool canReanchor = (m_lastVisibleWidth > 0.0f && m_timeline != nullptr);
    float anchorScreenOffset = 0.0f;
    Timecode anchorTime = 0;
    if (canReanchor) {
        const float playheadPx = timeToPixel(m_timeline->getCurrentTime());
        const float viewLeft  = m_syncScrollX;
        const float viewRight = m_syncScrollX + m_lastVisibleWidth;
        const bool playheadVisible = (playheadPx >= viewLeft && playheadPx <= viewRight);
        if (playheadVisible) {
            anchorTime = m_timeline->getCurrentTime();
            anchorScreenOffset = playheadPx - viewLeft;
        } else {
            anchorScreenOffset = m_lastVisibleWidth * 0.5f;
            anchorTime = pixelToTime(viewLeft + anchorScreenOffset);
        }
    }

    m_zoomIndex = newIdx;
    applyZoomIndex();

    if (canReanchor) {
        const float anchorPxNew = timeToPixel(anchorTime);
        const float durationPx = timeToPixel(m_timeline->getDuration());
        const float maxScrollX = std::max(0.0f, durationPx - m_lastVisibleWidth);
        float newScroll = anchorPxNew - anchorScreenOffset;
        newScroll = std::clamp(newScroll, 0.0f, maxScrollX);
        m_syncScrollX = newScroll;
        m_pendingScrollX = true;
    }
}

void TimelineWidget::render() {
    if (!m_timeline) {
        ImGui::Text("No timeline set");
        return;
    }

    // Ghost preview is a derived per-frame visual. Reset it at the top of
    // every frame except while a clip drag is in progress — in that case
    // handleTracksInteraction owns m_ghost (set at end of frame, read at
    // the next frame's renderTracks). For ImGui drag-drop sessions the
    // peek inside renderTrack re-enables the ghost this frame if a
    // payload is active.
    if (!m_isDraggingClip) {
        m_ghost.active = false;
    }

    // Clean up stale entity references in expansion state
    {
        auto& reg = m_timeline->getRegistry();
        for (auto it = m_expandedTracks.begin(); it != m_expandedTracks.end(); ) {
            entt::entity entity = static_cast<entt::entity>(*it);
            if (!reg.valid(entity)) {
                it = m_expandedTracks.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Note: Window Begin/End is now handled by TimelineWindow wrapper
    // This render() method only renders the timeline content

    ImVec2 contentRegion = ImGui::GetContentRegionAvail();

    // Calculate timeline dimensions. Tick spacing is fixed at TICK_PX; zoom
    // changes the time each tick represents, so content width grows/shrinks
    // with zoom (and horizontal scroll is used to pan within long timelines).
    float durationSeconds = m_timeline->getDuration() / 1000000.0f;
    m_lastVisibleWidth = contentRegion.x - TRACK_HEADER_WIDTH - 4.0f;
    applyZoomIndex();
    float timelineWidth = durationSeconds * m_pixelsPerSecond;

    // Follow the playhead during playback. Must run HERE, before the tracks child
    // is submitted below: ensurePlayheadVisible() writes m_pendingScrollX, which is
    // consumed by the SetNextWindowScroll ahead of that child's Begin(). Called any
    // later and the scroll wouldn't land until the following frame.
    const bool isPlaying = (m_timeline->getPlaybackState() == PlaybackState::Playing);
    if (isPlaying && !m_wasPlaying) {
        m_followSuspended = false;  // a fresh play re-arms following
    }
    m_wasPlaying = isPlaying;

    if (isPlaying && m_followPlayhead && !m_followSuspended) {
        ensurePlayheadVisible();
    }

    // Build the per-frame track-row layout cache (Phase 9). This is the single
    // source of truth for which tracks are visible (shy tracks skipped when the
    // global hide toggle is on) and their cumulative Y / height. Every renderer
    // and hit-test below iterates it.
    buildTrackRows();

    // Total tracks content height = the visible rows' heights + padding. Derives
    // from the same cache so the InvisibleButton extent matches exactly what's
    // drawn (and shrinks when shy tracks are hidden).
    float tracksContentHeight = 0.0f;
    for (const auto& row : m_trackRows) {
        tracksContentHeight += row.height + TRACK_PADDING;
    }

    // Compute cue-lane height *before* allocating the ruler child so the
    // band can be reserved above the time ruler. Lane sits between any
    // toolbar/menu rows above us and the time ruler below; child window
    // height grows by m_cueLaneHeight to host both.
    {
        std::vector<CueLaneSlot> slots;
        int rowsUsed = 0;
        computeCueLaneLayout(slots, rowsUsed);
        const float minLaneH = kCueRowH;  // one row tall, ~16px so right-click hit-test always engages
        m_cueLaneHeight = std::max(minLaneH,
            kCueRowH * static_cast<float>(rowsUsed) + kCueLanePad);
    }
    const float topBandHeight = RULER_HEIGHT + m_cueLaneHeight;

    // Reserve space for controls at bottom (approximately 40 pixels)
    float controlsHeight = 40.0f;
    float availableHeight = contentRegion.y - controlsHeight;
    float tracksWindowHeight = availableHeight - topBandHeight;
    float timelineContentWidth = m_lastVisibleWidth;

    // The InvisibleButton content extent inside both the tracks child and the
    // header inner child. Both children MUST declare the same vertical content
    // extent so their ScrollMax clamps match — otherwise a SetScrollY on the
    // slave near the bottom is clamped to a different ceiling than the master
    // and the two panels stay permanently offset (research finding #4).
    const float tracksContentExtentY = tracksContentHeight + 100.0f;

    // === SCROLL-SYNC OWNERSHIP (Phase 3 rework) ===
    // The tracks child owns its scroll natively: it carries the real scrollbar,
    // takes the mouse wheel, and we do NOT force-set its scroll every frame.
    // The ruler (X) and the header panel (Y) are SLAVES — their scroll is
    // SetScroll'd to the tracks child's CURRENT-frame value.
    //
    // Two ImGui facts drive the submission order (verified against vendored
    // imgui 1.89.7):
    //   1. SetScrollY writes window->ScrollTarget, consumed at the START of
    //      that window's Begin() and then cleared. So a slave's SetScroll must
    //      be issued BEFORE that slave's BeginChild() to take effect the same
    //      frame.
    //   2. GetScrollX/Y reads window->Scroll, which only reflects an applied
    //      target after the window's Begin() has run.
    // Therefore the master (TimelineTracks) is submitted FIRST so we can read
    // its committed scroll, then the slaves are submitted with that same-frame
    // value set pre-Begin. Children are positioned explicitly via SetCursorPos
    // (instead of cursor-flow + SameLine) so the submission reorder doesn't
    // disturb the on-screen layout.
    const ImVec2 layoutOrigin = ImGui::GetCursorPos();
    const float spacingY = ImGui::GetStyle().ItemSpacing.y;
    const float rightColumnX = layoutOrigin.x + TRACK_HEADER_WIDTH + 2.0f;
    const float bottomRowY = layoutOrigin.y + topBandHeight + spacingY;

    // --- MASTER: scrollable tracks (right side, bottom row) ---
    // Submitted first each frame so its committed scroll can be read and fed to
    // the slaves the SAME frame. Owns its scroll natively (HorizontalScrollbar,
    // wheel) — no per-frame SetScrollX/Y force-set; programmatic moves go
    // through the pending-flag path below.
    ImGui::SetCursorPos(ImVec2(rightColumnX, bottomRowY));
    // Programmatic scroll moves only (ensurePlayheadVisible / zoom re-anchor /
    // wheel-over-header) push the target through SetNextWindowScroll, applied
    // at this child's Begin THIS frame (research finding #1). -1 leaves an axis
    // untouched so a pure-X move doesn't reset Y and vice versa. The pending
    // flags also suppress the same-frame read-back below so the just-applied
    // target isn't stomped.
    if (m_pendingScrollX || m_pendingScrollY) {
        ImGui::SetNextWindowScroll(ImVec2(m_pendingScrollX ? m_syncScrollX : -1.0f,
                                          m_pendingScrollY ? m_syncScrollY : -1.0f));
    }
    ImGui::BeginChild("TimelineTracks", ImVec2(timelineContentWidth, tracksWindowHeight), false,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNav);

    // Save tracks position for playhead rendering
    m_tracksScreenPos = ImGui::GetCursorScreenPos();

    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::InvisibleButton("##tracksArea", ImVec2(timelineWidth, tracksContentExtentY));
    ImGui::SetCursorPos(ImVec2(0, 0));

    renderTracks();

    // Read the tracks child's committed scroll SAME-frame, BEFORE the slaves
    // are submitted and before hit-test code (handleTracksInteraction) runs.
    // m_syncScrollX/Y are the canonical last-known scroll the input code reads
    // (TimelineWidgetInput.cpp ~1204, ~1458). On a frame where we issued a
    // programmatic SetScroll above, GetScroll still returns the pre-target
    // value, so keep our target for that one frame (matches the documented
    // SetScroll-vs-GetScroll lag).
    if (!m_pendingScrollX) {
        const float committedScrollX = ImGui::GetScrollX();
        // A horizontal scroll we didn't ask for, mid-playback, is the user reaching
        // for a different part of the timeline — stop following so the view doesn't
        // yank straight back to the playhead. On frames where follow moved the
        // scroll itself, m_pendingScrollX is set and this read-back is skipped
        // entirely, so our own moves can't trip this.
        if (isPlaying && m_followPlayhead && !m_followSuspended &&
            std::fabs(committedScrollX - m_syncScrollX) > 2.0f)
        {
            m_followSuspended = true;
        }
        m_syncScrollX = committedScrollX;
    }
    if (!m_pendingScrollY) {
        m_syncScrollY = ImGui::GetScrollY();
    }
    m_pendingScrollX = false;
    m_pendingScrollY = false;
    m_tracksHeight = tracksContentHeight;

    handleTracksInteraction();

    ImGui::EndChild();

    // === TOP ROW: Header corner + Ruler ===
    // Header corner (empty space above track headers, aligned with ruler).
    // Spans the cue-lane band too so vertical alignment stays consistent.
    ImGui::SetCursorPos(layoutOrigin);
    ImGui::BeginChild("HeaderCorner", ImVec2(TRACK_HEADER_WIDTH, topBandHeight), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* cornerDrawList = ImGui::GetWindowDrawList();
    ImVec2 cornerMin = ImGui::GetWindowPos();
    ImVec2 cornerMax(cornerMin.x + TRACK_HEADER_WIDTH, cornerMin.y + topBandHeight);
    cornerDrawList->AddRectFilled(cornerMin, cornerMax, IM_COL32(35, 35, 40, 255));
    cornerDrawList->AddText(ImVec2(cornerMin.x + 8, cornerMin.y + 8), IM_COL32(150, 150, 150, 255), "Tracks");

    // Global "hide shy tracks" toggle. Right-aligned in the corner band so it
    // doesn't collide with the "Tracks" label. ASCII-only label (the editor's
    // default font has no glyph for an eye icon). Direct view-state write — not
    // undoable (twirldown precedent). When ON, shy tracks drop out of the row
    // cache on the next buildTrackRows().
    {
        const bool hideShy = m_timeline->getHideShyTracks();
        const char* shyLabel = hideShy ? "Shy*" : "Shy";
        const ImVec2 sz = ImGui::CalcTextSize(shyLabel);
        const float btnW = sz.x + 10.0f;
        ImGui::SetCursorScreenPos(ImVec2(cornerMax.x - btnW - 6.0f, cornerMin.y + 5.0f));
        if (ImGui::SmallButton(shyLabel)) {
            m_timeline->setHideShyTracks(!hideShy);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(hideShy ? "Showing all tracks. Click to hide shy tracks."
                                      : "Click to hide tracks flagged shy.");
        }
    }
    ImGui::EndChild();

    // === STICKY RULER (Fixed at top, right side) — SLAVE (X) ===
    // NoNav: arrow keys are owned by Engine::handleKey(). NoScrollWithMouse so
    // the wheel scrolls the tracks child, not the ruler independently.
    // NoScrollbar hides the user-facing scrollbar widget — the SetScrollX below
    // mirrors the tracks child's committed scroll (same-frame). Without
    // NoScrollbar, dragging the ruler's scrollbar desyncs from the tracks
    // scroll and the playhead renders as a diagonal line (top end uses ruler
    // scroll, bottom end uses tracks scroll).
    //
    // SetScrollX MUST precede this BeginChild (research finding #1) — calling it
    // after Begin lags the ruler one frame behind the tracks.
    //
    // Child height = cue lane band (top) + time ruler (bottom). The cue
    // lane is drawn by renderCueLane() before the ruler so triangle pointers
    // can drop down into the ruler band underneath.
    ImGui::SetCursorPos(ImVec2(rightColumnX, layoutOrigin.y));
    ImGui::SetNextWindowScroll(ImVec2(m_syncScrollX, -1.0f));  // -1 = leave Y untouched
    ImGui::BeginChild("TimelineRuler", ImVec2(timelineContentWidth, topBandHeight), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);

    ImVec2 childOrigin = ImGui::GetCursorScreenPos();

    // Cue lane band (above the ruler). Skipped when no cues -> 0 height.
    if (m_cueLaneHeight > 0.0f) {
        renderCueLane(childOrigin, m_cueLaneHeight, childOrigin.y + m_cueLaneHeight);
    }

    // Push cursor down past the cue lane so renderTimeRuler() lands at the
    // correct screen Y. m_rulerScreenPos must point at the ruler top, NOT
    // the cue-lane top — playhead/section-break rendering reads it.
    ImGui::SetCursorPos(ImVec2(0, m_cueLaneHeight));
    m_rulerScreenPos = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton("##rulerArea", ImVec2(timelineWidth, RULER_HEIGHT));
    ImGui::SetCursorPos(ImVec2(0, m_cueLaneHeight));

    renderTimeRuler();
    handleRulerInteraction();

    ImGui::EndChild();

    // === BOTTOM ROW (left): Track headers — SLAVE (Y) ===
    // Track header panel (left side, syncs vertical scroll only).
    ImGui::SetCursorPos(ImVec2(layoutOrigin.x, bottomRowY));
    ImGui::BeginChild("TrackHeaders", ImVec2(TRACK_HEADER_WIDTH, tracksWindowHeight), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);
    // Capture the header panel's true screen rect so the wheel-over-header
    // forward (below) hit-tests against screen space directly, independent of
    // any outer-window scroll.
    const ImVec2 headerScreenMin = ImGui::GetWindowPos();

    // Inner scrollable region for header content. SetNextWindowScroll(Y) MUST
    // precede this BeginChild so the header is locked to the tracks child's
    // CURRENT-frame scroll, not last frame's (research finding #1 — that lag
    // was the header/lanes ghosting). The inner child's content extent matches
    // the tracks child's (tracksContentExtentY) so their ScrollMax clamps agree
    // (research finding #4).
    ImGui::SetNextWindowScroll(ImVec2(-1.0f, m_syncScrollY));  // -1 = leave X untouched
    ImGui::BeginChild("TrackHeadersInner", ImVec2(TRACK_HEADER_WIDTH - 2, tracksContentExtentY), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);

    renderTrackHeaderPanel(tracksContentHeight);

    ImGui::EndChild();
    ImGui::EndChild();

    // Restore the layout cursor to just below the bottom row so the Separator +
    // playback controls flow naturally after the explicitly-positioned children.
    ImGui::SetCursorPos(ImVec2(layoutOrigin.x, bottomRowY + tracksWindowHeight + spacingY));

    // Forward a mouse wheel that landed over the header panel (which has
    // NoScrollWithMouse) into a pending Y move on the tracks child. Without
    // this the wheel is dead while the cursor is over the headers.
    {
        ImGuiIO& io = ImGui::GetIO();
        if (io.MouseWheel != 0.0f) {
            const ImVec2 m = io.MousePos;
            const bool overHeaders =
                m.x >= headerScreenMin.x && m.x < headerScreenMin.x + TRACK_HEADER_WIDTH &&
                m.y >= headerScreenMin.y && m.y < headerScreenMin.y + tracksWindowHeight;
            if (overHeaders) {
                // Match ImGui's wheel step (5 lines × font line height) and
                // clamp to the tracks child's scroll range.
                const float step = io.MouseWheel * ImGui::GetTextLineHeightWithSpacing() * 5.0f;
                const float maxScrollY = std::max(0.0f, tracksContentExtentY - tracksWindowHeight);
                const float target = std::clamp(m_syncScrollY - step, 0.0f, maxScrollY);
                if (target != m_syncScrollY) {
                    m_syncScrollY = target;
                    m_pendingScrollY = true;
                }
            }
        }
    }

    // Render playhead (spans across both ruler and tracks)
    renderPlayhead();

    // Controls below timeline
    ImGui::Separator();

    // Playback controls
    if (ImGui::Button("Play")) {
        m_timeline->play();
    }
    ImGui::SameLine();
    if (ImGui::Button("Pause")) {
        m_timeline->pause();
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop")) {
        m_timeline->stop();
    }
    ImGui::SameLine();

    // Display current time in SMPTE timecode format (MM:SS:FF)
    FrameNumber currentFrame = m_timeline->getCurrentFrame();
    double frameRate = m_timeline->getFrameRate();
    int totalSeconds = static_cast<int>(currentFrame / frameRate);
    int minutes = totalSeconds / 60;
    int seconds = totalSeconds % 60;
    int frames = static_cast<int>(currentFrame % static_cast<FrameNumber>(frameRate));
    ImGui::Text("Time: %02d:%02d:%02d", minutes, seconds, frames);
    ImGui::SameLine();
    ImGui::Text("Frame %lld", static_cast<long long>(currentFrame));

    // Master audio controls
    if (m_engine) {
        auto* ae = m_engine->getAudioEngine();
        if (ae) {
            ImGui::SameLine();
            ImGui::Text("|");
            ImGui::SameLine();

            // Master mute toggle
            bool masterMute = ae->masterMute();
            if (ImGui::Checkbox("M##masterMute", &masterMute) && m_commandDispatcher) {
                auto cmd = std::make_unique<SetMasterMuteCommand>(masterMute);
                cmd->setPreviousMute(ae->masterMute());
                ae->setMasterMute(masterMute);
                m_commandDispatcher->enqueue(std::move(cmd));
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Master mute");

            ImGui::SameLine();

            // Master gain slider in dB
            constexpr float kMinDb = -60.0f;
            constexpr float kMaxDb =  6.0f;
            float masterGainLinear = ae->masterGain();
            float masterGainDb = masterGainLinear <= 0.0f
                ? kMinDb
                : std::max(kMinDb, 20.0f * std::log10(masterGainLinear));
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::SliderFloat("##masterGain", &masterGainDb, kMinDb, kMaxDb, "%.1fdB")) {
                ae->setMasterGain(std::pow(10.0f, masterGainDb / 20.0f));
            }
            if (ImGui::IsItemActivated()) {
                m_preEditMasterGain = masterGainLinear;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && m_commandDispatcher) {
                const float newLinear = ae->masterGain();
                auto cmd = std::make_unique<SetMasterGainCommand>(newLinear);
                cmd->setPreviousGain(m_preEditMasterGain);
                m_commandDispatcher->enqueue(std::move(cmd));
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Master gain");
        }
    }

    ImGui::SameLine();

    // Discrete zoom ladder: dropdown + minus/plus + Alt+scroll all step the
    // same m_zoomIndex. Fixed division sizes instead
    // of a continuous px/sec slider. Labels come straight off the ladder so they
    // can't drift out of sync with the stops.
    const char* kZoomLabels[ZOOM_LEVEL_COUNT];
    for (int i = 0; i < ZOOM_LEVEL_COUNT; ++i) kZoomLabels[i] = ZOOM_STOPS[i].label;
    ImGui::Text("Zoom:");
    ImGui::SameLine();
    if (ImGui::SmallButton("-##zoomOut")) setZoomIndex(m_zoomIndex + 1);  // larger frames/div = zoomed out
    ImGui::SameLine(0, 2);
    ImGui::SetNextItemWidth(70.0f);
    {
        // Route through setZoomIndex() so the re-anchor happens. Writing directly
        // to m_zoomIndex via Combo bypasses the scroll adjustment.
        int pickedIdx = m_zoomIndex;
        if (ImGui::Combo("##zoom", &pickedIdx, kZoomLabels, ZOOM_LEVEL_COUNT)) {
            setZoomIndex(pickedIdx);
        }
    }
    ImGui::SameLine(0, 2);
    if (ImGui::SmallButton("+##zoomIn")) setZoomIndex(m_zoomIndex - 1);   // smaller frames/div = zoomed in

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    // Follow-playhead toggle. Re-arms the suspend so a scroll during the previous
    // playback doesn't leave following silently dead after the user re-enables it.
    if (ImGui::Checkbox("Follow", &m_followPlayhead)) {
        m_followSuspended = false;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Scroll the timeline to keep the playhead in view during "
                          "playback.\nScrolling by hand pauses following until the "
                          "next play or seek.");
    }

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    // Add Track button
    if (ImGui::Button("+ Add Track")) {
        std::ostringstream trackName;
        trackName << "Video Track " << (m_timeline->getTrackCount() + 1);
        m_timeline->createTrack(trackName.str());
    }

    // Per-clip buffer-fill indicator removed in Phase C.10 — frames now
    // live in an engine-global FrameCache. A cache-wide bytesUsed/maxBytes
    // overlay can come back here once the widget gets an Engine* injected.

    // Handle context menus
    handleContextMenus();

    // Note: ImGui::End() is now handled by TimelineWindow wrapper
}

float TimelineWidget::timeToPixel(Timecode time) const {
    float seconds = time / 1000000.0f;  // Timecode is in microseconds
    return seconds * m_pixelsPerSecond;
}

Timecode TimelineWidget::pixelToTime(float pixel) const {
    float seconds = pixel / m_pixelsPerSecond;
    return static_cast<Timecode>(seconds * 1000000.0f);  // Timecode is in microseconds
}

float TimelineWidget::frameToPixel(FrameNumber frame) const {
    if (!m_timeline) return 0.0f;
    return timeToPixel(m_timeline->frameToTime(frame));
}

entt::entity TimelineWidget::findClipAtPlayhead(entt::entity trackEntity) const {
    if (!m_timeline) return entt::null;
    auto& registry = m_timeline->getRegistry();
    const auto* track = registry.try_get<TimelineTrack>(trackEntity);
    if (!track) return entt::null;

    FrameNumber currentFrame = m_timeline->getCurrentFrame();
    for (entt::entity clipEntity : track->layers) {
        FrameNumber startFrame = 0;
        FrameNumber duration   = 0;
        // Both Clip-backed and Layer-only (OA / Generative) entities are
        // valid track expansions. Clip is the source of truth for Clip-backed
        // entities; Layer is the source of truth for layer-only entities.
        if (const auto* clip = registry.try_get<Clip>(clipEntity)) {
            startFrame = clip->startFrame;
            duration   = clip->duration;
        } else if (const auto* lay = registry.try_get<Layer>(clipEntity);
                   lay && (registry.all_of<ObjectAnimationLayer>(clipEntity) ||
                           registry.all_of<GenerativeLayer>(clipEntity)       ||
                           registry.all_of<SignalLayer>(clipEntity))) {
            startFrame = lay->startFrame;
            duration   = lay->duration;
        } else {
            continue;
        }
        if (currentFrame >= startFrame && currentFrame < startFrame + duration) {
            return clipEntity;
        }
    }
    return entt::null;
}

void TimelineWidget::ensurePlayheadVisible() {
    if (!m_timeline) return;

    // Every explicit playhead jump (J/L, arrows, Home/End — see Engine.cpp) lands
    // here, and deliberately moving the playhead is a statement that you want to
    // look at it again. So re-arm following, which a manual scroll may have
    // suspended. Follow's own per-frame call clears an already-clear flag: a no-op.
    m_followSuspended = false;

    if (m_lastVisibleWidth <= 0.0f) return;
    applyZoomIndex();

    const float playheadPx = timeToPixel(m_timeline->getCurrentTime());
    const float durationPx = timeToPixel(m_timeline->getDuration());
    const float margin = 20.0f;
    const float viewLeft  = m_syncScrollX;
    const float viewRight = m_syncScrollX + m_lastVisibleWidth;
    const float maxScrollX = std::max(0.0f, durationPx - m_lastVisibleWidth);

    float newScrollX = m_syncScrollX;
    if (playheadPx < viewLeft + margin) {
        newScrollX = playheadPx - margin;
    } else if (playheadPx > viewRight - margin) {
        newScrollX = playheadPx - m_lastVisibleWidth + margin;
    }
    newScrollX = std::clamp(newScrollX, 0.0f, maxScrollX);

    if (newScrollX != m_syncScrollX) {
        m_syncScrollX = newScrollX;
        m_pendingScrollX = true;
    }
}

Timecode TimelineWidget::checkClipCollision(entt::entity clipEntity, Timecode newStartTime, int trackIndex) {
    if (!m_timeline) return newStartTime;

    auto& registry = m_timeline->getRegistry();

    // Read placement for both Clip-backed entities (source of truth: Clip)
    // and Layer-only entities (ObjectAnimation / Generative). Durations
    // throughout are in timeline frames, so the timeline frame rate is
    // the correct denominator regardless of source media frame rate.
    auto readDuration = [&](entt::entity e) -> std::pair<FrameNumber, bool> {
        if (const auto* clip = registry.try_get<Clip>(e)) {
            return {clip->duration, true};
        }
        if (const auto* lay = registry.try_get<Layer>(e);
            lay && (registry.all_of<ObjectAnimationLayer>(e) ||
                    registry.all_of<GenerativeLayer>(e)       ||
                    registry.all_of<SignalLayer>(e))) {
            return {lay->duration, true};
        }
        return {0, false};
    };
    auto readStartAndDuration = [&](entt::entity e) -> std::tuple<FrameNumber, FrameNumber, bool> {
        if (const auto* clip = registry.try_get<Clip>(e)) {
            return {clip->startFrame, clip->duration, true};
        }
        if (const auto* lay = registry.try_get<Layer>(e);
            lay && (registry.all_of<ObjectAnimationLayer>(e) ||
                    registry.all_of<GenerativeLayer>(e)       ||
                    registry.all_of<SignalLayer>(e))) {
            return {lay->startFrame, lay->duration, true};
        }
        return {0, 0, false};
    };

    auto [movingDur, movingValid] = readDuration(clipEntity);
    if (!movingValid) return newStartTime;

    const double timelineFps = m_timeline->getFrameRate();
    if (timelineFps <= 0.0) return newStartTime;

    // Calculate moving clip's time range at new position
    float durationSeconds = movingDur / static_cast<float>(timelineFps);
    Timecode newEndTime = newStartTime + static_cast<Timecode>(durationSeconds * 1000000.0f);

    // Get the track
    const auto& tracks = m_timeline->getTracks();
    if (trackIndex < 0 || trackIndex >= static_cast<int>(tracks.size())) {
        return newStartTime;
    }

    entt::entity trackEntity = tracks[trackIndex];
    const auto* track = registry.try_get<TimelineTrack>(trackEntity);
    if (!track) return newStartTime;

    // Check for collisions with other clips on the same track
    Timecode snapPosition = newStartTime;
    float minDistance = std::numeric_limits<float>::max();

    for (entt::entity otherClipEntity : track->layers) {
        // Skip the clip we're moving
        if (otherClipEntity == clipEntity) continue;

        auto [otherStart, otherDur, otherValid] = readStartAndDuration(otherClipEntity);
        if (!otherValid) continue;

        // Calculate other clip's time range (timeline frames -> seconds)
        float otherStartSeconds = otherStart / static_cast<float>(timelineFps);
        float otherDurationSeconds = otherDur / static_cast<float>(timelineFps);
        Timecode otherStartTime = static_cast<Timecode>(otherStartSeconds * 1000000.0f);
        Timecode otherEndTime = otherStartTime + static_cast<Timecode>(otherDurationSeconds * 1000000.0f);

        // Check for overlap
        bool overlaps = (newStartTime < otherEndTime && newEndTime > otherStartTime);

        if (overlaps) {
            // Calculate snap positions (before or after the other clip)
            Timecode snapBefore = otherStartTime - static_cast<Timecode>(durationSeconds * 1000000.0f);
            Timecode snapAfter = otherEndTime;

            // Choose the snap position closest to the desired position
            float distBefore = std::abs(static_cast<float>(snapBefore - newStartTime));
            float distAfter = std::abs(static_cast<float>(snapAfter - newStartTime));

            Timecode candidateSnap = (distBefore < distAfter) ? snapBefore : snapAfter;
            float candidateDistance = std::min(distBefore, distAfter);

            // Keep track of the closest valid position
            if (candidateDistance < minDistance) {
                minDistance = candidateDistance;
                snapPosition = candidateSnap;
            }
        }
    }

    // Clamp snap position to valid range (>= 0)
    if (snapPosition < 0) snapPosition = 0;

    return snapPosition;
}

Timecode TimelineWidget::snapTimeToTickGrid(Timecode t) const {
    if (!m_timeline || m_timeline->getFrameRate() <= 0.0) return t;
    const FrameNumber tickEvery = static_cast<FrameNumber>(framesPerTick());
    if (tickEvery <= 0) return t;
    // Resolve the cursor to a whole frame the SAME way the hover-highlight
    // does (Timeline::timeToFrame rounds), then floor to the tick-grid cell.
    // Using round here — not std::floor — keeps section-break / cue placement
    // landing on exactly the division the hover band is showing.
    FrameNumber f = m_timeline->timeToFrame(t);
    if (f < 0) f = 0;
    f = (f / tickEvery) * tickEvery;
    return m_timeline->frameToTime(f);
}

Timecode TimelineWidget::snapTimeToCues(Timecode t, Timecode tol) const {
    if (!m_timeline) return t;
    const auto& cues = m_timeline->getCueTags();
    Timecode best = t;
    Timecode bestDist = tol + 1;
    for (const auto& cue : cues) {
        // Skip the cue currently being dragged so it can't snap to its own
        // pre-drag position (which would freeze the drag in place).
        if (m_isDraggingCue && cue.number == m_draggedCueNumber) continue;
        // Cues are stored as integer frames; convert to the microsecond
        // axis for the pixel-distance snap test.
        const Timecode cueTime = m_timeline->frameToTime(cue.frame);
        const Timecode d = std::llabs(static_cast<long long>(cueTime - t));
        if (d <= tol && d < bestDist) {
            bestDist = d;
            best = cueTime;
        }
    }
    return best;
}

Timecode TimelineWidget::snapTimeToSections(Timecode t, Timecode tol) const {
    if (!m_timeline) return t;
    const auto& sections = m_timeline->getSections();
    Timecode best = t;
    Timecode bestDist = tol + 1;
    for (const auto& sec : sections) {
        // Section breaks are stored as integer frames; convert to the
        // microsecond axis for the pixel-distance snap test.
        const Timecode secTime = m_timeline->frameToTime(sec.breakFrame);
        const Timecode d = std::llabs(static_cast<long long>(secTime - t));
        if (d <= tol && d < bestDist) {
            bestDist = d;
            best = secTime;
        }
    }
    return best;
}

Timecode TimelineWidget::snapTimeToBest(Timecode t) const {
    if (!m_timeline) return t;
    const Timecode tol = pixelToTime(kSnapPx);
    const Timecode gridT = snapTimeToTickGrid(t);
    const Timecode cueT  = snapTimeToCues(t, tol);
    const Timecode secT  = snapTimeToSections(t, tol);

    Timecode best = gridT;
    Timecode bestDist = std::llabs(static_cast<long long>(gridT - t));
    auto consider = [&](Timecode candidate) {
        const Timecode d = std::llabs(static_cast<long long>(candidate - t));
        if (d < bestDist) {
            bestDist = d;
            best = candidate;
        }
    };
    consider(cueT);
    consider(secT);
    return best;
}

bool TimelineWidget::wouldOverlapAnyClip(int trackIndex, Timecode startTime,
                                         Timecode durationTime,
                                         entt::entity excludeEntity) const {
    if (!m_timeline || trackIndex < 0 || durationTime <= 0) return false;
    auto& registry = m_timeline->getRegistry();
    const auto& tracks = m_timeline->getTracks();
    if (trackIndex >= static_cast<int>(tracks.size())) return false;
    const auto* track = registry.try_get<TimelineTrack>(tracks[trackIndex]);
    if (!track) return false;

    const double fps = m_timeline->getFrameRate();
    if (fps <= 0.0) return false;

    const Timecode endTime = startTime + durationTime;

    // Mirror checkClipCollision's archetype branching but boolean-only.
    auto readPlacement = [&](entt::entity e) -> std::tuple<FrameNumber, FrameNumber, bool> {
        if (const auto* clip = registry.try_get<Clip>(e)) {
            return {clip->startFrame, clip->duration, true};
        }
        if (const auto* lay = registry.try_get<Layer>(e);
            lay && (registry.all_of<ObjectAnimationLayer>(e) ||
                    registry.all_of<GenerativeLayer>(e)       ||
                    registry.all_of<SignalLayer>(e))) {
            return {lay->startFrame, lay->duration, true};
        }
        return {0, 0, false};
    };

    for (entt::entity other : track->layers) {
        if (other == excludeEntity) continue;
        auto [oStart, oDur, valid] = readPlacement(other);
        if (!valid) continue;

        const float oStartSec = oStart / static_cast<float>(fps);
        const float oDurSec   = oDur   / static_cast<float>(fps);
        const Timecode oStartT = static_cast<Timecode>(oStartSec * 1000000.0f);
        const Timecode oEndT   = static_cast<Timecode>((oStartSec + oDurSec) * 1000000.0f);

        if (startTime < oEndT && endTime > oStartT) return true;
    }
    return false;
}

bool TimelineWidget::wouldOverlapAnyClipExcluding(
        int trackIndex, Timecode startTime, Timecode durationTime,
        const std::vector<entt::entity>& exclude) const {
    if (!m_timeline || trackIndex < 0 || durationTime <= 0) return false;
    auto& registry = m_timeline->getRegistry();
    const auto& tracks = m_timeline->getTracks();
    if (trackIndex >= static_cast<int>(tracks.size())) return false;
    const auto* track = registry.try_get<TimelineTrack>(tracks[trackIndex]);
    if (!track) return false;
    const double fps = m_timeline->getFrameRate();
    if (fps <= 0.0) return false;

    const Timecode endTime = startTime + durationTime;
    auto isExcluded = [&](entt::entity e) {
        return std::find(exclude.begin(), exclude.end(), e) != exclude.end();
    };
    auto readPl = [&](entt::entity e) -> std::tuple<FrameNumber, FrameNumber, bool> {
        if (const auto* clip = registry.try_get<Clip>(e)) {
            return {clip->startFrame, clip->duration, true};
        }
        if (const auto* lay = registry.try_get<Layer>(e);
            lay && (registry.all_of<ObjectAnimationLayer>(e) ||
                    registry.all_of<GenerativeLayer>(e)       ||
                    registry.all_of<SignalLayer>(e))) {
            return {lay->startFrame, lay->duration, true};
        }
        return {0, 0, false};
    };

    for (entt::entity other : track->layers) {
        if (isExcluded(other)) continue;
        auto [oStart, oDur, valid] = readPl(other);
        if (!valid) continue;
        const float oStartSec = oStart / static_cast<float>(fps);
        const float oDurSec   = oDur   / static_cast<float>(fps);
        const Timecode oStartT = static_cast<Timecode>(oStartSec * 1000000.0f);
        const Timecode oEndT   = static_cast<Timecode>((oStartSec + oDurSec) * 1000000.0f);
        if (startTime < oEndT && endTime > oStartT) return true;
    }
    return false;
}

int TimelineWidget::findTrackIndexForClip(entt::entity clip) const {
    if (!m_timeline) return -1;
    auto& registry = m_timeline->getRegistry();
    const auto& tracks = m_timeline->getTracks();
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i) {
        const auto* track = registry.try_get<TimelineTrack>(tracks[i]);
        if (!track) continue;
        if (std::find(track->layers.begin(), track->layers.end(), clip)
            != track->layers.end()) {
            return i;
        }
    }
    return -1;
}

Timecode TimelineWidget::snapDropPosition(Timecode rawTime, int trackIndex) const {
    (void)trackIndex;  // reserved for a future track-local snap variant
    if (!m_timeline) return rawTime;
    if (rawTime < 0) rawTime = 0;

    const Timecode tol = pixelToTime(SNAP_THRESHOLD_PIXELS);
    Timecode chosen = rawTime;
    Timecode bestDist = tol + 1;
    auto consider = [&](Timecode candidate) {
        if (candidate == rawTime) return;
        const Timecode d = std::llabs(static_cast<long long>(candidate - rawTime));
        if (d <= tol && d < bestDist) {
            bestDist = d;
            chosen = candidate;
        }
    };

    consider(m_timeline->getCurrentTime());          // playhead
    consider(snapTimeToCues(rawTime, tol));
    consider(snapTimeToSections(rawTime, tol));
    consider(snapTimeToTickGrid(rawTime));           // always-on grid for drops

    return chosen;
}

void TimelineWidget::computeCueLaneLayout(std::vector<CueLaneSlot>& outSlots, int& outRowsUsed) const {
    outSlots.clear();
    outRowsUsed = 0;
    if (!m_timeline) return;
    const auto& cues = m_timeline->getCueTags();
    if (cues.empty()) return;

    // Build sorted index list by timestamp so we walk left-to-right
    // (cue vector is sorted by `number`, not time).
    std::vector<size_t> order;
    order.reserve(cues.size());
    for (size_t i = 0; i < cues.size(); ++i) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return cues[a].frame < cues[b].frame;
    });

    // Greedy left-to-right with per-row "rightmost-end" trackers. For each
    // cue compute its label rect, place in the lowest row whose previous
    // rightmost end is <= this cue's left edge. New row if all rows
    // collide. Capped at kMaxCueRows; over-cap rows fall back to short
    // (number-only) labels and re-attempt; if still over-cap, force-place
    // on the last row (visual overlap accepted as the degenerate case).
    constexpr float kFlagPadX = 4.0f;
    constexpr float kFlagSepX = 2.0f;
    std::vector<float> rowRightX(kMaxCueRows, -1.0e9f);
    int maxRowAssigned = -1;

    auto measureLabel = [](const CueTag& cue, bool useLong) -> float {
        char buf[320];
        if (useLong && !cue.label.empty()) {
            std::snprintf(buf, sizeof(buf), "%.2f [%s]", cue.number, cue.label.c_str());
        } else {
            std::snprintf(buf, sizeof(buf), "%.2f", cue.number);
        }
        return ImGui::CalcTextSize(buf).x + 2.0f * kFlagPadX;
    };

    outSlots.reserve(order.size());
    for (size_t idx : order) {
        const CueTag& cue = cues[idx];
        const float x = frameToPixel(cue.frame);

        bool useLong = !cue.label.empty();
        float w = measureLabel(cue, useLong);

        int chosenRow = -1;
        for (int r = 0; r < kMaxCueRows; ++r) {
            if (x >= rowRightX[r] + kFlagSepX) { chosenRow = r; break; }
        }
        if (chosenRow < 0 && useLong) {
            useLong = false;
            w = measureLabel(cue, false);
            for (int r = 0; r < kMaxCueRows; ++r) {
                if (x >= rowRightX[r] + kFlagSepX) { chosenRow = r; break; }
            }
        }
        if (chosenRow < 0) {
            chosenRow = kMaxCueRows - 1;
            useLong = false;
            w = measureLabel(cue, false);
        }

        rowRightX[chosenRow] = x + w;
        if (chosenRow > maxRowAssigned) maxRowAssigned = chosenRow;

        CueLaneSlot s;
        s.cueIndex = idx;
        s.row = chosenRow;
        s.labelW = w;
        s.longLabel = useLong;
        outSlots.push_back(s);
    }

    outRowsUsed = (maxRowAssigned >= 0) ? (maxRowAssigned + 1) : 0;
}

void TimelineWidget::buildTrackRows() {
    m_trackRows.clear();
    if (!m_timeline) return;

    auto& registry = m_timeline->getRegistry();
    const auto& tracks = m_timeline->getTracks();
    const bool hideShy = m_timeline->getHideShyTracks();

    float cumulativeY = 0.0f;
    for (size_t i = 0; i < tracks.size(); ++i) {
        const entt::entity trackEntity = tracks[i];

        // Shy skip: this is the ONE place visibility is decided. A shy track
        // with the global hide on is omitted entirely — it contributes no row
        // and no Y, so it vanishes from header panel, lanes, and every
        // hit-test. modelIndex still tracks the real Timeline index for
        // commands.
        if (hideShy) {
            const auto* ui = registry.try_get<TrackUiState>(trackEntity);
            if (ui && ui->shy) continue;
        }

        TrackRow row;
        row.track      = trackEntity;
        row.modelIndex = static_cast<int>(i);
        row.y          = cumulativeY;
        row.expanded   = m_expandedTracks.count(static_cast<uint32_t>(trackEntity)) > 0 ||
                         m_timeline->isTrackExpanded(trackEntity);

        float trackHeight = TRACK_HEIGHT;
        if (row.expanded) {
            entt::entity atPlayhead = findClipAtPlayhead(trackEntity);
            if (atPlayhead != entt::null) {
                row.clipAtPlayhead  = atPlayhead;
                row.propPanelHeight = expandedPropertyRowCount(atPlayhead) * PROPERTY_ROW_HEIGHT;
                trackHeight += row.propPanelHeight;
            }
        }
        row.height = trackHeight;
        m_trackRows.push_back(row);

        cumulativeY += trackHeight + TRACK_PADDING;
    }
}

const TimelineWidget::TrackRow* TimelineWidget::rowForModelIndex(int modelIndex) const {
    for (const auto& row : m_trackRows) {
        if (row.modelIndex == modelIndex) return &row;
    }
    return nullptr;
}

int TimelineWidget::findTrackAtY(float mouseY, float windowY) const {
    if (!m_timeline) return -1;

    // A track "hit" is the clip-body band only (TRACK_HEIGHT) — the expanded
    // property panel below a clip is not a valid clip-drop zone. Iterates the
    // per-frame row cache so hidden (shy) tracks are skipped and expanded
    // tracks above shift everything below by the cached row height. Returns
    // the model index (commands address tracks by it).
    for (const auto& row : m_trackRows) {
        const float trackY = windowY + row.y;
        if (mouseY >= trackY && mouseY <= trackY + TRACK_HEIGHT) {
            return row.modelIndex;
        }
    }
    return -1;
}

} // namespace entity
