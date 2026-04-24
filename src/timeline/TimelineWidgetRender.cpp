/**
 * TimelineWidget drawing code.
 *
 * Split from TimelineWidget.cpp (Phase B #16). All render*() methods live here;
 * input/hit-testing lives in TimelineWidgetInput.cpp; core dispatch + geometry
 * helpers stay in TimelineWidget.cpp.
 */

#include "entity/timeline/TimelineWidget.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/FrameBuffer.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include "entity/media/FrameRingBuffer.hpp"
#include <sstream>
#include <iomanip>
#include <cmath>
#include <cstdio>
#include <set>
#include <limits>

namespace entity {

void TimelineWidget::renderTimeRuler() {
    if (!m_timeline) return;

    ImVec2 windowPos = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Get timeline framerate
    double frameRate = m_timeline->getFrameRate();
    float durationSeconds = m_timeline->getDuration() / 1000000.0f;
    FrameNumber totalFrames = static_cast<FrameNumber>(durationSeconds * frameRate);
    const float timelinePx = durationSeconds * m_pixelsPerSecond;

    // Ruler bounds: background stretches across the FULL content width, not the
    // visible region (GetContentRegionAvail caps at window width, which makes
    // the background vanish once the user scrolls past a small offset). ImGui's
    // clip rect handles hiding the off-screen portion.
    ImVec2 rulerMin = windowPos;
    ImVec2 rulerMax = ImVec2(windowPos.x + timelinePx, windowPos.y + RULER_HEIGHT);

    // Draw ruler background
    drawList->AddRectFilled(rulerMin, rulerMax, IM_COL32(40, 40, 40, 255));

    // Tick stride = zoom-ladder value. Tick pixel spacing is fixed at TICK_PX;
    // zoom changes the time each tick represents, not the spacing. Labels only
    // appear on every 5th tick (major ticks) so the ruler doesn't get crowded;
    // minor ticks are shorter and unlabeled.
    const FrameNumber tickEvery = static_cast<FrameNumber>(framesPerTick());
    const int LABEL_EVERY_N = 5;

    // Only iterate the visible range — at extreme zoom (1f over 10 min) the
    // total frame count hits 18000. Drawing all of them would blow past ImGui's
    // default 16-bit vertex index limit and silently drop past a threshold.
    const float pxPerFrame = m_pixelsPerSecond / static_cast<float>(frameRate);
    const float visibleStartPx = m_syncScrollX - TICK_PX;
    const float visibleEndPx   = m_syncScrollX + m_lastVisibleWidth + TICK_PX;
    FrameNumber firstVisibleFrame = pxPerFrame > 0.0f
        ? std::max<FrameNumber>(0, static_cast<FrameNumber>(visibleStartPx / pxPerFrame))
        : 0;
    FrameNumber lastVisibleFrame = pxPerFrame > 0.0f
        ? std::min(totalFrames, static_cast<FrameNumber>(visibleEndPx / pxPerFrame) + 1)
        : totalFrames;
    // Snap to tick-stride grid and preserve the tickCounter modulo relative to
    // frame 0 so major/minor striping stays stable as the user scrolls.
    FrameNumber firstTick = (firstVisibleFrame / tickEvery) * tickEvery;
    int tickCounter = static_cast<int>(firstTick / tickEvery);

    for (FrameNumber frame = firstTick; frame <= lastVisibleFrame; frame += tickEvery, ++tickCounter) {
        float timeSeconds = static_cast<float>(frame) / static_cast<float>(frameRate);
        float x = windowPos.x + timeToPixel(static_cast<Timecode>(timeSeconds * 1000000.0f));

        const bool isMajor = (tickCounter % LABEL_EVERY_N == 0);
        const float tickHeight = isMajor ? 12.0f : 6.0f;
        const ImU32 tickColor = isMajor ? IM_COL32(200, 200, 200, 255)
                                        : IM_COL32(140, 140, 140, 255);
        drawList->AddRectFilled(
            ImVec2(x, rulerMax.y - tickHeight),
            ImVec2(x + 1.0f, rulerMax.y),
            tickColor
        );

        if (!isMajor) continue;

        // SMPTE timecode label HH:MM:SS:FF on every major (5th) tick.
        int totalSeconds = static_cast<int>(frame / frameRate);
        int hours = totalSeconds / 3600;
        int minutes = (totalSeconds % 3600) / 60;
        int seconds = totalSeconds % 60;
        int frames = static_cast<int>(frame % static_cast<FrameNumber>(frameRate));

        std::ostringstream oss;
        if (hours > 0) {
            oss << std::setfill('0') << std::setw(2) << hours << ":";
        }
        oss << std::setfill('0') << std::setw(2) << minutes << ":"
            << std::setfill('0') << std::setw(2) << seconds << ":"
            << std::setfill('0') << std::setw(2) << frames;

        drawList->AddText(
            ImVec2(x + 3.0f, rulerMin.y + 5.0f),
            IM_COL32(200, 200, 200, 255),
            oss.str().c_str()
        );
    }

    // Named-section bands. Drawn before the range-selection overlay so the
    // active selection always sits on top (the selection is what the next
    // ripple/section op acts on, so it gets visual priority).
    {
        const float bandH = 6.0f;
        const float bandY = rulerMin.y + 1.0f;
        for (const auto& sec : m_timeline->getSections()) {
            if (sec.end <= sec.start) continue;
            const float xs = windowPos.x + timeToPixel(sec.start);
            const float xe = windowPos.x + timeToPixel(sec.end);
            if (xe < windowPos.x || xs > rulerMax.x) continue;
            drawList->AddRectFilled(ImVec2(xs, bandY), ImVec2(xe, bandY + bandH), sec.color);
            // Section name centered, clamped to the band's pixel bounds so
            // long names don't bleed past the boundary.
            if (!sec.name.empty()) {
                const float textW = ImGui::CalcTextSize(sec.name.c_str()).x;
                const float bandW = xe - xs;
                if (bandW > 12.0f) {
                    const float labelX = std::max(xs + 4.0f, xs + (bandW - textW) * 0.5f);
                    drawList->PushClipRect(ImVec2(xs, bandY), ImVec2(xe, bandY + 18.0f), true);
                    drawList->AddText(ImVec2(labelX, bandY + bandH + 1.0f),
                        IM_COL32(240, 240, 240, 230), sec.name.c_str());
                    drawList->PopClipRect();
                }
            }
        }
    }

    // Range-selection overlay on the ruler — stronger tint than the tracks
    // band so the endpoints are obviously interactive. While the user is
    // actively shift+dragging, show the In/Out/Δ frame readout near the
    // mouse so they can land an exact span without fishing.
    if (m_range.active && m_range.end > m_range.start) {
        const float xStart = windowPos.x + timeToPixel(m_range.start);
        const float xEnd   = windowPos.x + timeToPixel(m_range.end);
        drawList->AddRectFilled(
            ImVec2(xStart, rulerMin.y), ImVec2(xEnd, rulerMax.y),
            IM_COL32(80, 160, 255, 80));
        drawList->AddLine(ImVec2(xStart, rulerMin.y), ImVec2(xStart, rulerMax.y),
            IM_COL32(160, 220, 255, 255), 1.5f);
        drawList->AddLine(ImVec2(xEnd, rulerMin.y), ImVec2(xEnd, rulerMax.y),
            IM_COL32(160, 220, 255, 255), 1.5f);

        if (m_isCreatingRange) {
            FrameNumber inF  = m_timeline->timeToFrame(m_range.start);
            FrameNumber outF = m_timeline->timeToFrame(m_range.end);
            char buf[96];
            std::snprintf(buf, sizeof(buf), "In F%lld | Out F%lld | %lldf",
                static_cast<long long>(inF),
                static_cast<long long>(outF),
                static_cast<long long>(outF - inF));
            const float labelX = std::min(xEnd + 6.0f, rulerMax.x - 200.0f);
            drawList->AddText(ImVec2(labelX, rulerMin.y + 5.0f),
                IM_COL32(220, 240, 255, 255), buf);
        }
    }
}

void TimelineWidget::renderTracks() {
    if (!m_timeline) return;

    // Get base window position ONCE at the start
    // This prevents cursor drift from InvisibleButtons affecting subsequent tracks
    ImVec2 baseWindowPos = ImGui::GetCursorScreenPos();

    // Range-selection band across all tracks. Drawn before the track loop
    // so track bg fills can sit on top — the band tint mixes with the bg.
    // We re-draw the endpoint accent lines AFTER the track loop so they
    // remain crisp on top of clips.
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const float visibleH = ImGui::GetWindowHeight();
        const float scrollX = m_syncScrollX;
        if (m_range.active && m_range.end > m_range.start) {
            const float xStart = baseWindowPos.x + (timeToPixel(m_range.start) - scrollX);
            const float xEnd   = baseWindowPos.x + (timeToPixel(m_range.end)   - scrollX);
            const float yTop = baseWindowPos.y;
            const float yBot = baseWindowPos.y + visibleH;
            drawList->AddRectFilled(ImVec2(xStart, yTop), ImVec2(xEnd, yBot),
                IM_COL32(80, 160, 255, 36));
        }
    }

