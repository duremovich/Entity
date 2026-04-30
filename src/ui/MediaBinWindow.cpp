#include "entity/ui/MediaBinWindow.hpp"
#include "entity/core/Engine.hpp"
#include "entity/color/OcioManager.hpp"
#include "entity/media/Decoder.hpp"
#include "entity/media/TranscodeManager.hpp"
#include "entity/components/Clip.hpp"
#include "entity/project/MediaVersioning.hpp"
#include <imgui.h>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <unordered_map>
#include <tuple>
#include <vector>

namespace entity {

MediaBinWindow::MediaBinWindow(Engine* engine)
    : m_engine(engine) {
}

namespace {

// Classify an FFmpeg codec short name into the playback tier we use to
// color-code the Codec column (#27). Unknowns fall through to OK so we
// don't paint legitimate codecs red just because we forgot to list them.
MediaBinWindow::CodecTier classifyCodec(std::string_view ffmpegName) {
    if (ffmpegName.empty()) return MediaBinWindow::CodecTier::Unknown;
    // HAP family — designed for realtime media servers.
    if (ffmpegName == "hap")    return MediaBinWindow::CodecTier::Good;
    // Intra-frame / image sequences — heavy but predictable.
    if (ffmpegName == "prores") return MediaBinWindow::CodecTier::OK;
    if (ffmpegName == "dnxhd")  return MediaBinWindow::CodecTier::OK;
    if (ffmpegName == "cfhd")   return MediaBinWindow::CodecTier::OK;  // CineForm
    if (ffmpegName == "png")    return MediaBinWindow::CodecTier::OK;
    if (ffmpegName == "dpx")    return MediaBinWindow::CodecTier::OK;
    if (ffmpegName == "tiff")   return MediaBinWindow::CodecTier::OK;
    // Interframe / GOP codecs — seek-hostile, slow random-access.
    if (ffmpegName == "h264")   return MediaBinWindow::CodecTier::Bad;
    if (ffmpegName == "hevc")   return MediaBinWindow::CodecTier::Bad;
    if (ffmpegName == "vp9")    return MediaBinWindow::CodecTier::Bad;
    if (ffmpegName == "av1")    return MediaBinWindow::CodecTier::Bad;
    if (ffmpegName == "mpeg4")  return MediaBinWindow::CodecTier::Bad;
    return MediaBinWindow::CodecTier::OK;  // unknowns default to "playable but unverified"
}

// Pretty-print an FFmpeg codec name for the user-facing cell.
std::string prettyCodec(std::string_view ffmpegName) {
    if (ffmpegName == "hap")    return "HAP";
    if (ffmpegName == "prores") return "ProRes";
    if (ffmpegName == "dnxhd")  return "DNxHD";
    if (ffmpegName == "cfhd")   return "CineForm";
    if (ffmpegName == "h264")   return "H.264";
    if (ffmpegName == "hevc")   return "H.265";
    if (ffmpegName == "vp9")    return "VP9";
    if (ffmpegName == "av1")    return "AV1";
    if (ffmpegName == "mpeg4")  return "MPEG-4";
    if (ffmpegName == "png")    return "PNG sequence";
    if (ffmpegName == "dpx")    return "DPX sequence";
    if (ffmpegName == "tiff")   return "TIFF sequence";
    if (ffmpegName.empty())     return "(unknown)";
    return std::string(ffmpegName);
}

ImVec4 colorForTier(MediaBinWindow::CodecTier tier) {
    switch (tier) {
        case MediaBinWindow::CodecTier::Good: return ImVec4(0.50f, 0.85f, 0.50f, 1.0f);
        case MediaBinWindow::CodecTier::OK:   return ImVec4(0.90f, 0.78f, 0.45f, 1.0f);
        case MediaBinWindow::CodecTier::Bad:  return ImVec4(0.95f, 0.45f, 0.45f, 1.0f);
        default:                              return ImVec4(0.65f, 0.65f, 0.65f, 1.0f);
    }
}

// Group of MediaLibraryEntry pointers sharing a logical name (#27). The
// "primary" is the latest-version member; tooltip shows all members.
struct VersionGroup {
    std::string groupKey;                                          // e.g. "act1/intro"
    std::string logicalPath;                                       // e.g. "act1/intro.mov"
    std::vector<const ProjectManager::MediaLibraryEntry*> members; // sorted ascending by version tag
    const ProjectManager::MediaLibraryEntry* primary{nullptr};     // latest member (== members.back())
    std::string latestTag;                                         // empty if group has only unversioned
};

// Build groups from the flat library. Order of groups in the output matches
// the order their first entry appeared in the library (stable per-frame
// across re-renders so rows don't jitter).
std::vector<VersionGroup> buildGroups(
    const std::vector<ProjectManager::MediaLibraryEntry>& entries) {
    std::vector<VersionGroup> out;
    std::unordered_map<std::string, std::size_t> indexByKey;
    for (const auto& e : entries) {
        std::string key = groupKeyOf(e.originalPath);
        // Linked + Managed never cross-group: differentiate the key by
        // pathKind tag.
        if (e.pathKind == ProjectManager::PathKind::Linked) {
            key.push_back('\x01');
            key.append("linked");
        } else {
            key.push_back('\x01');
            key.append("managed");
        }

        auto [it, inserted] = indexByKey.try_emplace(key, out.size());
        if (inserted) {
            VersionGroup g;
            g.groupKey    = groupKeyOf(e.originalPath);
            g.logicalPath = toLogicalPath(e.originalPath);
            g.members.push_back(&e);
            out.push_back(std::move(g));
        } else {
            out[it->second].members.push_back(&e);
        }
    }
    // Sort each group's members by version tag and pick the highest as
    // primary. Pure stable_sort keeps deterministic order for ties.
    for (auto& g : out) {
        std::stable_sort(g.members.begin(), g.members.end(),
            [](const auto* a, const auto* b) {
                const std::string aTag = parseVersion(
                    std::filesystem::path(a->originalPath).stem().string()).tag;
                const std::string bTag = parseVersion(
                    std::filesystem::path(b->originalPath).stem().string()).tag;
                return compareVersionTags(aTag, bTag) < 0;
            });
        g.primary = g.members.back();
        g.latestTag = parseVersion(
            std::filesystem::path(g.primary->originalPath).stem().string()).tag;
    }
    return out;
}

// Rendered in the Status column per entry. Returns true if the entry is
// in a state where dragging to the timeline is legal.
struct StatusDisplay {
    bool dragAllowed{true};
    TranscodeState transState{TranscodeState::Done};
    bool hasActiveWorker{false};
    float progress01{0.0f};
    int64_t framesDone{0};
    int64_t framesTotal{0};
    Result failureResult{Result::Success};  // only meaningful when state == Failed
    bool hasTranscode{false};
    bool isSourceAlreadyHap{false};
};

StatusDisplay computeStatus(const ProjectManager::MediaLibraryEntry& entry,
                             const TranscodeManager* tmgr,
                             bool isSourceAlreadyHap) {
    StatusDisplay d;
    d.hasTranscode = !entry.transcodedPath.empty();
    d.isSourceAlreadyHap = isSourceAlreadyHap;

    if (tmgr) {
        if (auto s = tmgr->statusOf(entry.originalPath)) {
            d.hasActiveWorker = true;
            d.transState    = s->state;
            d.progress01    = s->progress01;
            d.framesDone    = s->framesDone;
            d.framesTotal   = s->framesTotal;
            d.failureResult = s->result;
        }
    }

    // Drag is allowed for anything that isn't actively being transcoded
    // (the worker may hold the source open exclusively). Non-HAP files
    // with no transcode play via the slow path — acceptable now that
    // pro-sync workflows (#27) put files in the bin without ever going
    // through Import / auto-transcode.
    const bool running = d.hasActiveWorker &&
                         (d.transState == TranscodeState::Queued ||
                          d.transState == TranscodeState::Running);
    d.dragAllowed = !running;
    return d;
}

// (renderStatusCell removed in #27 — Codec column now renders its own
// cell inline so it can mix codec name + transcode lifecycle states.)

} // namespace

void MediaBinWindow::renderPendingImportModal() {
    const Engine::PendingImport* pending = m_engine->pendingImport();

    // Open the popup exactly once per new pending import. Popup state is
    // keyed by string ID — if it's already open ImGui ignores repeat opens.
    if (pending) {
        if (pending->filepath != m_modalLastFilepath) {
            m_modalLastFilepath = pending->filepath;
            m_modalDontAskAgain = false;   // fresh import, reset checkbox
            ImGui::OpenPopup("Transcode to HAP?");
        }
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Transcode to HAP?", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        // Popup survives a single frame where pendingImport() returns null
        // (after resolve, before the next render). Use the stashed path for
        // display + close if the engine's pending state has truly cleared.
        const std::string& path = pending ? pending->filepath : m_modalLastFilepath;
        const size_t lastSlash = path.find_last_of("/\\");
        const std::string stem = (lastSlash != std::string::npos)
            ? path.substr(lastSlash + 1) : path;

        ImGui::Text("\"%s\" isn't HAP.", stem.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped(
            "HAP is a GPU-friendly codec — transcoding this file will let you "
            "play it smoothly on a multi-layer timeline. Your original is "
            "preserved: managed imports archive it alongside the new HAP "
            "file (in <subfolder>/.archive/); linked imports leave the "
            "source where it is and cache the HAP in <project>/.cache/hap/.");
        ImGui::Spacing();
        ImGui::Checkbox("Don't ask again for this project", &m_modalDontAskAgain);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Saves your choice as the project default. You can change it\n"
                "later via the \"Non-HAP imports\" dropdown above.");
        }
        ImGui::Spacing();

        const float btnWidth = 140.0f;
        if (ImGui::Button("Transcode to HAP", ImVec2(btnWidth, 0))) {
            m_engine->resolvePendingImport(/*transcode*/ true, m_modalDontAskAgain);
            m_modalLastFilepath.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Skip (use source)", ImVec2(btnWidth, 0))) {
            m_engine->resolvePendingImport(/*transcode*/ false, m_modalDontAskAgain);
            m_modalLastFilepath.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    } else if (pending && m_modalLastFilepath == pending->filepath) {
        // Popup got dismissed by some other means (e.g. ESC — though we
        // didn't set ImGuiWindowFlags_Popup to allow it). Default = Skip
        // without don't-ask-again, same as the button.
        m_engine->resolvePendingImport(false, false);
        m_modalLastFilepath.clear();
    }
}

void MediaBinWindow::renderImportTargetToolbar() {
    // ADR-0009: when the user is in an interactive session (MediaBin is
    // visible), the import default is Copy into content/unsorted/. We
    // seed this once on first render so script-driven flows (which
    // never instantiate MediaBin) keep the Link default, and so an
    // operator who explicitly changed the toggle in a previous session
    // doesn't get reverted. After the seed, the toolbar is the source
    // of truth; Engine just stores whatever it last received.
    if (!m_importDefaultsSeeded) {
        m_engine->setImportMode(Engine::ImportMode::Copy, "unsorted");
        std::snprintf(m_importSubfolderBuf, sizeof(m_importSubfolderBuf),
                      "%s", m_engine->importSubfolder().c_str());
        m_importDefaultsSeeded = true;
    }

    Engine::ImportMode mode = m_engine->importMode();
    int modeIdx = static_cast<int>(mode);
    const char* modeLabels[] = {
        "Link in place",     // Engine::ImportMode::Link
        "Copy into project", // Engine::ImportMode::Copy
    };

    ImGui::Text("Imports:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::Combo("##import-mode", &modeIdx, modeLabels, IM_ARRAYSIZE(modeLabels))) {
        m_engine->setImportMode(static_cast<Engine::ImportMode>(modeIdx),
                                m_engine->importSubfolder());
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Where new imports land:\n"
            "  Copy into project — file is copied into content/<subfolder>/.\n"
            "    Travels with the project; portable across machines.\n"
            "  Link in place — file stays at its absolute path.\n"
            "    QLab/Watchout-style; not portable without relinking.");
    }

