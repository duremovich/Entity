/**
 * TimelineWidget input / interaction code.
 *
 * Split from TimelineWidget.cpp (Phase B #16). All handle*() and findClip*()
 * hit-testing live here; drawing lives in TimelineWidgetRender.cpp; core
 * dispatch + geometry helpers stay in TimelineWidget.cpp.
 */

#include "entity/timeline/TimelineWidget.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include <sstream>
#include <cmath>
#include <limits>

namespace entity {

void TimelineWidget::handleInteraction() {
    if (!m_timeline) return;

    ImVec2 windowPos = ImGui::GetCursorScreenPos();
    ImVec2 windowSize = ImGui::GetContentRegionAvail();
    ImVec2 mousePos = ImGui::GetMousePos();
    ImGuiIO& io = ImGui::GetIO();

    // Check if mouse is over the timeline window
    bool overTimeline = (mousePos.x >= windowPos.x &&
                         mousePos.x <= windowPos.x + windowSize.x &&
                         mousePos.y >= windowPos.y);

    // Alt+scroll steps the discrete zoom ladder. Wheel up = zoom in =
    // smaller frames per major tick = lower index.
    if (overTimeline && io.MouseWheel != 0.0f && io.KeyAlt) {
        setZoomIndex(m_zoomIndex + (io.MouseWheel > 0.0f ? -1 : 1));
    }

    // Check if mouse is over the time ruler
    bool overRuler = (mousePos.x >= windowPos.x &&
                      mousePos.x <= windowPos.x + windowSize.x &&
                      mousePos.y >= windowPos.y &&
                      mousePos.y <= windowPos.y + RULER_HEIGHT);

    // Handle ruler click and drag for seeking/scrubbing
    if (overRuler && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        m_isDraggingRuler = true;
        float relativeX = mousePos.x - windowPos.x;
        Timecode newTime = pixelToTime(relativeX);
        m_timeline->seek(newTime);
    }

    if (m_isDraggingRuler) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Continue dragging - update time as mouse moves
            float relativeX = mousePos.x - windowPos.x;
            Timecode newTime = pixelToTime(relativeX);
            m_timeline->seek(newTime);
        } else {
            // Mouse released - stop dragging
            m_isDraggingRuler = false;
        }
    }

    // Handle clip dragging
    if (m_isDraggingClip) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Calculate new clip position based on mouse
            float relativeX = mousePos.x - windowPos.x - m_dragOffsetX;
            Timecode newStartTime = pixelToTime(relativeX);

            // Clamp to valid range (>= 0)
            if (newStartTime < 0) newStartTime = 0;

            // Update clip's start frame
            auto& registry = m_timeline->getRegistry();
            if (registry.valid(m_selectedClip)) {
                auto* clip = registry.try_get<Clip>(m_selectedClip);
                if (clip) {
                    float newStartSeconds = newStartTime / 1000000.0f;
                    double timelineFrameRate = m_timeline->getFrameRate();
                    clip->startFrame = static_cast<FrameNumber>(newStartSeconds * timelineFrameRate);
                }
            }
        } else {
            // Mouse released - stop dragging
            m_isDraggingClip = false;
        }
    } else if (!m_isDraggingRuler) {
        // Only check for clip selection if not dragging ruler or clip
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            int trackIndex = -1;
            entt::entity clipUnderMouse = findClipAtPosition(mousePos, windowPos, trackIndex);

            if (clipUnderMouse != entt::null) {
                // Start dragging this clip
                m_selectedClip = clipUnderMouse;
                m_isDraggingClip = true;

                // Calculate drag offset (where in the clip the user clicked)
                auto& registry = m_timeline->getRegistry();
                if (registry.valid(clipUnderMouse)) {
                    auto* clip = registry.try_get<Clip>(clipUnderMouse);
                    if (clip) {
                        double timelineFrameRate = m_timeline->getFrameRate();
                        float startSeconds = clip->startFrame / static_cast<float>(timelineFrameRate);
                        Timecode clipStartTime = static_cast<Timecode>(startSeconds * 1000000.0f);
                        float clipX = windowPos.x + timeToPixel(clipStartTime);
                        m_dragOffsetX = mousePos.x - clipX;
                        m_clipDragStartTime = clipStartTime;
                    }
                }
            } else {
                // Clicked on empty space - deselect
                m_selectedClip = entt::null;
            }
        }
    }
}