    // Calculate cumulative Y offset for tracks to account for expanded tracks/clips
    float cumulativeY = 0.0f;
    const auto& tracks = m_timeline->getTracks();
    auto& registry = m_timeline->getRegistry();

    for (size_t i = 0; i < tracks.size(); ++i) {
        // Calculate track Y position including offset from previous expanded tracks
        float trackY = baseWindowPos.y + cumulativeY;
        ImVec2 trackBasePos(baseWindowPos.x, trackY);

        // Calculate track height (including expanded clips from header hierarchy)
        float trackHeight = TRACK_HEIGHT;
        uint32_t trackId = static_cast<uint32_t>(tracks[i]);
        bool trackExpanded = m_expandedTracks.count(trackId) > 0 || m_timeline->isTrackExpanded(tracks[i]);

        const auto* track = registry.try_get<TimelineTrack>(tracks[i]);

        renderTrack(tracks[i], static_cast<int>(i), trackBasePos, trackHeight);

        // Render expanded clip content (property tracks with keyframe diamonds)
        // This must match the Y positions used in the header panel
        if (trackExpanded && track) {
            // Clip rows start right after track (no padding between track and first clip)
            float clipContentY = cumulativeY + trackHeight;

            for (entt::entity clipEntity : track->clips) {
                // Space for clip header row (no visual on timeline, just spacing)
                clipContentY += HEADER_ROW_HEIGHT;

                uint32_t clipId = static_cast<uint32_t>(clipEntity);
                bool clipExpanded = m_expandedClips.count(clipId) > 0 || m_timeline->isClipExpanded(clipEntity);
                if (clipExpanded) {
                    // Render property tracks (keyframe diamonds) at this Y position
                    float propY = baseWindowPos.y + clipContentY;
                    renderPropertyTracks(clipEntity, static_cast<int>(i), baseWindowPos, propY);
                    clipContentY += 6 * PROPERTY_ROW_HEIGHT;
                }
            }

            // Update cumulative Y to account for all expanded content + padding
            cumulativeY = clipContentY + TRACK_PADDING;
        } else {
            cumulativeY += trackHeight + TRACK_PADDING;
        }
    }

    // Tick gridlines + range endpoint accents — drawn AFTER the track loop
    // so they sit on top of the (opaque) track-bg fills and clip rects.
    // Iteration is clamped to the visible viewport so a 24h timeline at
    // 1f tick spacing doesn't enumerate millions of off-screen frames.
    //
    // gridBot extends to whichever is larger of:
    //   (a) cumulativeY — the painted track-content area (so lines reach
    //       every track even when the pane is shorter than total content
    //       and the user scrolls), and
    //   (b) the child window's visible height — so lines also extend
    //       through the empty canvas BELOW the last track when the pane
    //       is taller than total content. Without this, the grid stops
    //       partway down the visible pane and looks like it's bound to
    //       the pane rather than the timeline.
    // The actual draw is scissor-clipped by ImGui to the child's visible
    // bounds, so overdrawing past the bottom is free.
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const float visibleW = ImGui::GetWindowWidth();
        const float visibleH = ImGui::GetWindowHeight();
        const float scrollY = ImGui::GetScrollY();
        const float scrollX = m_syncScrollX;
        const double frameRate = m_timeline->getFrameRate();
        const FrameNumber tickEvery = static_cast<FrameNumber>(framesPerTick());
        const float pxPerFrame = m_pixelsPerSecond / static_cast<float>(frameRate);

        // Visible bottom in screen coords (window-y baseline + visible
        // height). baseWindowPos.y already includes -scrollY, so add it
        // back to translate cumulativeY's content-space bottom into the
        // same screen-coord reference as the visible bottom.
        const float gridTop = baseWindowPos.y;
        const float contentBot = baseWindowPos.y + cumulativeY;
        const float visibleBot = baseWindowPos.y + scrollY + visibleH;
        const float gridBot = std::max(contentBot, visibleBot);

        if (pxPerFrame > 0.0f && tickEvery > 0) {
            FrameNumber firstFrame = static_cast<FrameNumber>(std::floor(scrollX / pxPerFrame));
            FrameNumber lastFrame  = static_cast<FrameNumber>(std::ceil((scrollX + visibleW) / pxPerFrame));
            firstFrame = std::max<FrameNumber>(0, (firstFrame / tickEvery) * tickEvery);

            for (FrameNumber f = firstFrame; f <= lastFrame; f += tickEvery) {
                // Cull on xRel (content offset from scroll — tells us if this
                // frame is in the viewport) but draw at absolute content pos:
                // baseWindowPos.x already includes -Scroll (ImGui sets
                // CursorStartPos = Pos + Padding - Scroll), so adding xRel
                // would subtract scroll a second time and push the line off
                // screen left. Use f*pxPerFrame directly.
                const float xRel = f * pxPerFrame - scrollX;
                if (xRel < 0 || xRel > visibleW) continue;
                const float x = baseWindowPos.x + f * pxPerFrame;
                drawList->AddRectFilled(
                    ImVec2(x, gridTop),
                    ImVec2(x + 1.0f, gridBot),
                    IM_COL32(255, 255, 255, 56)
                );
            }
        }