    // Subfolder field is only meaningful in Copy mode.
    ImGui::SameLine();
    ImGui::TextDisabled("content/");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::BeginDisabled(mode != Engine::ImportMode::Copy);
    if (ImGui::InputText("##import-subfolder", m_importSubfolderBuf,
                         sizeof(m_importSubfolderBuf))) {
        m_engine->setImportMode(mode, m_importSubfolderBuf);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && mode == Engine::ImportMode::Copy) {
        ImGui::SetTooltip(
            "Target subfolder under content/ for new imports.\n"
            "Created on demand if it doesn't exist.\n"
            "Default: \"unsorted\".");
    }
}

void MediaBinWindow::render() {
    const auto& mediaFiles = m_engine->getLoadedMediaFiles();
    TranscodeManager* tmgr = m_engine->getTranscodeManager();

    // --- Toolbar: non-HAP import policy ------------------------------------
    using Policy = ProjectManager::NonHapImportPolicy;
    const char* policyLabels[] = {
        "Ask each time",
        "Auto-transcode",
        "Use source as-is",
    };
    Policy policy = m_engine->nonHapImportPolicy();
    int policyIdx = static_cast<int>(policy);
    ImGui::Text("Non-HAP imports:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::Combo("##nonhap-policy", &policyIdx, policyLabels, IM_ARRAYSIZE(policyLabels))) {
        m_engine->setNonHapImportPolicy(static_cast<Policy>(policyIdx));
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "What to do when you import a non-HAP file:\n"
            "  Ask each time — prompt with Transcode / Skip per import\n"
            "  Auto-transcode — silently re-encode to HAP. Managed imports\n"
            "    replace source at content/<sub>/ and archive the original\n"
            "    to content/<sub>/.archive/; linked imports cache the HAP\n"
            "    at <project>/.cache/hap/.\n"
            "  Use source as-is — keep the slow source (ProRes / H264 / ...)"
            );
    }

    ImGui::SameLine();
    ImGui::Text("  |  Loaded: %zu", mediaFiles.size());

    // --- ADR-0009 — Copy/Link import target toolbar -----------------------
    renderImportTargetToolbar();

    ImGui::Separator();

    // --- First-import modal ------------------------------------------------
    renderPendingImportModal();

    if (mediaFiles.empty()) {
        ImGui::TextDisabled("No media loaded");
        ImGui::Spacing();
        ImGui::TextWrapped("Use File > Open Video to import media files.");
        return;
    }

    // Build a per-path metadata cache from whatever clips exist (may be
    // empty — a freshly-imported file that's still transcoding has no
    // clip yet, so its resolution / duration cells will show "--").
    std::unordered_map<std::string, std::tuple<uint32_t, uint32_t, double, FrameNumber, bool>> metadataCache;
    {
        auto& registry = m_engine->getRegistry();
        auto clipView = registry.view<Clip>();
        for (auto [entity, clip] : clipView.each()) {
            if (metadataCache.find(clip.filepath) == metadataCache.end()) {
                metadataCache[clip.filepath] = std::make_tuple(
                    clip.width, clip.height, clip.framerate,
                    clip.totalMediaFrames, clip.hasAlpha);
            }
        }
    }

    OcioManager* ocio = m_engine->getOcioManager();
    const std::vector<std::string> colorSpaces =
        (ocio && ocio->getConfig()) ? ocio->listColorSpaces() : std::vector<std::string>{};

    // #27 — collapse versioned files into one row per logical name.
    const std::vector<VersionGroup> groups = buildGroups(mediaFiles);

    if (ImGui::BeginTable("MediaBinTable", 8,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Filename",    ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Version",     ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Codec",       ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn("Resolution",  ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Duration",    ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("FPS",         ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Alpha",       ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("Color space", ImGuiTableColumnFlags_WidthFixed, 200.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < groups.size(); i++) {
            const VersionGroup& group = groups[i];
            const auto& entry = *group.primary;          // latest member
            const std::string& filepath = entry.originalPath;
            const std::string& logicalPath = group.logicalPath;

            // Cache the "is HAP?" check. The underlying probe
            // (avformat_open_input + find_stream_info on a .mov) costs
            // 30-40ms per call on a 4K ProRes file; calling it every
            // render frame produced sustained ~17fps render during
            // playback. Codec doesn't change at runtime, so write-once
            // is enough.
            // Lazy probe of codec + metadata. Cached per filepath because
            // each probe opens the file with FFmpeg (avformat + decoder
            // open), which is 30-40ms on a 4K ProRes. Managed entries
            // store project-relative paths, so resolve through the engine
            // first to give FFmpeg an absolute path it can actually open.
            const ProbeInfo* probe = nullptr;
            if (auto it = m_probeCache.find(filepath); it != m_probeCache.end()) {
                probe = &it->second;
            } else {
                ProbeInfo info;
                const std::string absForProbe = m_engine->resolveMediaPath(filepath);
                const MediaType mt = detectMediaType(absForProbe);
                info.isHap = isHapMediaType(mt);
                if (mt != MediaType::Unknown) {
                    if (auto dec = createDecoder(mt); dec) {
                        if (dec->open(absForProbe) == Result::Success) {
                            info.width       = dec->getWidth();
                            info.height      = dec->getHeight();
                            info.framerate   = dec->getFrameRate();
                            info.totalFrames = dec->getDuration();
                            info.hasAlpha    = dec->hasAlpha();
                            info.valid       = true;
                        }
                    }
                }
                info.sourceCodecName  = probeSourceCodecName(absForProbe);
                info.tier             = classifyCodec(info.sourceCodecName);
                info.displayCodecName = prettyCodec(info.sourceCodecName);
                auto [it2, _] = m_probeCache.emplace(filepath, info);
                probe = &it2->second;
            }
            const bool isHap = probe->isHap;
            const StatusDisplay status = computeStatus(entry, tmgr, isHap);

            ImGui::TableNextRow();

            // --- Filename (display unversioned form) ---
            ImGui::TableSetColumnIndex(0);
            const size_t lastSlash = logicalPath.find_last_of("/\\");
            const std::string displayName = (lastSlash != std::string::npos)
                ? logicalPath.substr(lastSlash + 1) : logicalPath;

            ImGui::PushID(static_cast<int>(i));
            ImGui::Selectable(displayName.c_str(), false, ImGuiSelectableFlags_SpanAllColumns);

            // Tooltip lists the resolved version + every member of the
            // group so users can confirm what's on disk.
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("Logical: %s", logicalPath.c_str());
                if (!group.latestTag.empty()) {
                    ImGui::Text("Latest version: v%s", group.latestTag.c_str());
                }
                if (group.members.size() > 1) {
                    ImGui::Separator();
                    ImGui::TextDisabled("%zu versions on disk:", group.members.size());
                    for (auto it = group.members.rbegin(); it != group.members.rend(); ++it) {
                        ImGui::BulletText("%s", (*it)->originalPath.c_str());
                    }
                } else {
                    ImGui::Text("Source: %s", filepath.c_str());
                }
                if (!entry.transcodedPath.empty()) {
                    ImGui::Separator();
                    ImGui::Text("Transcoded: %s", entry.transcodedPath.c_str());
                    ImGui::Text("Variant: %s", entry.variant.c_str());
                }
                if (status.hasActiveWorker && status.transState == TranscodeState::Failed) {
                    ImGui::Separator();
                    ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.4f, 1.0f),
                                       "Transcode failed: %s",
                                       ResultToString(status.failureResult));
                    ImGui::TextDisabled("Check console for FFmpeg detail.");
                }
                ImGui::Separator();
                ImGui::TextDisabled("Right-click for options.");
                ImGui::EndTooltip();
            }

            // Drag source — payload is the LOGICAL path (no _v tag) so
            // dropped clips reference the group; the resolver auto-rolls
            // to the latest version at decode-open. Per-clip pin/rollback
            // UI lands in #29.
            if (status.dragAllowed) {
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    ImGui::SetDragDropPayload("MEDIA_FILE", logicalPath.c_str(),
                                              logicalPath.size() + 1);
                    ImGui::Text("Drag to Timeline:");
                    ImGui::Text("%s", displayName.c_str());
                    if (!group.latestTag.empty()) {
                        ImGui::TextDisabled("(plays v%s — latest)", group.latestTag.c_str());
                    }
                    ImGui::EndDragDropSource();
                }
            }

            // Right-click context menu: cancel running transcodes, kick
            // off one manually when auto-transcode is off.
            if (ImGui::BeginPopupContextItem("mediabin_row")) {
                if (status.hasActiveWorker && status.transState == TranscodeState::Running) {
                    if (ImGui::MenuItem("Cancel transcode")) {
                        if (tmgr) tmgr->cancel(filepath);
                    }
                }
                const bool canQueue =
                    !status.isSourceAlreadyHap &&
                    !status.hasTranscode &&
                    !(status.hasActiveWorker &&
                      (status.transState == TranscodeState::Running ||
                       status.transState == TranscodeState::Queued));
                if (canQueue) {
                    if (ImGui::MenuItem("Transcode to HAP")) {
                        if (tmgr) tmgr->enqueue(filepath, "hap_alpha", 0.0);
                    }
                }
                if (status.hasActiveWorker && status.transState == TranscodeState::Failed) {
                    if (ImGui::MenuItem("Retry transcode")) {
                        if (tmgr) {
                            tmgr->remove(filepath);  // wipe the failed worker first
                            tmgr->enqueue(filepath, "hap_alpha", 0.0);
                        }
                    }
                }

                // ADR-0009 — Restore Original. Available only for Managed
                // entries whose pre-transcode source is sitting in
                // .archive/ on disk. The action swaps the archive back
                // into the canonical content path; user gets the
                // original codec back, can re-trigger transcode later.
                {
                    const bool hasArchive =
                        entry.pathKind == ProjectManager::PathKind::Managed
                        && !entry.archivedOriginal.empty();
                    if (hasArchive) {
                        ImGui::Separator();
                        std::string label = "Restore Original";
                        if (!entry.originalCodec.empty()) {
                            label += " (";
                            label += entry.originalCodec;
                            label += ")";
                        }
                        if (ImGui::MenuItem(label.c_str())) {
                            m_engine->restoreOriginalMedia(filepath);
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip(
                                "Replaces the HAP transcode at the canonical\n"
                                "content path with the pre-transcode source from\n"
                                "%s.\n"
                                "The HAP version is discarded; re-transcode if you\n"
                                "want it back.",
                                entry.archivedOriginal.c_str());
                        }
                    }
                }

                ImGui::Separator();
                if (ImGui::MenuItem("Remove from library")) {
                    m_engine->removeMediaFromLibrary(filepath);
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();

            // --- Version ---
            ImGui::TableSetColumnIndex(1);
            if (group.latestTag.empty()) {
                ImGui::TextDisabled(group.members.size() > 1 ? "(multi)" : "—");
            } else {
                ImGui::Text("v%s", group.latestTag.c_str());
                if (group.members.size() > 1 && ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%zu versions; auto-rolls to v%s",
                                      group.members.size(), group.latestTag.c_str());
                }
            }

            // --- Codec (with color tier + transcode lifecycle overlay) ---
            ImGui::TableSetColumnIndex(2);
            if (status.hasActiveWorker &&
                status.transState == TranscodeState::Running) {
                // Active transcode: overlay progress on the cell.
                char label[64];
                if (status.framesTotal > 0) {
                    std::snprintf(label, sizeof(label), "%lld / %lld",
                                  static_cast<long long>(status.framesDone),
                                  static_cast<long long>(status.framesTotal));
                } else {
                    std::snprintf(label, sizeof(label), "%lld frames",
                                  static_cast<long long>(status.framesDone));
                }
                ImGui::ProgressBar(status.progress01, ImVec2(-FLT_MIN, 0), label);
            } else if (status.hasActiveWorker &&
                       status.transState == TranscodeState::Queued) {
                ImGui::TextDisabled("Queued");
            } else if (status.hasActiveWorker &&
                       status.transState == TranscodeState::Failed) {
                ImGui::TextColored(colorForTier(CodecTier::Bad),
                                   "Failed: %s",
                                   ResultToString(status.failureResult));
            } else {
                // Idle: codec name with playback-tier color. If a HAP
                // transcode exists we'll be decoding HAP regardless of
                // the source codec, so show "HAP" (green) rather than
                // the source codec.
                if (status.hasTranscode) {
                    ImGui::TextColored(colorForTier(CodecTier::Good),
                                       "HAP (transcoded)");
                    if (probe && !probe->displayCodecName.empty() &&
                        ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Source codec: %s",
                                          probe->displayCodecName.c_str());
                    }
                } else {
                    const char* name = (probe && !probe->displayCodecName.empty())
                        ? probe->displayCodecName.c_str()
                        : "(probing)";
                    const CodecTier tier = probe ? probe->tier : CodecTier::Unknown;
                    ImGui::TextColored(colorForTier(tier), "%s", name);
                    if (tier == CodecTier::Bad && ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "Long-GOP codec — slow seeking and heavy CPU "
                            "during playback. Right-click → Transcode to HAP "
                            "for realtime performance.");
                    } else if (tier == CodecTier::OK && ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(
                            "Intra-frame codec — playable but heavy. "
                            "Transcoding to HAP gives the best playback.");
                    }
                }
            }

            // Metadata: prefer clip-derived (most accurate, includes any
            // timeline-side adjustments); fall back to the probe cache so
            // scanner-discovered files (no clip yet) still display
            // resolution / duration / fps / alpha.
            bool hasMetadata = false;
            uint32_t width = 0, height = 0;
            FrameNumber duration = 0;
            double framerate = 0.0;
            bool hasAlpha = false;
            if (auto it = metadataCache.find(filepath); it != metadataCache.end()) {
                hasMetadata = true;
                std::tie(width, height, framerate, duration, hasAlpha) = it->second;
            } else if (probe && probe->valid) {
                hasMetadata = true;
                width     = probe->width;
                height    = probe->height;
                framerate = probe->framerate;
                duration  = probe->totalFrames;
                hasAlpha  = probe->hasAlpha;
            }

            ImGui::TableSetColumnIndex(3);
            if (hasMetadata) ImGui::Text("%ux%u", width, height);
            else             ImGui::TextDisabled("--");

            ImGui::TableSetColumnIndex(4);
            if (hasMetadata && framerate > 0) {
                const double durationSec = static_cast<double>(duration) / framerate;
                const int minutes = static_cast<int>(durationSec) / 60;
                const int seconds = static_cast<int>(durationSec) % 60;
                const int frames  = static_cast<int>(duration % static_cast<FrameNumber>(framerate));
                ImGui::Text("%d:%02d:%02d", minutes, seconds, frames);
            } else {
                ImGui::TextDisabled("--");
            }

            ImGui::TableSetColumnIndex(5);
            if (hasMetadata) ImGui::Text("%.2f", framerate);
            else             ImGui::TextDisabled("--");

            ImGui::TableSetColumnIndex(6);
            if (hasMetadata) ImGui::Text("%s", hasAlpha ? "Yes" : "No");
            else             ImGui::TextDisabled("--");

            // --- Color space override (Phase C.12 #9) ---
            ImGui::TableSetColumnIndex(7);
            ImGui::PushID(static_cast<int>(i) ^ 0x6359);  // distinct from row PushID
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ocio && ocio->getConfig()) {
                const char* preview = entry.inputColorSpaceOverride.empty()
                    ? "Auto (decoder)"
                    : entry.inputColorSpaceOverride.c_str();
                if (ImGui::BeginCombo("##cs", preview)) {
                    {
                        bool selected = entry.inputColorSpaceOverride.empty();
                        if (ImGui::Selectable("Auto (decoder)", selected)) {
                            m_engine->setInputColorSpaceOverride(filepath, "");
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    for (const auto& cs : colorSpaces) {
                        bool selected = (cs == entry.inputColorSpaceOverride);
                        if (ImGui::Selectable(cs.c_str(), selected)) {
                            m_engine->setInputColorSpaceOverride(filepath, cs);
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "OCIO input color space for this clip.\n"
                        "Auto = use whatever the decoder tagged (HAP / ProRes\n"
                        "metadata / PNG default). Override forces a specific\n"
                        "color space, e.g. when a ProRes file lies about its\n"
                        "AVColorSpace tag.");
                }
            } else {
                // OcioManager unbound (config failed to load) — let the user
                // type a name anyway; takes effect once a working config loads.
                char buf[128];
                std::strncpy(buf, entry.inputColorSpaceOverride.c_str(), sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
                if (ImGui::InputTextWithHint("##cs", "Auto (decoder)", buf, sizeof(buf))) {
                    m_engine->setInputColorSpaceOverride(filepath, buf);
                }
            }
            ImGui::PopID();
        }

        ImGui::EndTable();
    }
}

} // namespace entity