entt::entity TimelineWidget::findClipAtPosition(ImVec2 mousePos, ImVec2 windowPos, int& outTrackIndex) {
    if (!m_timeline) return entt::null;

    auto& registry = m_timeline->getRegistry();
    const auto& tracks = m_timeline->getTracks();

    // Calculate cumulative Y position to account for expanded clips
    float cumulativeY = 0.0f;

    // Check each track
    for (size_t i = 0; i < tracks.size(); ++i) {
        entt::entity trackEntity = tracks[i];
        const auto* track = registry.try_get<TimelineTrack>(trackEntity);
        if (!track) continue;

        // Calculate track Y position including offset from previous expanded tracks
        float trackY = windowPos.y + cumulativeY;

        // Calculate this track's height (including expanded clips)
        float trackHeight = TRACK_HEIGHT;
        for (entt::entity clipEntity : track->clips) {
            bool isExpanded = m_expandedClips.count(static_cast<uint32_t>(clipEntity)) > 0 ||
                              m_timeline->isClipExpanded(clipEntity);
            bool isSelected = (clipEntity == m_selectedClip) ||
                              (clipEntity == m_timeline->getSelectedClip());
            if (isExpanded && isSelected) {
                trackHeight = TRACK_HEIGHT + 6 * PROPERTY_ROW_HEIGHT;
                break;
            }
        }

        // Check if mouse Y is within this track (including expanded area)
        if (mousePos.y >= trackY && mousePos.y <= trackY + trackHeight) {
            // Check each clip in this track
            for (entt::entity clipEntity : track->clips) {
                const auto* clip = registry.try_get<Clip>(clipEntity);
                if (!clip) continue;

                // Calculate clip bounds (use timeline frame rate, not clip source rate)
                double timelineFrameRate = m_timeline->getFrameRate();
                float startSeconds = clip->startFrame / static_cast<float>(timelineFrameRate);
                float durationSeconds = clip->duration / static_cast<float>(timelineFrameRate);
                Timecode startTime = static_cast<Timecode>(startSeconds * 1000000.0f);
                Timecode endTime = static_cast<Timecode>((startSeconds + durationSeconds) * 1000000.0f);

                float clipX = windowPos.x + timeToPixel(startTime);
                float clipWidth = timeToPixel(endTime - startTime);

                // Calculate this clip's height (including expanded property tracks)
                float clipHeight = TRACK_HEIGHT;
                bool isExpanded = m_expandedClips.count(static_cast<uint32_t>(clipEntity)) > 0 ||
                                  m_timeline->isClipExpanded(clipEntity);
                bool isSelected = (clipEntity == m_selectedClip) ||
                                  (clipEntity == m_timeline->getSelectedClip());
                if (isExpanded && isSelected) {
                    clipHeight = TRACK_HEIGHT + 6 * PROPERTY_ROW_HEIGHT;
                }

                // Check if mouse is within clip bounds (including expanded area)
                if (mousePos.x >= clipX && mousePos.x <= clipX + clipWidth &&
                    mousePos.y >= trackY && mousePos.y <= trackY + clipHeight) {
                    outTrackIndex = static_cast<int>(i);
                    return clipEntity;
                }
            }
        }

        cumulativeY += trackHeight + TRACK_PADDING;
    }

    outTrackIndex = -1;
    return entt::null;
}