        // Range endpoint accent lines on top of clips so the boundary stays
        // visible even when a clip is colored similar to the band tint.
        if (m_range.active && m_range.end > m_range.start) {
            const float xStart = baseWindowPos.x + (timeToPixel(m_range.start) - scrollX);
            const float xEnd   = baseWindowPos.x + (timeToPixel(m_range.end)   - scrollX);
            drawList->AddLine(ImVec2(xStart, gridTop), ImVec2(xStart, gridBot),
                IM_COL32(120, 200, 255, 220), 1.0f);
            drawList->AddLine(ImVec2(xEnd, gridTop), ImVec2(xEnd, gridBot),
                IM_COL32(120, 200, 255, 220), 1.0f);
        }
    }
}

void TimelineWidget::renderTrack(entt::entity trackEntity, int trackIndex, ImVec2 baseWindowPos, float trackHeight) {
    if (!m_timeline) return;

    ImDrawList* drawList = ImGui::GetWindowDrawList();

    auto& registry = m_timeline->getRegistry();
    const auto* track = registry.try_get<TimelineTrack>(trackEntity);
    if (!track) return;

    // Track rect spans the full timeline content width, not GetContentRegionAvail
    // (which caps at window width and makes the background vanish past a small
    // scroll offset). ImGui's clip rect handles hiding the off-screen portion.
    const float durationSec = m_timeline->getDuration() / 1000000.0f;
    const float timelinePx = durationSec * m_pixelsPerSecond;
    float trackY = baseWindowPos.y;
    ImVec2 trackMin = ImVec2(baseWindowPos.x, trackY);
    ImVec2 trackMax = ImVec2(baseWindowPos.x + timelinePx, trackY + trackHeight);

    // Draw track background (alternating colors)
    ImU32 trackColor = (trackIndex % 2 == 0)
        ? IM_COL32(50, 50, 50, 255)
        : IM_COL32(45, 45, 45, 255);
    drawList->AddRectFilled(trackMin, trackMax, trackColor);

    // Draw track border
    drawList->AddRect(trackMin, trackMax, IM_COL32(70, 70, 70, 255));

    // Draw track label
    std::ostringstream labelStream;
    labelStream << "Track " << (trackIndex + 1);
    drawList->AddText(
        ImVec2(trackMin.x + 10.0f, trackMin.y + TRACK_HEIGHT / 2.0f - 7.0f),
        IM_COL32(180, 180, 180, 255),
        labelStream.str().c_str()
    );

    // Render clips in this track FIRST (so we can handle clip clicks before drag-drop)
    for (entt::entity clipEntity : track->clips) {
        renderClip(clipEntity, trackIndex, baseWindowPos);
    }

    // Handle drag-drop target for this track AFTER rendering clips
    // Use InvisibleButton with AllowOverlap to not block clip interaction
    ImGui::SetCursorScreenPos(trackMin);
    std::ostringstream dropTargetId;
    dropTargetId << "##trackDropTarget" << trackIndex;

    // Allow this button to overlap with other items (clip rendering uses draw list, not ImGui widgets)
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton(dropTargetId.str().c_str(), ImVec2(trackMax.x - trackMin.x, trackHeight));

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MEDIA_FILE")) {
            // Extract the file path from the payload
            const char* droppedPath = static_cast<const char*>(payload->Data);
            std::string filepath(droppedPath);

            // Calculate the drop position in timeline time
            ImVec2 mousePos = ImGui::GetMousePos();
            float relativeX = mousePos.x - baseWindowPos.x + m_syncScrollX;
            Timecode dropTime = pixelToTime(relativeX);
            if (dropTime < 0) dropTime = 0;

            // Call the callback if set
            if (m_mediaDropCallback) {
                m_mediaDropCallback(filepath, trackIndex, dropTime);
            }
        }
        ImGui::EndDragDropTarget();
    }
}

