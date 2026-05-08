#include "entity/ui/MediaBinWindow.hpp"
#include "entity/core/Engine.hpp"
#include "entity/color/OcioManager.hpp"
#include "entity/media/Decoder.hpp"
#include "entity/media/TranscodeManager.hpp"
#include "entity/media/MediaProbeWorker.hpp"
#include "entity/components/Clip.hpp"
#include "entity/project/MediaVersioning.hpp"
#include <imgui.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <unordered_map>
#include <tuple>
#include <vector>

namespace entity {

MediaBinWindow::MediaBinWindow(Engine* engine)
    : m_engine(engine) {
}

namespace {

// classifyCodec / prettyCodec live in entity/media/MediaProbe.hpp now
// (shared with MediaProbeWorker). Anon-namespace duplicates removed.

ImVec4 colorForTier(CodecTier tier) {
    switch (tier) {
        case CodecTier::Good: return ImVec4(0.50f, 0.85f, 0.50f, 1.0f);
        case CodecTier::OK:   return ImVec4(0.90f, 0.78f, 0.45f, 1.0f);
        case CodecTier::Bad:  return ImVec4(0.95f, 0.45f, 0.45f, 1.0f);
        default:              return ImVec4(0.65f, 0.65f, 0.65f, 1.0f);
    }
}

// Case-insensitive ASCII substring search. File paths in this project are
// ASCII; no Unicode pretense. Returns true on empty needle so callers can
// skip the branch when the filter is inactive.
bool containsCi(std::string_view hay, std::string_view ndl) {
    if (ndl.empty()) return true;
    if (ndl.size() > hay.size()) return false;
    auto up = [](unsigned char c) -> unsigned char {
        return (c >= 'a' && c <= 'z') ? static_cast<unsigned char>(c - 32) : c;
    };
    const std::size_t end = hay.size() - ndl.size();
    for (std::size_t i = 0; i <= end; ++i) {
        std::size_t j = 0;
        for (; j < ndl.size(); ++j) {
            if (up(static_cast<unsigned char>(hay[i + j])) !=
                up(static_cast<unsigned char>(ndl[j]))) break;
        }
        if (j == ndl.size()) return true;
    }
    return false;
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

// Folder tree built from the bin's flat group list. Mirrors the on-disk
// `content/<sub>/...` shape so the user sees the same hierarchy they
// organized. The leading `content/` segment is dropped (it's implicit).
// Linked entries (absolute paths from outside the project) all bucket
// under a synthetic "(Linked media)" root — rendering an absolute
// `C:\Users\...` path as a tree would be useless.
struct FolderNode {
    std::string                       name;        // segment, not full path
    std::vector<FolderNode>           subfolders;  // kept sorted by name
    std::vector<const VersionGroup*>  groups;      // insertion-order from buildGroups
};

// Get-or-create a child folder by name, keeping `subfolders` sorted so
// renders are deterministic.
FolderNode& folderChild(FolderNode& parent, const std::string& name) {
    auto it = std::lower_bound(parent.subfolders.begin(), parent.subfolders.end(), name,
        [](const FolderNode& n, const std::string& s) { return n.name < s; });
    if (it != parent.subfolders.end() && it->name == name) return *it;
    FolderNode child;
    child.name = name;
    return *parent.subfolders.insert(it, std::move(child));
}

// Split a project-relative path on `/` or `\` into segments, dropping
// empty pieces (handles trailing/double slashes defensively).
std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/' || path[i] == '\\') {
            if (i > start) out.emplace_back(path.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

FolderNode buildFolderTree(const std::vector<VersionGroup>& groups) {
    FolderNode root;
    for (const auto& g : groups) {
        if (g.primary->pathKind == ProjectManager::PathKind::Linked) {
            FolderNode& linked = folderChild(root, "(Linked media)");
            linked.groups.push_back(&g);
            continue;
        }
        // Managed: split, drop leading "content" if present, walk to the
        // file's parent folder.
        const std::vector<std::string> segs = splitPath(g.primary->originalPath);
        if (segs.empty()) continue;
        const std::size_t first = (segs.front() == "content") ? 1 : 0;
        FolderNode* node = &root;
        // Walk all segments except the last (which is the filename).
        for (std::size_t i = first; i + 1 < segs.size(); ++i) {
            node = &folderChild(*node, segs[i]);
        }
        node->groups.push_back(&g);
    }
    return root;
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
        if (pending->sourceFilePath != m_modalLastFilepath) {
            m_modalLastFilepath = pending->sourceFilePath;
            // Seed defaults from whatever was pre-decided.
            m_modalChosenMode      = pending->storageDecided
                ? static_cast<int>(pending->resolvedMode)
                : 1;  // Copy default — matches the muscle memory of the old toolbar
            m_modalSubfolderBuf[0] = '\0';
            m_modalChosenTranscode = pending->transcodeDecided
                ? (pending->resolvedTranscode ? 1 : 0)
                : 1;  // transcode-by-default for non-HAP — best UX
            m_modalDontAskStorage   = false;
            m_modalDontAskTranscode = false;
            ImGui::OpenPopup("Import Media");
        }
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560, 0));

    if (ImGui::BeginPopupModal("Import Media", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        // Pending may go null for one frame between resolve and the next
        // render; cache the source path for display in that gap.
        const std::string& path =
            pending ? pending->sourceFilePath : m_modalLastFilepath;
        const size_t lastSlash = path.find_last_of("/\\");
        const std::string stem = (lastSlash != std::string::npos)
            ? path.substr(lastSlash + 1) : path;

        ImGui::TextDisabled("Importing:");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(stem.c_str());
        ImGui::PopTextWrapPos();
        ImGui::TextDisabled("%s", path.c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const bool askStorage  = pending && !pending->storageDecided;
        const bool askTranscode = pending && !pending->transcodeDecided;

        // --- Storage question ---
        if (askStorage) {
            ImGui::TextUnformatted("Storage");
            ImGui::RadioButton("Copy into project##store", &m_modalChosenMode, 1);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Copy the file into <project>/content/. Travels with the\n"
                    "project; portable across machines.");
            }
            ImGui::RadioButton("Link to original location##store", &m_modalChosenMode, 0);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Reference the file at its current absolute path.\n"
                    "QLab/Watchout-style; not portable without relinking.");
            }

            // Subfolder input — only meaningful in Copy mode.
            ImGui::BeginDisabled(m_modalChosenMode != 1);
            ImGui::TextDisabled("content/");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(220.0f);
            ImGui::InputTextWithHint("##subfolder", "(optional subfolder)",
                                      m_modalSubfolderBuf,
                                      sizeof(m_modalSubfolderBuf));
            if (ImGui::IsItemHovered() && m_modalChosenMode == 1) {
                ImGui::SetTooltip(
                    "Optional subfolder under content/ for this import.\n"
                    "Created on demand if it doesn't exist. Leave blank to\n"
                    "land in content/ root.");
            }
            ImGui::EndDisabled();

            ImGui::Checkbox("Don't ask Storage again##store", &m_modalDontAskStorage);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Persist this Storage choice (Copy or Link) as the default\n"
                    "for all future imports on this machine. Edit settings.json\n"
                    "to reset.");
            }
            ImGui::Spacing();
        }

        // --- Transcode question ---
        if (askTranscode) {
            if (askStorage) {
                ImGui::Separator();
                ImGui::Spacing();
            }
            ImGui::TextUnformatted("Transcode");
            ImGui::RadioButton("Transcode to HAP (recommended)##tx",
                                &m_modalChosenTranscode, 1);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "HAP is a GPU-friendly codec — transcoding this file\n"
                    "lets it play smoothly on a multi-layer timeline.\n"
                    "Source is preserved (archived for managed imports).");
            }
            ImGui::RadioButton("Use source as-is (slow path)##tx",
                                &m_modalChosenTranscode, 0);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Play the source codec directly. Heavier on CPU/GPU\n"
                    "but no transcoding wait. You can transcode later from\n"
                    "the MediaBin row's right-click menu.");
            }
            ImGui::Checkbox("Don't ask Transcode again##tx", &m_modalDontAskTranscode);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Persist this Transcode choice as the project default\n"
                    "(Always Transcode / Never Transcode).");
            }
            ImGui::Spacing();
        }

        ImGui::Separator();
        ImGui::Spacing();

        const float btnWidth = 120.0f;
        if (ImGui::Button("Import", ImVec2(btnWidth, 0))) {
            // Resolve into Engine. Pre-decided fields are passed verbatim
            // (the radio reflected the resolved value); newly-asked
            // fields use the radio's chosen value.
            const auto mode = static_cast<Engine::ImportMode>(m_modalChosenMode);
            const std::string subfolder = m_modalSubfolderBuf;
            const bool transcode = (m_modalChosenTranscode != 0);
            m_engine->resolvePendingImport(
                mode, subfolder, transcode,
                /*rememberStorage*/   askStorage   && m_modalDontAskStorage,
                /*rememberTranscode*/ askTranscode && m_modalDontAskTranscode);
            m_modalLastFilepath.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(btnWidth, 0))) {
            // Cancel = behave as "Use source as-is + Link" with no
            // remember. Nothing gets copied or transcoded; if the source
            // is reachable the clip still lands on the timeline. If the
            // user wanted to truly cancel we'd need to drop the import
            // entirely — leave that as a follow-up.
            m_engine->resolvePendingImport(
                Engine::ImportMode::Link, /*subfolder*/ "",
                /*transcode*/ false,
                /*rememberStorage*/ false, /*rememberTranscode*/ false);
            m_modalLastFilepath.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    } else if (pending && m_modalLastFilepath == pending->sourceFilePath) {
        // Popup dismissed externally (e.g. ESC). Match Cancel behavior.
        m_engine->resolvePendingImport(
            Engine::ImportMode::Link, "", false, false, false);
        m_modalLastFilepath.clear();
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

    // --- Search filter ----------------------------------------------------
    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputTextWithHint("##bin_filter", "Search media...",
                              m_filterBuf, sizeof(m_filterBuf));
    if (m_filterBuf[0] != '\0') {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##bin_filter")) {
            m_filterBuf[0] = '\0';
        }
    }

    ImGui::Separator();

    // --- Unified per-import modal (#32) -----------------------------------
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

    const ImVec2 fp = ImGui::GetStyle().FramePadding;
    ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, fp.y));
    if (ImGui::BeginTable("MediaBinTable", 8,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                           ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Filename",    ImGuiTableColumnFlags_WidthFixed, 320.0f);
        ImGui::TableSetupColumn("Version",     ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Codec",       ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn("Resolution",  ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Duration",    ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("FPS",         ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Alpha",       ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("Color space", ImGuiTableColumnFlags_WidthFixed, 200.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        const std::string_view filter{m_filterBuf};

        // Render one VersionGroup as a table row. The Filename column
        // always shows just the leaf filename — folder context comes from
        // the tree node it sits under, not from the row label. The leaf
        // hit target is a TreeNodeEx with Leaf | NoTreePushOnOpen so it
        // indents correctly under its parent folder.
        auto renderGroupRow = [&](const VersionGroup& group) {
            const auto& entry = *group.primary;          // latest member
            const std::string& filepath = entry.originalPath;
            const std::string& logicalPath = group.logicalPath;

            // Probe metadata (codec / resolution / framerate / alpha) is
            // populated off-thread by Engine::probeWorker (see
            // entity/media/MediaProbeWorker.hpp). The synchronous in-row
            // probe that used to live here cost 30-60ms per first-render
            // and froze the UI during startup with a few dozen files.
            //
            // tryGet returns nullopt while the worker hasn't completed
            // this row's probe yet; the row renders with "(probing)" /
            // "--" placeholders and fills in next frame after the worker
            // posts a result. The enqueue call is a safety net for paths
            // that slipped past Engine's bulk enqueue (Linked entries
            // with empty resolved paths, etc.) — idempotent.
            std::optional<ProbeInfo> probeOpt;
            if (m_engine && m_engine->probeWorker()) {
                probeOpt = m_engine->probeWorker()->tryGet(filepath);
                if (!probeOpt) m_engine->probeWorker()->enqueue(filepath);
            }
            const ProbeInfo* probe = probeOpt ? &*probeOpt : nullptr;
            const bool isHap = probe ? probe->isHap : false;
            const StatusDisplay status = computeStatus(entry, tmgr, isHap);

            ImGui::TableNextRow();

            // --- Filename (just the leaf — folder context comes from the parent tree node) ---
            ImGui::TableSetColumnIndex(0);
            const size_t lastSlash = logicalPath.find_last_of("/\\");
            const std::string displayName = (lastSlash != std::string::npos)
                ? logicalPath.substr(lastSlash + 1) : logicalPath;

            // Filepath is unique per entry, so it's a stable ID across
            // filter-toggle. ImGui hashes the C-string.
            ImGui::PushID(filepath.c_str());
            ImGui::TreeNodeEx(displayName.c_str(),
                ImGuiTreeNodeFlags_Leaf
                | ImGuiTreeNodeFlags_NoTreePushOnOpen
                | ImGuiTreeNodeFlags_SpanAvailWidth);

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
            ImGui::PushID(filepath.c_str());
            ImGui::PushID("cs");  // nested so we don't collide with the leaf PushID block above
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
            ImGui::PopID();  // "cs"
            ImGui::PopID();  // filepath
        };  // renderGroupRow lambda

        // True if this folder (or any descendant) has at least one group
        // whose logical path matches `f`. Used to prune empty branches
        // out of the tree while a search is active.
        std::function<bool(const FolderNode&, std::string_view)> subtreeMatches =
            [&](const FolderNode& node, std::string_view f) -> bool {
                if (f.empty()) return true;
                for (const auto* g : node.groups) {
                    if (containsCi(g->logicalPath, f)) return true;
                }
                for (const auto& sub : node.subfolders) {
                    if (subtreeMatches(sub, f)) return true;
                }
                return false;
            };

        // Recursive folder walker. Subfolders render as TreeNodeEx; leaves
        // call into renderGroupRow. While searching, branches with no
        // matching descendant are skipped entirely and folders are forced
        // open so matches are visible without manual expansion.
        std::function<void(const FolderNode&)> renderFolder =
            [&](const FolderNode& f) {
                for (const auto& sub : f.subfolders) {
                    if (!subtreeMatches(sub, filter)) continue;
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (!filter.empty()) {
                        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                    }
                    bool open = ImGui::TreeNodeEx(sub.name.c_str(),
                        ImGuiTreeNodeFlags_SpanAvailWidth
                        | ImGuiTreeNodeFlags_DefaultOpen);
                    if (open) {
                        renderFolder(sub);
                        ImGui::TreePop();
                    }
                }
                for (const auto* g : f.groups) {
                    if (!filter.empty() && !containsCi(g->logicalPath, filter)) {
                        continue;
                    }
                    renderGroupRow(*g);
                }
            };

        const FolderNode root = buildFolderTree(groups);
        renderFolder(root);

        ImGui::EndTable();
    }
    ImGui::PopStyleVar(2);  // FramePadding, IndentSpacing
}

} // namespace entity