ClipEdge TimelineWidget::findClipEdgeAtPosition(ImVec2 mousePos, ImVec2 windowPos, entt::entity& outClip, int& outTrackIndex) {
    if (!m_timeline) {
        outClip = entt::null;
        outTrackIndex = -1;
        return ClipEdge::None;
    }

    auto& registry = m_timeline->getRegistry();
    const auto& tracks = m_timeline->getTracks();

    // Check each track
    for (size_t i = 0; i < tracks.size(); ++i) {
        entt::entity trackEntity = tracks[i];
        const auto* track = registry.try_get<TimelineTrack>(trackEntity);
        if (!track) continue;

        // Calculate track Y bounds
        float trackY = windowPos.y + i * (TRACK_HEIGHT + TRACK_PADDING);

        // Check if mouse Y is within this track
        if (mousePos.y < trackY || mousePos.y > trackY + TRACK_HEIGHT) {
            continue;
        }

        // Check each clip in this track
        for (entt::entity clipEntity : track->clips) {
            const auto* clip = registry.try_get<Clip>(clipEntity);
            if (!clip) continue;

            // Calculate clip bounds (use timeline frame rate, not clip source rate)
            double timelineFrameRate = m_timeline->getFrameRate();
            float startSeconds = clip->startFrame / static_cast<float>(timelineFrameRate);
            float durationSeconds = clip->duration / static_cast<float>(timelineFrameRate);
            Timecode startTime = static_cast<Timecode>(startSeconds * 1000000.0f);
            Timecode endTime = static_cast<Timecode>((startSeconds + durationSeconds) * 1000000.0f);

            float clipX = windowPos.x + timeToPixel(startTime) - m_syncScrollX;
            float clipWidth = timeToPixel(endTime - startTime);
            float clipEndX = clipX + clipWidth;

            // Check if mouse is near the left edge
            if (mousePos.x >= clipX - TRIM_EDGE_WIDTH / 2 && mousePos.x <= clipX + TRIM_EDGE_WIDTH / 2) {
                outClip = clipEntity;
                outTrackIndex = static_cast<int>(i);
                return ClipEdge::Left;
            }

            // Check if mouse is near the right edge
            if (mousePos.x >= clipEndX - TRIM_EDGE_WIDTH / 2 && mousePos.x <= clipEndX + TRIM_EDGE_WIDTH / 2) {
                outClip = clipEntity;
                outTrackIndex = static_cast<int>(i);
                return ClipEdge::Right;
            }
        }
    }

    outClip = entt::null;
    outTrackIndex = -1;
    return ClipEdge::None;
}

void TimelineWidget::handleRulerInteraction() {
    if (!m_timeline) return;

    ImVec2 windowPos = ImGui::GetCursorScreenPos();
    ImVec2 windowSize = ImGui::GetContentRegionAvail();
    ImVec2 mousePos = ImGui::GetMousePos();
    ImGuiIO& io = ImGui::GetIO();

    // Check if mouse is over the ruler window
    bool overRuler = (mousePos.x >= windowPos.x &&
                      mousePos.x <= windowPos.x + windowSize.x &&
                      mousePos.y >= windowPos.y &&
                      mousePos.y <= windowPos.y + RULER_HEIGHT);

    // Alt+scroll over the ruler also steps the discrete zoom ladder.
    // (Same as in handleInteraction(); ruler is its own child window so the
    // event arrives here instead.)
    if (overRuler && io.MouseWheel != 0.0f && io.KeyAlt) {
        setZoomIndex(m_zoomIndex + (io.MouseWheel > 0.0f ? -1 : 1));
    }

    // Shift+click on the ruler starts a range selection. Plain click keeps
    // the existing scrub gesture and clears any prior range so the user can
    // get back to scrubbing without hunting for a clear command.
    if (overRuler && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        float relativeX = mousePos.x - windowPos.x + m_syncScrollX;
        Timecode clickTime = pixelToTime(relativeX);
        if (clickTime < 0) clickTime = 0;

        if (io.KeyShift) {
            m_isCreatingRange = true;
            m_rangeAnchorTime = snapTimeToTickGrid(clickTime);
            m_range.start = m_rangeAnchorTime;
            m_range.end = m_rangeAnchorTime;
            m_range.active = true;
        } else {
            m_isDraggingRuler = true;
            m_timeline->setScrubbing(true);
            if (clickTime != m_lastSeekTime) {
                m_lastSeekTime = clickTime;
                m_timeline->seek(clickTime);
            }
            // Plain ruler click also clears any selection — Disguise/Resolve
            // both behave this way so the user isn't left wondering why the
            // ripple-delete shortcut is greyed out.
            m_range = {};
        }
    }

    if (m_isCreatingRange) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float relativeX = mousePos.x - windowPos.x + m_syncScrollX;
            Timecode mouseTime = pixelToTime(relativeX);
            if (mouseTime < 0) mouseTime = 0;
            mouseTime = snapTimeToTickGrid(mouseTime);
            m_range.start = std::min(m_rangeAnchorTime, mouseTime);
            m_range.end   = std::max(m_rangeAnchorTime, mouseTime);
        } else {
            m_isCreatingRange = false;
            // Zero-length drag = treat as just clearing the range.
            if (m_range.end <= m_range.start) m_range = {};
        }
    } else if (m_isDraggingRuler) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float relativeX = mousePos.x - windowPos.x + m_syncScrollX;
            Timecode newTime = pixelToTime(relativeX);
            if (newTime != m_lastSeekTime) {
                m_lastSeekTime = newTime;
                m_timeline->seek(newTime);
            }
        } else {
            m_isDraggingRuler = false;
            m_timeline->setScrubbing(false);
            m_lastSeekTime = -1;
        }
    }

    // Right-click handling on the ruler — section ops. Hit-tests existing
    // section bands first (delete/jump-to-start menu), then falls back to
    // "create section from active range" when the user has a selection.
    if (overRuler && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        float relativeX = mousePos.x - windowPos.x + m_syncScrollX;
        Timecode clickTime = pixelToTime(relativeX);

        m_rightClickedSection = -1;
        const auto& sections = m_timeline->getSections();
        for (size_t i = 0; i < sections.size(); ++i) {
            if (clickTime >= sections[i].start && clickTime <= sections[i].end) {
                m_rightClickedSection = static_cast<int>(i);
                break;
            }
        }

        if (m_rightClickedSection >= 0) {
            ImGui::OpenPopup("SectionContextMenu");
        } else if (m_range.active && m_range.end > m_range.start) {
            // No section under cursor but a range is active — offer to make one.
            m_rangeContextMenuRequested = true;
            ImGui::OpenPopup("RangeContextMenu");
        }
    }

    // "Create Section from Selection" popup.
    if (ImGui::BeginPopup("RangeContextMenu")) {
        if (m_range.active && m_range.end > m_range.start) {
            if (ImGui::MenuItem("Create Section from Selection")) {
                Timeline::Section sec;
                sec.start = m_range.start;
                sec.end = m_range.end;
                sec.name = "Section " + std::to_string(m_timeline->getSections().size() + 1);
                m_timeline->addSection(std::move(sec));
            }
            ImGui::Separator();
            FrameNumber inF = m_timeline->timeToFrame(m_range.start);
            FrameNumber outF = m_timeline->timeToFrame(m_range.end);
            ImGui::TextDisabled("Range: F%lld - F%lld (%lldf)",
                static_cast<long long>(inF),
                static_cast<long long>(outF),
                static_cast<long long>(outF - inF));
        }
        ImGui::EndPopup();
    }

    // Section band right-click: delete + jump-to-start.
    if (ImGui::BeginPopup("SectionContextMenu")) {
        const auto& sections = m_timeline->getSections();
        if (m_rightClickedSection >= 0 && m_rightClickedSection < static_cast<int>(sections.size())) {
            const auto& sec = sections[m_rightClickedSection];
            ImGui::TextDisabled("%s", sec.name.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Jump to Start")) {
                m_timeline->seek(sec.start);
            }
            if (ImGui::MenuItem("Delete Section")) {
                m_timeline->removeSection(static_cast<size_t>(m_rightClickedSection));
                m_rightClickedSection = -1;
            }
        }
        ImGui::EndPopup();
    }
}