float TimelineWidget::renderClip(entt::entity clipEntity, int trackIndex, ImVec2 baseWindowPos) {
    (void)trackIndex;  // trackIndex no longer used for Y calculation
    if (!m_timeline) return TRACK_HEIGHT;

    ImDrawList* drawList = ImGui::GetWindowDrawList();

    auto& registry = m_timeline->getRegistry();
    const auto* clip = registry.try_get<Clip>(clipEntity);
    if (!clip) return TRACK_HEIGHT;

    // Check if this clip is expanded (check both local and Timeline state)
    uint32_t entityId = static_cast<uint32_t>(clipEntity);
    bool isExpanded = m_expandedClips.count(entityId) > 0 || m_timeline->isClipExpanded(clipEntity);

    // Use baseWindowPos.y directly - it already includes cumulative offset from renderTracks
    float trackY = baseWindowPos.y;

    // Convert frame timing to microseconds (Timecode units)
    // Use timeline frame rate (not clip source rate) since startFrame and duration are in timeline frames
    double timelineFrameRate = m_timeline->getFrameRate();
    float startSeconds = static_cast<float>(clip->startFrame) / static_cast<float>(timelineFrameRate);
    float durationSeconds = static_cast<float>(clip->duration) / static_cast<float>(timelineFrameRate);
    Timecode startTime = static_cast<Timecode>(startSeconds * 1000000.0f);
    Timecode endTime = static_cast<Timecode>((startSeconds + durationSeconds) * 1000000.0f);

    float clipX = baseWindowPos.x + timeToPixel(startTime);
    float clipWidth = timeToPixel(endTime - startTime);

    ImVec2 clipMin = ImVec2(clipX, trackY + CLIP_PADDING);
    ImVec2 clipMax = ImVec2(clipX + clipWidth, trackY + TRACK_HEIGHT - CLIP_PADDING);

    // Choose clip color based on selection (check both local and Timeline state)
    bool isSelected = (clipEntity == m_selectedClip) || (clipEntity == m_timeline->getSelectedClip());
    ImU32 clipColor = isSelected
        ? IM_COL32(100, 150, 255, 255)
        : IM_COL32(80, 120, 180, 255);

    // Draw clip rectangle
    drawList->AddRectFilled(clipMin, clipMax, clipColor, 3.0f);
    drawList->AddRect(clipMin, clipMax, IM_COL32(120, 160, 220, 255), 3.0f, 0, 2.0f);

    // Draw twirl-down triangle for expansion (only when selected)
    if (isSelected && clipWidth > 30) {
        float triSize = 6.0f;
        float triX = clipMin.x + 8.0f;
        float triY = clipMin.y + 8.0f;

        ImVec2 triPoints[3];
        if (isExpanded) {
            // Down-pointing triangle (expanded)
            triPoints[0] = ImVec2(triX - triSize, triY - triSize/2);
            triPoints[1] = ImVec2(triX + triSize, triY - triSize/2);
            triPoints[2] = ImVec2(triX, triY + triSize/2);
        } else {
            // Right-pointing triangle (collapsed)
            triPoints[0] = ImVec2(triX - triSize/2, triY - triSize);
            triPoints[1] = ImVec2(triX + triSize/2, triY);
            triPoints[2] = ImVec2(triX - triSize/2, triY + triSize);
        }
        drawList->AddTriangleFilled(triPoints[0], triPoints[1], triPoints[2], IM_COL32(255, 255, 255, 200));

        // Handle click on twirl-down
        ImVec2 triHitMin(triX - triSize - 4, triY - triSize - 4);
        ImVec2 triHitMax(triX + triSize + 4, triY + triSize + 4);
        if (ImGui::IsMouseClicked(0)) {
            ImVec2 mousePos = ImGui::GetMousePos();
            if (mousePos.x >= triHitMin.x && mousePos.x <= triHitMax.x &&
                mousePos.y >= triHitMin.y && mousePos.y <= triHitMax.y) {
                if (isExpanded) {
                    m_expandedClips.erase(entityId);
                } else {
                    m_expandedClips.insert(entityId);
                }
            }
        }
    }

    // Draw clip label (filename without path)
    size_t lastSlash = clip->filepath.find_last_of("/\\");
    std::string filename = (lastSlash != std::string::npos)
        ? clip->filepath.substr(lastSlash + 1)
        : clip->filepath;

    // Truncate filename if too long
    if (filename.length() > 20) {
        filename = filename.substr(0, 17) + "...";
    }

    // Offset label if twirl-down is visible
    float labelOffsetX = isSelected ? 20.0f : 5.0f;
    drawList->AddText(
        ImVec2(clipMin.x + labelOffsetX, clipMin.y + 4.0f),
        IM_COL32(255, 255, 255, 255),
        filename.c_str()
    );

    // Draw buffer progress bar at bottom of clip
    const auto* frameBuffer = registry.try_get<FrameBuffer>(clipEntity);
    if (frameBuffer && frameBuffer->ringBuffer) {
        float fillPct = frameBuffer->ringBuffer->getFillPercentage();

        // Progress bar dimensions
        float barHeight = 4.0f;
        float barY = clipMax.y - barHeight - 2.0f;
        float barWidth = clipWidth - 6.0f;

        // Background bar
        ImVec2 barMin(clipMin.x + 3.0f, barY);
        ImVec2 barMax(clipMin.x + 3.0f + barWidth, barY + barHeight);
        drawList->AddRectFilled(barMin, barMax, IM_COL32(30, 30, 30, 200), 2.0f);

        // Fill bar (color based on level)
        ImU32 fillColor;
        if (fillPct > 0.5f) {
            fillColor = IM_COL32(50, 200, 50, 255);  // Green
        } else if (fillPct > 0.2f) {
            fillColor = IM_COL32(200, 200, 50, 255); // Yellow
        } else {
            fillColor = IM_COL32(200, 50, 50, 255);  // Red
        }

        ImVec2 fillMax(barMin.x + barWidth * fillPct, barMax.y);
        if (fillPct > 0.01f) {
            drawList->AddRectFilled(barMin, fillMax, fillColor, 2.0f);
        }
    }

    // Draw keyframe markers if clip has animated properties
    const auto* animProps = registry.try_get<AnimatedProperties>(clipEntity);
    if (animProps && animProps->hasAnyKeyframes()) {
        // Keyframe diamond size
        float diamondSize = 4.0f;
        float keyframeY = clipMin.y + 8.0f;  // Position near top of clip

        // Collect all unique keyframe frames
        std::set<FrameNumber> keyframeFrames;
        for (const auto& track : animProps->tracks) {
            for (const auto& kf : track.keyframes) {
                keyframeFrames.insert(kf.frame);
            }
        }

        // Draw diamond marker for each keyframe
        for (FrameNumber kfFrame : keyframeFrames) {
            // Convert keyframe frame (relative to clip) to pixel position
            float kfSeconds = kfFrame / clip->framerate;
            float kfX = clipMin.x + (kfSeconds * m_pixelsPerSecond);

            // Only draw if within clip bounds
            if (kfX >= clipMin.x && kfX <= clipMax.x) {
                // Draw diamond shape
                ImVec2 points[4] = {
                    ImVec2(kfX, keyframeY - diamondSize),           // Top
                    ImVec2(kfX + diamondSize, keyframeY),           // Right
                    ImVec2(kfX, keyframeY + diamondSize),           // Bottom
                    ImVec2(kfX - diamondSize, keyframeY)            // Left
                };

                // Gold color for keyframes
                drawList->AddConvexPolyFilled(points, 4, IM_COL32(255, 200, 50, 255));
                drawList->AddPolyline(points, 4, IM_COL32(255, 255, 255, 200), ImDrawFlags_Closed, 1.0f);
            }
        }
    }

    // Draw trim handles at clip edges
    // Show handles when clip is selected or when actively trimming this clip
    bool showHandles = isSelected || (m_isTrimmingClip && m_trimClip == clipEntity);

    if (showHandles && clipWidth > TRIM_EDGE_WIDTH * 4) {
        // Handle dimensions
        float handleWidth = TRIM_EDGE_WIDTH - 2.0f;
        float handleHeight = TRACK_HEIGHT - CLIP_PADDING * 4;
        float handleY = clipMin.y + (TRACK_HEIGHT - CLIP_PADDING * 2 - handleHeight) / 2;

        // Left trim handle
        ImVec2 leftHandleMin(clipMin.x + 1.0f, handleY);
        ImVec2 leftHandleMax(clipMin.x + 1.0f + handleWidth, handleY + handleHeight);

        // Highlight if actively trimming this edge
        ImU32 leftHandleColor = (m_isTrimmingClip && m_trimClip == clipEntity && m_trimEdge == ClipEdge::Left)
            ? IM_COL32(255, 200, 100, 255)   // Orange when trimming
            : IM_COL32(200, 200, 200, 180);  // Gray normally

        drawList->AddRectFilled(leftHandleMin, leftHandleMax, leftHandleColor, 2.0f);
        drawList->AddRect(leftHandleMin, leftHandleMax, IM_COL32(255, 255, 255, 200), 2.0f);

        // Right trim handle
        ImVec2 rightHandleMin(clipMax.x - 1.0f - handleWidth, handleY);
        ImVec2 rightHandleMax(clipMax.x - 1.0f, handleY + handleHeight);

        ImU32 rightHandleColor = (m_isTrimmingClip && m_trimClip == clipEntity && m_trimEdge == ClipEdge::Right)
            ? IM_COL32(255, 200, 100, 255)   // Orange when trimming
            : IM_COL32(200, 200, 200, 180);  // Gray normally

        drawList->AddRectFilled(rightHandleMin, rightHandleMax, rightHandleColor, 2.0f);
        drawList->AddRect(rightHandleMin, rightHandleMax, IM_COL32(255, 255, 255, 200), 2.0f);
    }

    // Property tracks are now rendered from renderTracks() to align with header panel
    return TRACK_HEIGHT;
}

float TimelineWidget::renderPropertyTracks(entt::entity clipEntity, int trackIndex, ImVec2 baseWindowPos, float startY) {
    (void)trackIndex;  // Unused parameter
    if (!m_timeline) return 0.0f;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    auto& registry = m_timeline->getRegistry();

    const auto* clip = registry.try_get<Clip>(clipEntity);
    if (!clip) return 0.0f;

    // Get AnimatedProperties for keyframe data
    auto* animProps = registry.try_get<AnimatedProperties>(clipEntity);

    // Calculate clip timing for positioning keyframes
    // Use timeline frame rate since startFrame and duration are in timeline frames
    double timelineFrameRate = m_timeline->getFrameRate();
    float startSeconds = static_cast<float>(clip->startFrame) / static_cast<float>(timelineFrameRate);
    float durationSeconds = static_cast<float>(clip->duration) / static_cast<float>(timelineFrameRate);
    Timecode startTime = static_cast<Timecode>(startSeconds * 1000000.0f);
    float clipX = baseWindowPos.x + timeToPixel(startTime);
    float clipWidth = timeToPixel(static_cast<Timecode>(durationSeconds * 1000000.0f));

    // Define the properties
    AnimatableProperty properties[] = {
        AnimatableProperty::PositionX,
        AnimatableProperty::PositionY,
        AnimatableProperty::ScaleX,
        AnimatableProperty::ScaleY,
        AnimatableProperty::Rotation,
        AnimatableProperty::Opacity
    };

    int numProperties = 6;

    for (int i = 0; i < numProperties; i++) {
        float rowY = startY + i * PROPERTY_ROW_HEIGHT;

        // Draw property row background (scrollable track area)
        ImVec2 rowMin(baseWindowPos.x, rowY);
        ImVec2 rowMax(baseWindowPos.x + 4000.0f, rowY + PROPERTY_ROW_HEIGHT);
        drawList->AddRectFilled(rowMin, rowMax, IM_COL32(35, 40, 50, 255));

        // Draw property track area background under clip (slightly lighter)
        ImVec2 trackMin(clipX, rowY);
        ImVec2 trackMax(clipX + clipWidth, rowY + PROPERTY_ROW_HEIGHT);
        drawList->AddRectFilled(trackMin, trackMax, IM_COL32(50, 55, 65, 255));

        // Draw keyframe shapes on the timeline
        const KeyframeTrack* track = animProps ? animProps->getTrack(properties[i]) : nullptr;
        if (track && track->hasKeyframes()) {
            float size = 5.0f;
            float keyframeY = rowY + PROPERTY_ROW_HEIGHT / 2.0f;
            ImU32 kfColor = IM_COL32(255, 200, 50, 255);  // Gold color for all keyframes
            ImU32 outlineColor = IM_COL32(255, 255, 255, 200);

            for (const auto& kf : track->keyframes) {
                // Convert keyframe frame to pixel position
                float kfSeconds = static_cast<float>(kf.frame) / static_cast<float>(clip->framerate);
                float kfX = clipX + (kfSeconds * m_pixelsPerSecond);

                // Only draw if within clip bounds
                if (kfX >= clipX && kfX <= clipX + clipWidth) {
                    switch (kf.interpolation) {
                        case InterpolationType::Step: {
                            // Square shape for Hold
                            ImVec2 sqMin(kfX - size, keyframeY - size);
                            ImVec2 sqMax(kfX + size, keyframeY + size);
                            drawList->AddRectFilled(sqMin, sqMax, kfColor);
                            drawList->AddRect(sqMin, sqMax, outlineColor, 0.0f, 0, 1.0f);
                            break;
                        }

                        case InterpolationType::EaseIn: {
                            // Left hourglass (curved), right diamond (pointed)
                            // Draw left side: hourglass curves
                            drawList->AddBezierQuadratic(
                                ImVec2(kfX - size, keyframeY - size),
                                ImVec2(kfX, keyframeY),
                                ImVec2(kfX - size, keyframeY + size),
                                kfColor, 3.0f);
                            // Draw right side: diamond point
                            ImVec2 rightDiamond[3] = {
                                ImVec2(kfX, keyframeY - size),
                                ImVec2(kfX + size, keyframeY),
                                ImVec2(kfX, keyframeY + size)
                            };
                            drawList->AddTriangleFilled(rightDiamond[0], rightDiamond[1], rightDiamond[2], kfColor);
                            // Outline
                            drawList->AddLine(ImVec2(kfX, keyframeY - size), ImVec2(kfX + size, keyframeY), outlineColor, 1.0f);
                            drawList->AddLine(ImVec2(kfX + size, keyframeY), ImVec2(kfX, keyframeY + size), outlineColor, 1.0f);
                            break;
                        }

                        case InterpolationType::EaseOut: {
                            // Left diamond (pointed), right hourglass (curved)
                            // Draw left side: diamond point
                            ImVec2 leftDiamond[3] = {
                                ImVec2(kfX, keyframeY - size),
                                ImVec2(kfX - size, keyframeY),
                                ImVec2(kfX, keyframeY + size)
                            };
                            drawList->AddTriangleFilled(leftDiamond[0], leftDiamond[1], leftDiamond[2], kfColor);
                            // Draw right side: hourglass curves
                            drawList->AddBezierQuadratic(
                                ImVec2(kfX + size, keyframeY - size),
                                ImVec2(kfX, keyframeY),
                                ImVec2(kfX + size, keyframeY + size),
                                kfColor, 3.0f);
                            // Outline
                            drawList->AddLine(ImVec2(kfX, keyframeY - size), ImVec2(kfX - size, keyframeY), outlineColor, 1.0f);
                            drawList->AddLine(ImVec2(kfX - size, keyframeY), ImVec2(kfX, keyframeY + size), outlineColor, 1.0f);
                            break;
                        }

                        case InterpolationType::EaseInOut: {
                            // Full hourglass shape (pinched in middle)
                            // Top triangle
                            drawList->AddTriangleFilled(
                                ImVec2(kfX - size, keyframeY - size),
                                ImVec2(kfX + size, keyframeY - size),
                                ImVec2(kfX, keyframeY),
                                kfColor);
                            // Bottom triangle
                            drawList->AddTriangleFilled(
                                ImVec2(kfX - size, keyframeY + size),
                                ImVec2(kfX + size, keyframeY + size),
                                ImVec2(kfX, keyframeY),
                                kfColor);
                            // Outline
                            ImVec2 hourglass[6] = {
                                ImVec2(kfX - size, keyframeY - size),
                                ImVec2(kfX + size, keyframeY - size),
                                ImVec2(kfX, keyframeY),
                                ImVec2(kfX + size, keyframeY + size),
                                ImVec2(kfX - size, keyframeY + size),
                                ImVec2(kfX, keyframeY)
                            };
                            drawList->AddPolyline(hourglass, 6, outlineColor, ImDrawFlags_Closed, 1.0f);
                            break;
                        }

                        case InterpolationType::Linear:
                        default: {
                            // Diamond shape for Linear
                            ImVec2 diamond[4] = {
                                ImVec2(kfX, keyframeY - size),
                                ImVec2(kfX + size, keyframeY),
                                ImVec2(kfX, keyframeY + size),
                                ImVec2(kfX - size, keyframeY)
                            };
                            drawList->AddConvexPolyFilled(diamond, 4, kfColor);
                            drawList->AddPolyline(diamond, 4, outlineColor, ImDrawFlags_Closed, 1.0f);
                            break;
                        }
                    }

                    // Handle right-click for context menu
                    if (ImGui::IsMouseClicked(1)) {
                        ImVec2 mousePos = ImGui::GetMousePos();
                        if (mousePos.x >= kfX - size - 3 && mousePos.x <= kfX + size + 3 &&
                            mousePos.y >= keyframeY - size - 3 && mousePos.y <= keyframeY + size + 3) {
                            m_keyframeEditClip = clipEntity;
                            m_keyframeEditProperty = properties[i];
                            m_keyframeEditFrame = kf.frame;
                            m_showKeyframeContextMenu = true;
                        }
                    }
                }
            }
        }
    }

    return numProperties * PROPERTY_ROW_HEIGHT;
}