void TimelineWidget::handleTracksInteraction() {
    if (!m_timeline) return;

    // Use cached tracks position (saved in render() before cursor is moved by widgets)
    ImVec2 windowPos = m_tracksScreenPos;
    ImVec2 mousePos = ImGui::GetMousePos();

    // Only handle new interactions if this child window is hovered
    bool isWindowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    auto& registry = m_timeline->getRegistry();

    // Handle clip trimming (priority over dragging)
    if (m_isTrimmingClip) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Calculate mouse position in timeline time
            float relativeX = mousePos.x - windowPos.x + m_syncScrollX;
            Timecode mouseTime = pixelToTime(relativeX);
            if (mouseTime < 0) mouseTime = 0;

            // Get the clip being trimmed
            if (registry.valid(m_trimClip)) {
                auto* clip = registry.try_get<Clip>(m_trimClip);
                if (clip) {
                    double timelineFrameRate = m_timeline->getFrameRate();
                    FrameNumber mouseFrame = static_cast<FrameNumber>(mouseTime / 1000000.0f * timelineFrameRate);

                    // Find the track this clip is on for collision detection
                    int trimTrackIndex = -1;
                    const auto& tracks = m_timeline->getTracks();
                    for (size_t i = 0; i < tracks.size(); ++i) {
                        const auto* track = registry.try_get<TimelineTrack>(tracks[i]);
                        if (track) {
                            for (entt::entity clipEnt : track->clips) {
                                if (clipEnt == m_trimClip) {
                                    trimTrackIndex = static_cast<int>(i);
                                    break;
                                }
                            }
                        }
                        if (trimTrackIndex >= 0) break;
                    }

                    if (m_trimEdge == ClipEdge::Left) {
                        // Trim left edge - adjust start and duration
                        // Don't allow trimming past original end
                        FrameNumber originalEnd = m_trimOriginalStart + m_trimOriginalDuration;
                        if (mouseFrame < originalEnd - 1) {  // Keep at least 1 frame
                            // Don't allow trimming before 0
                            if (mouseFrame < 0) mouseFrame = 0;

                            // Check for collision with previous clip
                            FrameNumber minStartFrame = 0;
                            if (trimTrackIndex >= 0 && trimTrackIndex < static_cast<int>(tracks.size())) {
                                const auto* track = registry.try_get<TimelineTrack>(tracks[trimTrackIndex]);
                                if (track) {
                                    for (entt::entity otherClip : track->clips) {
                                        if (otherClip == m_trimClip) continue;
                                        const auto* other = registry.try_get<Clip>(otherClip);
                                        if (other) {
                                            FrameNumber otherEnd = other->startFrame + other->duration;
                                            // If other clip ends before our original start, it could limit us
                                            if (otherEnd <= m_trimOriginalStart && otherEnd > minStartFrame) {
                                                minStartFrame = otherEnd;
                                            }
                                        }
                                    }
                                }
                            }

                            // Clamp to minimum (collision boundary)
                            if (mouseFrame < minStartFrame) {
                                mouseFrame = minStartFrame;
                            }

                            FrameNumber framesDelta = mouseFrame - m_trimOriginalStart;
                            clip->startFrame = mouseFrame;
                            clip->duration = m_trimOriginalDuration - framesDelta;

                            // Adjust media start to keep sync
                            clip->mediaStartFrame = m_trimOriginalMediaStart + framesDelta;
                        }
                    } else if (m_trimEdge == ClipEdge::Right) {
                        // Trim right edge - adjust duration only
                        FrameNumber newDuration = mouseFrame - clip->startFrame;
                        if (newDuration >= 1) {  // Keep at least 1 frame
                            // Check for collision with next clip
                            FrameNumber maxEndFrame = std::numeric_limits<FrameNumber>::max();
                            if (trimTrackIndex >= 0 && trimTrackIndex < static_cast<int>(tracks.size())) {
                                const auto* track = registry.try_get<TimelineTrack>(tracks[trimTrackIndex]);
                                if (track) {
                                    for (entt::entity otherClip : track->clips) {
                                        if (otherClip == m_trimClip) continue;
                                        const auto* other = registry.try_get<Clip>(otherClip);
                                        if (other) {
                                            // If other clip starts after our start, it could limit us
                                            if (other->startFrame > clip->startFrame && other->startFrame < maxEndFrame) {
                                                maxEndFrame = other->startFrame;
                                            }
                                        }
                                    }
                                }
                            }

                            // Clamp duration to not exceed collision boundary
                            FrameNumber maxDuration = maxEndFrame - clip->startFrame;
                            if (newDuration > maxDuration) {
                                newDuration = maxDuration;
                            }

                            clip->duration = newDuration;
                        }
                    }
                }
            }
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        } else {
            // Mouse released - stop trimming
            m_isTrimmingClip = false;
            m_trimEdge = ClipEdge::None;
            m_trimClip = entt::null;
            m_timeline->setScrubbing(false);  // Exit scrubbing mode - triggers final seek
        }
        return;  // Don't process other interactions while trimming
    }

    // Handle clip dragging (continue even if not hovered, to allow drag outside window)
    if (m_isDraggingClip) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Calculate desired new clip position based on mouse
            float relativeX = mousePos.x - windowPos.x + m_syncScrollX - m_dragOffsetX;
            Timecode desiredStartTime = pixelToTime(relativeX);

            // Clamp to valid range (>= 0)
            if (desiredStartTime < 0) desiredStartTime = 0;

            // Snap to playhead and other clips if enabled
            m_isSnapping = false;  // Reset snap state
            if (m_snappingEnabled && registry.valid(m_selectedClip)) {
                auto* clip = registry.try_get<Clip>(m_selectedClip);
                if (clip) {
                    Timecode playheadTime = m_timeline->getCurrentTime();
                    // Use timeline frame rate since duration is in timeline frames
                    double timelineFrameRate = m_timeline->getFrameRate();
                    float durationSeconds = clip->duration / static_cast<float>(timelineFrameRate);
                    Timecode clipDuration = static_cast<Timecode>(durationSeconds * 1000000.0f);
                    Timecode desiredEndTime = desiredStartTime + clipDuration;

                    // Convert snap threshold from pixels to time
                    Timecode snapThresholdTime = pixelToTime(SNAP_THRESHOLD_PIXELS);

                    // Track best snap candidate
                    Timecode bestSnapTime = 0;
                    Timecode bestSnapDistance = snapThresholdTime + 1;  // Start with invalid distance
                    bool snapToStart = true;  // Whether to snap clip start (true) or end (false)

                    // Check snap to playhead - start edge
                    Timecode distToPlayheadStart = std::abs(desiredStartTime - playheadTime);
                    if (distToPlayheadStart < bestSnapDistance) {
                        bestSnapDistance = distToPlayheadStart;
                        bestSnapTime = playheadTime;
                        snapToStart = true;
                    }

                    // Check snap to playhead - end edge
                    Timecode distToPlayheadEnd = std::abs(desiredEndTime - playheadTime);
                    if (distToPlayheadEnd < bestSnapDistance) {
                        bestSnapDistance = distToPlayheadEnd;
                        bestSnapTime = playheadTime;
                        snapToStart = false;
                    }

                    // Check snap to other clips on the same track
                    if (m_selectedClipTrackIndex >= 0) {
                        const auto& tracks = m_timeline->getTracks();
                        if (m_selectedClipTrackIndex < static_cast<int>(tracks.size())) {
                            const auto* track = registry.try_get<TimelineTrack>(tracks[m_selectedClipTrackIndex]);
                            if (track) {
                                for (entt::entity otherClipEntity : track->clips) {
                                    if (otherClipEntity == m_selectedClip) continue;

                                    const auto* otherClip = registry.try_get<Clip>(otherClipEntity);
                                    if (!otherClip) continue;

                                    float otherStartSec = otherClip->startFrame / static_cast<float>(otherClip->framerate);
                                    float otherDurSec = otherClip->duration / static_cast<float>(otherClip->framerate);
                                    Timecode otherStartTime = static_cast<Timecode>(otherStartSec * 1000000.0f);
                                    Timecode otherEndTime = static_cast<Timecode>((otherStartSec + otherDurSec) * 1000000.0f);

                                    // Our start to other's start
                                    Timecode dist1 = std::abs(desiredStartTime - otherStartTime);
                                    if (dist1 < bestSnapDistance) {
                                        bestSnapDistance = dist1;
                                        bestSnapTime = otherStartTime;
                                        snapToStart = true;
                                    }

                                    // Our start to other's end
                                    Timecode dist2 = std::abs(desiredStartTime - otherEndTime);
                                    if (dist2 < bestSnapDistance) {
                                        bestSnapDistance = dist2;
                                        bestSnapTime = otherEndTime;
                                        snapToStart = true;
                                    }

                                    // Our end to other's start
                                    Timecode dist3 = std::abs(desiredEndTime - otherStartTime);
                                    if (dist3 < bestSnapDistance) {
                                        bestSnapDistance = dist3;
                                        bestSnapTime = otherStartTime;
                                        snapToStart = false;
                                    }

                                    // Our end to other's end
                                    Timecode dist4 = std::abs(desiredEndTime - otherEndTime);
                                    if (dist4 < bestSnapDistance) {
                                        bestSnapDistance = dist4;
                                        bestSnapTime = otherEndTime;
                                        snapToStart = false;
                                    }
                                }
                            }
                        }
                    }

                    // Apply best snap if found
                    if (bestSnapDistance < snapThresholdTime) {
                        if (snapToStart) {
                            desiredStartTime = bestSnapTime;
                        } else {
                            desiredStartTime = bestSnapTime - clipDuration;
                        }
                        if (desiredStartTime < 0) desiredStartTime = 0;
                        m_isSnapping = true;
                        m_snapTargetTime = bestSnapTime;
                    }
                }
            }

            // Check for collisions and snap to valid position
            Timecode newStartTime = checkClipCollision(m_selectedClip, desiredStartTime, m_selectedClipTrackIndex);

            // Update clip's start frame
            if (registry.valid(m_selectedClip)) {
                auto* clip = registry.try_get<Clip>(m_selectedClip);
                if (clip) {
                    float newStartSeconds = newStartTime / 1000000.0f;
                    double timelineFrameRate = m_timeline->getFrameRate();
                    clip->startFrame = static_cast<FrameNumber>(newStartSeconds * timelineFrameRate);
                }
            }
        } else {
            // Mouse released - stop dragging
            // Check if we need to move the clip to a different track
            int finalTrackIndex = findTrackAtY(mousePos.y, windowPos.y);
            if (finalTrackIndex >= 0 && finalTrackIndex != m_selectedClipTrackIndex) {
                // Move clip to new track
                m_timeline->moveClipToTrack(m_selectedClip, finalTrackIndex);
                m_selectedClipTrackIndex = finalTrackIndex;
            }

            m_isDraggingClip = false;
            m_selectedClipTrackIndex = -1;
            m_isSnapping = false;  // Clear snap indicator
            m_timeline->setScrubbing(false);  // Exit scrubbing mode - triggers final seek
        }
    } else if (!m_isDraggingRuler && isWindowHovered) {
        // Check for clip edge hover (for trimming cursor)
        entt::entity edgeClip = entt::null;
        int edgeTrackIndex = -1;
        ClipEdge edge = findClipEdgeAtPosition(mousePos, windowPos, edgeClip, edgeTrackIndex);

        if (edge != ClipEdge::None) {
            // Show resize cursor when hovering over clip edge
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

            // Start trimming on click
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                m_isTrimmingClip = true;
                m_trimEdge = edge;
                m_trimClip = edgeClip;
                m_selectedClip = edgeClip;
                m_timeline->setSelectedClip(edgeClip);
                m_timeline->setSelectedScreen(entt::null);  // Deselect screen when selecting clip
                m_timeline->setScrubbing(true);  // Enter scrubbing mode - prevents decoder seeks

                // Store original clip state for trim calculations
                auto* clip = registry.try_get<Clip>(edgeClip);
                if (clip) {
                    m_trimOriginalStart = clip->startFrame;
                    m_trimOriginalDuration = clip->duration;
                    m_trimOriginalMediaStart = clip->mediaStartFrame;
                }
                return;
            }
        }

        // Only check for clip selection if not hovering edge
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            int trackIndex = -1;
            entt::entity clipUnderMouse = findClipAtPosition(mousePos, windowPos, trackIndex);

            if (clipUnderMouse != entt::null) {
                // Start dragging this clip
                m_selectedClip = clipUnderMouse;
                m_isDraggingClip = true;
                m_selectedClipTrackIndex = trackIndex;
                m_timeline->setScrubbing(true);  // Enter scrubbing mode - prevents decoder seeks

                // Sync selection to Timeline for PropertyWindow
                m_timeline->setSelectedClip(clipUnderMouse);
                m_timeline->setSelectedScreen(entt::null);  // Deselect screen when selecting clip

                // Calculate drag offset (where in the clip the user clicked)
                if (registry.valid(clipUnderMouse)) {
                    auto* clip = registry.try_get<Clip>(clipUnderMouse);
                    if (clip) {
                        double timelineFrameRate = m_timeline->getFrameRate();
                        float startSeconds = clip->startFrame / static_cast<float>(timelineFrameRate);
                        Timecode clipStartTime = static_cast<Timecode>(startSeconds * 1000000.0f);
                        float clipX = windowPos.x + timeToPixel(clipStartTime) - m_syncScrollX;
                        m_dragOffsetX = mousePos.x - clipX;
                        m_clipDragStartTime = clipStartTime;
                    }
                }
            } else {
                // Clicked on empty space within timeline - deselect
                m_selectedClip = entt::null;
                m_selectedClipTrackIndex = -1;

                // Sync deselection to Timeline
                m_timeline->setSelectedClip(entt::null);
            }
        }

        // Handle right-click for context menus (only if window is hovered)
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            int trackIndex = -1;
            entt::entity clipUnderMouse = findClipAtPosition(mousePos, windowPos, trackIndex);

            if (clipUnderMouse != entt::null) {
                // Right-clicked on a clip
                m_rightClickedClip = clipUnderMouse;
                m_rightClickedTrackIndex = trackIndex;
                m_showClipContextMenu = true;
                m_showTrackContextMenu = false;
                ImGui::OpenPopup("ClipContextMenu");
            } else {
                // Check if right-clicked on a track (empty area)
                int trackAtY = findTrackAtY(mousePos.y, windowPos.y);
                if (trackAtY >= 0) {
                    m_rightClickedTrackIndex = trackAtY;
                    m_rightClickedClip = entt::null;
                    m_showTrackContextMenu = true;
                    m_showClipContextMenu = false;
                    ImGui::OpenPopup("TrackContextMenu");
                }
            }
        }
    }
}