void TimelineWidget::renderPlayhead() {
    if (!m_timeline) return;

    // Use foreground draw list to render on top of child windows
    ImDrawList* drawList = ImGui::GetForegroundDrawList();

    // Calculate playhead X position (accounting for scroll)
    Timecode currentTime = m_timeline->getCurrentTime();
    float playheadPixel = timeToPixel(currentTime) - m_syncScrollX;

    // Playhead X in ruler space
    float playheadXRuler = m_rulerScreenPos.x + playheadPixel;

    // Playhead X in tracks space
    float playheadXTracks = m_tracksScreenPos.x + playheadPixel;

    // Draw playhead line spanning from ruler through tracks
    drawList->AddLine(
        ImVec2(playheadXRuler, m_rulerScreenPos.y),
        ImVec2(playheadXTracks, m_tracksScreenPos.y + m_tracksHeight),
        IM_COL32(255, 100, 100, 255),
        2.0f
    );

    // Draw playhead triangle at top of ruler
    ImVec2 trianglePoints[3] = {
        ImVec2(playheadXRuler, m_rulerScreenPos.y),
        ImVec2(playheadXRuler - 6.0f, m_rulerScreenPos.y + 10.0f),
        ImVec2(playheadXRuler + 6.0f, m_rulerScreenPos.y + 10.0f)
    };
    drawList->AddTriangleFilled(trianglePoints[0], trianglePoints[1], trianglePoints[2],
                                IM_COL32(255, 100, 100, 255));

    // Draw snap indicator when actively snapping
    if (m_isSnapping) {
        float snapPixel = timeToPixel(m_snapTargetTime) - m_syncScrollX;
        float snapXRuler = m_rulerScreenPos.x + snapPixel;
        float snapXTracks = m_tracksScreenPos.x + snapPixel;

        // Draw bright cyan snap line (wider than playhead)
        drawList->AddLine(
            ImVec2(snapXRuler, m_rulerScreenPos.y),
            ImVec2(snapXTracks, m_tracksScreenPos.y + m_tracksHeight),
            IM_COL32(0, 255, 255, 200),  // Cyan color
            3.0f
        );

        // Draw small diamonds at top and bottom of snap line
        float diamondSize = 5.0f;
        ImVec2 topDiamond[4] = {
            ImVec2(snapXRuler, m_rulerScreenPos.y - diamondSize),
            ImVec2(snapXRuler - diamondSize, m_rulerScreenPos.y),
            ImVec2(snapXRuler, m_rulerScreenPos.y + diamondSize),
            ImVec2(snapXRuler + diamondSize, m_rulerScreenPos.y)
        };
        drawList->AddQuadFilled(topDiamond[0], topDiamond[1], topDiamond[2], topDiamond[3],
                                IM_COL32(0, 255, 255, 255));
    }
}

void TimelineWidget::renderTrackHeaderPanel(float panelHeight, float verticalScroll) {
    if (!m_timeline) return;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 windowPos = ImGui::GetCursorScreenPos();

    // Draw panel background
    ImVec2 panelMin = windowPos;
    ImVec2 panelMax(windowPos.x + TRACK_HEADER_WIDTH - 4, windowPos.y + panelHeight + 100);
    drawList->AddRectFilled(panelMin, panelMax, IM_COL32(40, 42, 48, 255));

    const auto& tracks = m_timeline->getTracks();
    auto& registry = m_timeline->getRegistry();

    float currentY = windowPos.y;

    for (size_t i = 0; i < tracks.size(); ++i) {
        float trackHeight = renderTrackHeaderRow(tracks[i], static_cast<int>(i), currentY);
        currentY += trackHeight;
    }
}

float TimelineWidget::renderTrackHeaderRow(entt::entity trackEntity, int trackIndex, float rowY) {
    if (!m_timeline) return TRACK_HEIGHT + TRACK_PADDING;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 windowPos = ImGui::GetWindowPos();
    auto& registry = m_timeline->getRegistry();

    const auto* track = registry.try_get<TimelineTrack>(trackEntity);
    if (!track) return TRACK_HEIGHT + TRACK_PADDING;

    uint32_t trackId = static_cast<uint32_t>(trackEntity);
    bool isExpanded = m_expandedTracks.count(trackId) > 0 || m_timeline->isTrackExpanded(trackEntity);

    // Track header background (matches track height)
    float headerX = windowPos.x + 2;
    ImVec2 headerMin(headerX, rowY);
    ImVec2 headerMax(headerX + TRACK_HEADER_WIDTH - 8, rowY + TRACK_HEIGHT);

    // Alternating background colors
    ImU32 bgColor = (trackIndex % 2 == 0)
        ? IM_COL32(50, 52, 58, 255)
        : IM_COL32(45, 47, 53, 255);
    drawList->AddRectFilled(headerMin, headerMax, bgColor);

    // Twirl-down triangle
    float triX = headerX + 12.0f;
    float triY = rowY + TRACK_HEIGHT / 2.0f;
    float triSize = 5.0f;

    ImVec2 triPoints[3];
    if (isExpanded) {
        // Down-pointing triangle (expanded)
        triPoints[0] = ImVec2(triX - triSize, triY - triSize / 2);
        triPoints[1] = ImVec2(triX + triSize, triY - triSize / 2);
        triPoints[2] = ImVec2(triX, triY + triSize / 2);
    } else {
        // Right-pointing triangle (collapsed)
        triPoints[0] = ImVec2(triX - triSize / 2, triY - triSize);
        triPoints[1] = ImVec2(triX + triSize / 2, triY);
        triPoints[2] = ImVec2(triX - triSize / 2, triY + triSize);
    }

    // Only show triangle if track has clips
    bool hasClips = !track->clips.empty();
    if (hasClips) {
        drawList->AddTriangleFilled(triPoints[0], triPoints[1], triPoints[2], IM_COL32(180, 180, 180, 255));
    }

    // Track name
    std::ostringstream trackName;
    trackName << "Track " << (trackIndex + 1);
    drawList->AddText(ImVec2(headerX + 26, rowY + TRACK_HEIGHT / 2 - 7),
                      IM_COL32(220, 220, 220, 255), trackName.str().c_str());

    // Clip count badge
    if (hasClips) {
        std::ostringstream clipCount;
        clipCount << "(" << track->clips.size() << ")";
        drawList->AddText(ImVec2(headerX + TRACK_HEADER_WIDTH - 50, rowY + TRACK_HEIGHT / 2 - 7),
                          IM_COL32(120, 120, 120, 255), clipCount.str().c_str());
    }

    // Handle click on twirl-down triangle
    if (hasClips && ImGui::IsMouseClicked(0)) {
        ImVec2 mousePos = ImGui::GetMousePos();
        ImVec2 triHitMin(triX - triSize - 6, triY - triSize - 6);
        ImVec2 triHitMax(triX + triSize + 6, triY + triSize + 6);

        if (mousePos.x >= triHitMin.x && mousePos.x <= triHitMax.x &&
            mousePos.y >= triHitMin.y && mousePos.y <= triHitMax.y) {
            if (isExpanded) {
                m_expandedTracks.erase(trackId);
            } else {
                m_expandedTracks.insert(trackId);
            }
        }
    }

    float totalHeight = TRACK_HEIGHT + TRACK_PADDING;

    // Render expanded clip rows
    if (isExpanded) {
        float clipY = rowY + TRACK_HEIGHT;
        for (entt::entity clipEntity : track->clips) {
            float clipHeight = renderClipHeaderRow(clipEntity, clipY);
            clipY += clipHeight;
            totalHeight += clipHeight;
        }
    }

    return totalHeight;
}