void TimelineWidget::handleContextMenus() {
    if (!m_timeline) return;

    // Clip context menu
    if (ImGui::BeginPopup("ClipContextMenu")) {
        if (m_rightClickedClip != entt::null) {
            if (ImGui::MenuItem("Delete Clip")) {
                // Clear selection if we're deleting the selected clip
                if (m_selectedClip == m_rightClickedClip) {
                    m_selectedClip = entt::null;
                    m_selectedClipTrackIndex = -1;
                }
                m_timeline->deleteClip(m_rightClickedClip);
                m_rightClickedClip = entt::null;
            }
            ImGui::Separator();

            // Get clip info for display
            auto& registry = m_timeline->getRegistry();
            const auto* clip = registry.try_get<Clip>(m_rightClickedClip);
            if (clip) {
                ImGui::TextDisabled("Duration: %d frames", clip->duration);
                ImGui::TextDisabled("Start: frame %d", clip->startFrame);
            }
        }
        ImGui::EndPopup();
    }

    // Track context menu
    if (ImGui::BeginPopup("TrackContextMenu")) {
        if (m_rightClickedTrackIndex >= 0) {
            std::ostringstream label;
            label << "Delete Track " << (m_rightClickedTrackIndex + 1);
            if (ImGui::MenuItem(label.str().c_str())) {
                const auto& tracks = m_timeline->getTracks();
                if (m_rightClickedTrackIndex < static_cast<int>(tracks.size())) {
                    entt::entity trackToDelete = tracks[m_rightClickedTrackIndex];
                    m_timeline->deleteTrack(trackToDelete);
                }
                m_rightClickedTrackIndex = -1;
            }

            ImGui::Separator();

            // Option to add a new track
            if (ImGui::MenuItem("Add Track Above")) {
                // For now, just add at the end (track ordering would need more work)
                std::ostringstream trackName;
                trackName << "Video Track " << (m_timeline->getTrackCount() + 1);
                m_timeline->createTrack(trackName.str());
            }

            ImGui::Separator();
            ImGui::TextDisabled("Track %d", m_rightClickedTrackIndex + 1);
        }
        ImGui::EndPopup();
    }

    // Keyframe context menu (for easing options)
    if (m_showKeyframeContextMenu) {
        ImGui::OpenPopup("KeyframeContextMenu");
        m_showKeyframeContextMenu = false;
    }

    if (ImGui::BeginPopup("KeyframeContextMenu")) {
        if (m_keyframeEditClip != entt::null) {
            auto& registry = m_timeline->getRegistry();
            auto* animProps = registry.try_get<AnimatedProperties>(m_keyframeEditClip);

            if (animProps) {
                auto* track = animProps->getTrack(m_keyframeEditProperty);
                if (track) {
                    Keyframe* kf = track->getKeyframeAt(m_keyframeEditFrame);
                    if (kf) {
                        ImGui::TextDisabled("Interpolation:");

                        bool isLinear = (kf->interpolation == InterpolationType::Linear);
                        bool isStep = (kf->interpolation == InterpolationType::Step);
                        bool isEaseIn = (kf->interpolation == InterpolationType::EaseIn);
                        bool isEaseOut = (kf->interpolation == InterpolationType::EaseOut);
                        bool isEaseInOut = (kf->interpolation == InterpolationType::EaseInOut);

                        if (ImGui::MenuItem("Linear (Diamond)", nullptr, isLinear)) {
                            kf->interpolation = InterpolationType::Linear;
                        }
                        if (ImGui::MenuItem("Hold (Square)", nullptr, isStep)) {
                            kf->interpolation = InterpolationType::Step;
                        }
                        if (ImGui::MenuItem("Ease In", nullptr, isEaseIn)) {
                            kf->interpolation = InterpolationType::EaseIn;
                        }
                        if (ImGui::MenuItem("Ease Out", nullptr, isEaseOut)) {
                            kf->interpolation = InterpolationType::EaseOut;
                        }
                        if (ImGui::MenuItem("Ease In/Out (Hourglass)", nullptr, isEaseInOut)) {
                            kf->interpolation = InterpolationType::EaseInOut;
                        }

                        ImGui::Separator();

                        if (ImGui::MenuItem("Delete Keyframe")) {
                            track->removeKeyframe(m_keyframeEditFrame);
                        }
                    }
                }
            }
        }
        ImGui::EndPopup();
    }
}

} // namespace entity