float TimelineWidget::renderClipHeaderRow(entt::entity clipEntity, float rowY) {
    if (!m_timeline) return HEADER_ROW_HEIGHT;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 windowPos = ImGui::GetWindowPos();
    auto& registry = m_timeline->getRegistry();

    const auto* clip = registry.try_get<Clip>(clipEntity);
    if (!clip) return HEADER_ROW_HEIGHT;

    uint32_t clipId = static_cast<uint32_t>(clipEntity);
    bool isExpanded = m_expandedClips.count(clipId) > 0 || m_timeline->isClipExpanded(clipEntity);
    bool isSelected = (clipEntity == m_selectedClip) || (clipEntity == m_timeline->getSelectedClip());

    // Clip row background (indented)
    float headerX = windowPos.x + 2 + INDENT_WIDTH;
    ImVec2 headerMin(headerX, rowY);
    ImVec2 headerMax(headerX + TRACK_HEADER_WIDTH - 8 - INDENT_WIDTH, rowY + HEADER_ROW_HEIGHT);

    ImU32 bgColor = isSelected
        ? IM_COL32(60, 80, 120, 255)
        : IM_COL32(48, 50, 56, 255);
    drawList->AddRectFilled(headerMin, headerMax, bgColor);

    // Twirl-down triangle
    float triX = headerX + 10.0f;
    float triY = rowY + HEADER_ROW_HEIGHT / 2.0f;
    float triSize = 4.0f;

    ImVec2 triPoints[3];
    if (isExpanded) {
        // Down-pointing triangle (expanded)
        triPoints[0] = ImVec2(triX - triSize, triY - triSize / 2);
        triPoints[1] = ImVec2(triX + triSize, triY - triSize / 2);
        triPoints[2] = ImVec2(triX, triY + triSize / 2);
    } else {
        // Right-pointing triangle (collapsed)
        triPoints[0] = ImVec2(triX - triSize / 2, triY - triSize);
        triPoints[1] = ImVec2(triX + triSize / 2, triY);
        triPoints[2] = ImVec2(triX - triSize / 2, triY + triSize);
    }
    drawList->AddTriangleFilled(triPoints[0], triPoints[1], triPoints[2], IM_COL32(150, 150, 150, 255));

    // Clip name (filename without path)
    size_t lastSlash = clip->filepath.find_last_of("/\\");
    std::string filename = (lastSlash != std::string::npos)
        ? clip->filepath.substr(lastSlash + 1)
        : clip->filepath;

    // Truncate if too long
    if (filename.length() > 16) {
        filename = filename.substr(0, 13) + "...";
    }

    drawList->AddText(ImVec2(headerX + 22, rowY + 4),
                      IM_COL32(200, 200, 200, 255), filename.c_str());

    // Handle click on twirl-down triangle
    if (ImGui::IsMouseClicked(0)) {
        ImVec2 mousePos = ImGui::GetMousePos();
        ImVec2 triHitMin(triX - triSize - 6, triY - triSize - 6);
        ImVec2 triHitMax(triX + triSize + 6, triY + triSize + 6);

        if (mousePos.x >= triHitMin.x && mousePos.x <= triHitMax.x &&
            mousePos.y >= triHitMin.y && mousePos.y <= triHitMax.y) {
            if (isExpanded) {
                m_expandedClips.erase(clipId);
            } else {
                m_expandedClips.insert(clipId);
            }
        }
    }

    // Handle click on clip row to select
    if (ImGui::IsMouseClicked(0)) {
        ImVec2 mousePos = ImGui::GetMousePos();
        if (mousePos.x >= headerMin.x && mousePos.x <= headerMax.x &&
            mousePos.y >= headerMin.y && mousePos.y <= headerMax.y) {
            m_selectedClip = clipEntity;
            m_timeline->setSelectedClip(clipEntity);
            m_timeline->setSelectedScreen(entt::null);  // Deselect screen when selecting clip
        }
    }

    float totalHeight = HEADER_ROW_HEIGHT;

    // Render expanded property rows
    if (isExpanded) {
        float propY = rowY + HEADER_ROW_HEIGHT;
        float propX = headerX + INDENT_WIDTH;
        float propWidth = TRACK_HEADER_WIDTH - 8 - INDENT_WIDTH * 2;

        // Get animation data
        auto* animProps = registry.try_get<AnimatedProperties>(clipEntity);

        // Get current frame relative to clip
        FrameNumber currentFrame = m_timeline->getCurrentFrame();
        FrameNumber localFrame = currentFrame - clip->startFrame;

        // Define properties
        struct PropertyDef {
            AnimatableProperty prop;
            const char* name;
            float defaultValue;
        };

        PropertyDef properties[] = {
            {AnimatableProperty::PositionX, "Pos X", 0.0f},
            {AnimatableProperty::PositionY, "Pos Y", 0.0f},
            {AnimatableProperty::ScaleX, "Scale X", 1.0f},
            {AnimatableProperty::ScaleY, "Scale Y", 1.0f},
            {AnimatableProperty::Rotation, "Rotation", 0.0f},
            {AnimatableProperty::Opacity, "Opacity", 1.0f}
        };

        for (int i = 0; i < 6; ++i) {
            // Property row background
            ImVec2 propMin(propX, propY);
            ImVec2 propMax(propX + propWidth, propY + PROPERTY_ROW_HEIGHT);
            drawList->AddRectFilled(propMin, propMax, IM_COL32(42, 44, 50, 255));

            // Get keyframe track info
            const KeyframeTrack* track = animProps ? animProps->getTrack(properties[i].prop) : nullptr;
            bool hasKeyframes = track && track->hasKeyframes();
            bool atKeyframe = track && track->getKeyframeAt(localFrame) != nullptr;

            // Get current value
            float currentValue = properties[i].defaultValue;
            if (animProps) {
                currentValue = animProps->evaluate(properties[i].prop, localFrame);
            }

            float centerY = propY + PROPERTY_ROW_HEIGHT / 2.0f;

            // === PROPERTY NAME ===
            drawList->AddText(ImVec2(propX + 4, propY + 2),
                              IM_COL32(140, 140, 140, 255), properties[i].name);

            // === KEYFRAME CONTROLS (right side of row) ===
            float controlsX = propX + propWidth - 70;  // Position for controls
            float arrowSize = 3.0f;
            float diamondSize = 4.0f;

            // Left arrow (prev keyframe)
            float leftArrowX = controlsX;
            ImVec2 leftArrow[3] = {
                ImVec2(leftArrowX - arrowSize, centerY),
                ImVec2(leftArrowX + arrowSize, centerY - arrowSize),
                ImVec2(leftArrowX + arrowSize, centerY + arrowSize)
            };
            ImU32 arrowColor = hasKeyframes ? IM_COL32(180, 180, 180, 255) : IM_COL32(80, 80, 80, 255);
            drawList->AddTriangleFilled(leftArrow[0], leftArrow[1], leftArrow[2], arrowColor);

            // Diamond (keyframe indicator)
            float diamondX = controlsX + 14;
            ImVec2 diamond[4] = {
                ImVec2(diamondX, centerY - diamondSize),
                ImVec2(diamondX + diamondSize, centerY),
                ImVec2(diamondX, centerY + diamondSize),
                ImVec2(diamondX - diamondSize, centerY)
            };
            if (atKeyframe) {
                drawList->AddConvexPolyFilled(diamond, 4, IM_COL32(255, 200, 50, 255));
            } else if (hasKeyframes) {
                drawList->AddPolyline(diamond, 4, IM_COL32(180, 180, 180, 255), ImDrawFlags_Closed, 1.0f);
            } else {
                drawList->AddPolyline(diamond, 4, IM_COL32(80, 80, 80, 255), ImDrawFlags_Closed, 1.0f);
            }

            // Right arrow (next keyframe)
            float rightArrowX = controlsX + 28;
            ImVec2 rightArrow[3] = {
                ImVec2(rightArrowX + arrowSize, centerY),
                ImVec2(rightArrowX - arrowSize, centerY - arrowSize),
                ImVec2(rightArrowX - arrowSize, centerY + arrowSize)
            };
            drawList->AddTriangleFilled(rightArrow[0], rightArrow[1], rightArrow[2], arrowColor);

            // === CURRENT VALUE ===
            char valueStr[32];
            if (properties[i].prop == AnimatableProperty::Opacity) {
                snprintf(valueStr, sizeof(valueStr), "%.0f%%", currentValue * 100.0f);
            } else if (properties[i].prop == AnimatableProperty::Rotation) {
                snprintf(valueStr, sizeof(valueStr), "%.1f", currentValue);
            } else {
                snprintf(valueStr, sizeof(valueStr), "%.2f", currentValue);
            }
            drawList->AddText(ImVec2(controlsX + 38, propY + 2),
                              IM_COL32(200, 200, 200, 255), valueStr);

            // === HANDLE CLICKS ===
            if (ImGui::IsMouseClicked(0)) {
                ImVec2 mousePos = ImGui::GetMousePos();

                // Left arrow click (prev keyframe)
                if (hasKeyframes &&
                    mousePos.x >= leftArrowX - arrowSize - 4 && mousePos.x <= leftArrowX + arrowSize + 4 &&
                    mousePos.y >= centerY - arrowSize - 4 && mousePos.y <= centerY + arrowSize + 4) {
                    // Find previous keyframe
                    FrameNumber prevFrame = -1;
                    for (const auto& kf : track->keyframes) {
                        if (kf.frame < localFrame && kf.frame > prevFrame) {
                            prevFrame = kf.frame;
                        }
                    }
                    if (prevFrame >= 0) {
                        m_timeline->seekToFrame(prevFrame + clip->startFrame);
                    }
                }

                // Diamond click (add/remove keyframe)
                if (mousePos.x >= diamondX - diamondSize - 4 && mousePos.x <= diamondX + diamondSize + 4 &&
                    mousePos.y >= centerY - diamondSize - 4 && mousePos.y <= centerY + diamondSize + 4) {
                    if (atKeyframe && animProps) {
                        // Remove keyframe
                        auto* mutableTrack = animProps->getTrack(properties[i].prop);
                        if (mutableTrack) {
                            mutableTrack->removeKeyframe(localFrame);
                        }
                    } else {
                        // Add keyframe at current position
                        auto& props = registry.get_or_emplace<AnimatedProperties>(clipEntity);
                        props.addKeyframe(properties[i].prop, localFrame, currentValue, InterpolationType::Linear);
                    }
                }

                // Right arrow click (next keyframe)
                if (hasKeyframes &&
                    mousePos.x >= rightArrowX - arrowSize - 4 && mousePos.x <= rightArrowX + arrowSize + 4 &&
                    mousePos.y >= centerY - arrowSize - 4 && mousePos.y <= centerY + arrowSize + 4) {
                    // Find next keyframe
                    FrameNumber nextFrame = std::numeric_limits<FrameNumber>::max();
                    for (const auto& kf : track->keyframes) {
                        if (kf.frame > localFrame && kf.frame < nextFrame) {
                            nextFrame = kf.frame;
                        }
                    }
                    if (nextFrame != std::numeric_limits<FrameNumber>::max()) {
                        m_timeline->seekToFrame(nextFrame + clip->startFrame);
                    }
                }
            }

            propY += PROPERTY_ROW_HEIGHT;
            totalHeight += PROPERTY_ROW_HEIGHT;
        }
    }

    return totalHeight;
}

} // namespace entity
