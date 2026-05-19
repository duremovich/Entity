#include "entity/ui/SettingsWindow.hpp"
#include "entity/ui/MathInput.hpp"
#include "entity/color/OcioManager.hpp"

#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace entity {

namespace {

constexpr const char* kPopupId = "Preferences##SettingsModal";

// Render a frame-cache slider with MB-granularity. The underlying field is
// uint64_t bytes; we expose it in MiB to keep the spinner sane.
void renderFrameCacheRow(uint64_t& bytes) {
    int mib = static_cast<int>(bytes / (1024ull * 1024ull));
    // Reasonable bounds: 64 MiB minimum (anything less and the cache barely
    // holds a single 4K HAP-Q frame), 16 GiB ceiling (above this you're
    // pre-loading the entire show — different problem, different setting).
    constexpr int kMinMiB = 64;
    constexpr int kMaxMiB = 16 * 1024;
    if (entity::ui::SliderInt("Frame Cache (MiB)##frameCache", &mib,
                         kMinMiB, kMaxMiB,
                         "%d MiB",
                         ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic)) {
        mib = std::clamp(mib, kMinMiB, kMaxMiB);
        bytes = static_cast<uint64_t>(mib) * 1024ull * 1024ull;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(
            "RAM budget for decoded frames. Higher = fewer re-decodes when\n"
            "scrubbing back into already-viewed regions, more RAM in use.\n"
            "Default 512 MiB ≈ 128 frames @ 4K HAP-Q.\n"
            "Bump on a workstation with lots of RAM; drop on a laptop.\n"
            "Wired up by Phase C.10 (FrameCache).");
        ImGui::EndTooltip();
    }
}

// Render an editable string as an ImGui InputText backed by a fixed buffer.
// Writes back into `value` if changed. Width is the full available region.
void renderStringField(const char* label, std::string& value, ImGuiInputTextFlags flags = 0) {
    char buf[1024];
    const size_t n = std::min(value.size(), sizeof(buf) - 1);
    std::memcpy(buf, value.data(), n);
    buf[n] = '\0';
    if (ImGui::InputText(label, buf, sizeof(buf), flags)) {
        value.assign(buf);
    }
}

// Pure dropdown with explicit "(config default)" first entry mapping to "".
// Returns true if `value` changed.
bool renderColorSpaceCombo(const char* label,
                           const std::vector<std::string>& options,
                           std::string& value,
                           bool allowEmptyDefault) {
    const char* preview = value.empty() ? (allowEmptyDefault ? "(config default)" : "(none)")
                                        : value.c_str();
    bool changed = false;
    if (ImGui::BeginCombo(label, preview)) {
        if (allowEmptyDefault) {
            const bool selected = value.empty();
            if (ImGui::Selectable("(config default)", selected)) {
                if (!value.empty()) { value.clear(); changed = true; }
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        for (const auto& opt : options) {
            const bool selected = (opt == value);
            if (ImGui::Selectable(opt.c_str(), selected)) {
                if (opt != value) { value = opt; changed = true; }
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

void renderColorSection(Settings& staged,
                        OcioManager* ocio,
                        const SettingsWindow::BrowseOcioCallback& browseOcio) {
    // OCIO config path with optional Browse... button.
    {
        ImGui::PushItemWidth(-110.0f);
        renderStringField("##ocioConfigPath", staged.ocioConfigPath);
        ImGui::PopItemWidth();
        ImGui::SameLine();
        if (ImGui::Button("Browse...##ocio")) {
            if (browseOcio) {
                // staged is &m_staged inside SettingsWindow, which lives
                // for the app's lifetime — safe to capture across frames.
                browseOcio([&staged](std::string chosen) {
                    if (!chosen.empty()) {
                        staged.ocioConfigPath = std::move(chosen);
                    }
                });
            }
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("OCIO config");
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(
                "Path to a custom OpenColorIO config file (.ocio).\n"
                "Leave blank to use the bundled ACES Studio Config 1.3.\n"
                "Switching configs requires an app restart in C.12.");
            ImGui::EndTooltip();
        }
    }

    // Default video / PNG color spaces.
    if (ocio && ocio->getConfig()) {
        const auto colorSpaces = ocio->listColorSpaces();
        renderColorSpaceCombo("Default video color space",
                              colorSpaces, staged.defaultVideoInputCs,
                              /*allowEmptyDefault=*/false);
        renderColorSpaceCombo("Default PNG color space",
                              colorSpaces, staged.defaultPngInputCs,
                              /*allowEmptyDefault=*/false);

        const auto displays = ocio->listDisplays();
        renderColorSpaceCombo("Default output display",
                              displays, staged.defaultDisplay,
                              /*allowEmptyDefault=*/true);

        // View dropdown filtered by the selected display (or the config's
        // default display if the user hasn't picked one).
        std::string displayForViews = staged.defaultDisplay.empty()
                                          ? ocio->getDefaultDisplay()
                                          : staged.defaultDisplay;
        const auto views = ocio->listViews(displayForViews);
        renderColorSpaceCombo("Default output view",
                              views, staged.defaultView,
                              /*allowEmptyDefault=*/true);
    } else {
        // OcioManager not bound (or config failed to load). Fall back to
        // free-form text entry so the user can still set defaults that will
        // take effect on next launch with a working config.
        renderStringField("Default video color space##cs",   staged.defaultVideoInputCs);
        renderStringField("Default PNG color space##cs",     staged.defaultPngInputCs);
        renderStringField("Default output display##disp",    staged.defaultDisplay);
        renderStringField("Default output view##view",       staged.defaultView);
        ImGui::TextDisabled("OCIO config not loaded — dropdowns disabled.");
    }

    ImGui::TextDisabled("OCIO config switches require app restart in C.12.");
}

void renderOscReceiverSection(Settings& staged) {
    ImGui::Checkbox("Enable OSC receiver", &staged.oscReceiverEnabled);

    int port = static_cast<int>(staged.oscReceiverPort);
    if (!staged.oscReceiverEnabled) ImGui::BeginDisabled();
    if (entity::ui::InputInt("Listener port##oscPort", &port, 1, 100,
                        ImGuiInputTextFlags_EnterReturnsTrue)) {
        port = std::clamp(port, 1, 65535);
        staged.oscReceiverPort = static_cast<uint16_t>(port);
    }
    if (!staged.oscReceiverEnabled) ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(
            "UDP port for the inbound OSC receiver plugin.\n"
            "Default 53000 matches QLab's outbound default.\n"
            "Point QLab Network Cues at <this-machine-ip>:<port>.\n"
            "Address namespace: /entity/play, /entity/pause,\n"
            "/entity/stop, /entity/section/next,\n"
            "/entity/cue/{number}/go, /entity/seek <int frame>.");
        ImGui::EndTooltip();
    }

    ImGui::TextDisabled("OSC changes take effect after restart.");
}

void renderDmxSection(Settings& staged) {
    // --- Inbound: Art-Net ---
    ImGui::Checkbox("Enable Art-Net listener", &staged.dmxArtnetEnabled);
    int artnetPort = static_cast<int>(staged.dmxArtnetListenPort);
    if (!staged.dmxArtnetEnabled) ImGui::BeginDisabled();
    if (entity::ui::InputInt("Art-Net port##dmxArtnetPort", &artnetPort, 1, 100,
                              ImGuiInputTextFlags_EnterReturnsTrue)) {
        artnetPort = std::clamp(artnetPort, 1, 65535);
        staged.dmxArtnetListenPort = static_cast<uint16_t>(artnetPort);
    }
    if (!staged.dmxArtnetEnabled) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(
            "UDP port for inbound Art-Net DMX. Default 6454 is the\n"
            "Art-Net well-known port (industry standard).\n"
            "Baked default mapping: Universe 0\n"
            "  ch 1 -> Play     ch 5 -> FireCue 1\n"
            "  ch 2 -> Pause    ch 6 -> FireCue 2\n"
            "  ch 3 -> Stop     ch 7 -> FireCue 3\n"
            "  ch 4 -> SectionGo ch 8 -> FireCue 4\n"
            "Threshold-edge triggers (channel >= 128 fires).\n"
            "Custom mappings come from the active project (v22+).");
        ImGui::EndTooltip();
    }

    // --- Inbound: sACN ---
    ImGui::Checkbox("Enable sACN (E1.31) listener", &staged.dmxSacnEnabled);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(
            "Streaming ACN (ESTA E1.31) multicast listener on UDP\n"
            "5568. Port is spec-fixed; joins multicast groups\n"
            "239.255.<universe-high>.<universe-low> lazily for each\n"
            "universe the mapping table references. When both Art-Net\n"
            "and sACN feed the same universe, the higher-priority\n"
            "source wins; ties resolve to the most recent. Art-Net is\n"
            "tagged at priority 100; sACN carries its own.");
        ImGui::EndTooltip();
    }

    // --- Enttec ---
    ImGui::Checkbox("Enable Enttec DMX-USB-Pro (Windows)", &staged.dmxEnttecEnabled);
    if (!staged.dmxEnttecEnabled) ImGui::BeginDisabled();
    renderStringField("COM port##dmxEnttecPort", staged.dmxEnttecPort);
    int enttecUniverse = static_cast<int>(staged.dmxEnttecUniverse);
    if (entity::ui::InputInt("Enttec universe##dmxEnttec", &enttecUniverse, 1, 10,
                              ImGuiInputTextFlags_EnterReturnsTrue)) {
        enttecUniverse = std::clamp(enttecUniverse, 0, 32767);
        staged.dmxEnttecUniverse = static_cast<uint16_t>(enttecUniverse);
    }
    if (!staged.dmxEnttecEnabled) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(
            "Enttec DMX-USB-Pro inbound via the FTDI Virtual COM Port\n"
            "driver. Empty COM port -> auto-pick first FTDI device.\n"
            "Each Pro is a single-universe interface; the universe\n"
            "field selects which logical universe to merge the read\n"
            "data into in the arbitration table. Windows-only in v1.");
        ImGui::EndTooltip();
    }

    ImGui::Separator();
    // --- Outbound ---
    ImGui::Checkbox("Enable DMX output", &staged.dmxOutEnabled);
    if (!staged.dmxOutEnabled) ImGui::BeginDisabled();
    ImGui::Checkbox("Send sACN multicast", &staged.dmxOutSacnEnabled);
    renderStringField("Art-Net targets##dmxOutTargets", staged.dmxOutArtnetTargets);
    if (!staged.dmxOutEnabled) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(
            "Output ticks at ~44 Hz (Enttec USB-Pro spec default).\n"
            "Art-Net targets: comma-separated dotted-quad IPs.\n"
            "Empty -> broadcast to 255.255.255.255.\n"
            "Driven by the SetDmxOut script command (and future\n"
            "timeline cells). sACN sender carries priority 100.");
        ImGui::EndTooltip();
    }

    ImGui::TextDisabled("DMX changes take effect after restart.");
}

} // namespace

void SettingsWindow::open(const Settings& current) {
    m_staged = current;
    m_pendingOpen = true;
}

void SettingsWindow::render() {
    if (m_pendingOpen) {
        ImGui::OpenPopup(kPopupId);
        m_pendingOpen = false;
        m_open = true;
    }

    // Center on the viewport so the modal doesn't end up in the corner.
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(540, 0), ImGuiCond_Appearing);

    if (!ImGui::BeginPopupModal(kPopupId, &m_open,
                                ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::TextUnformatted("Machine-global Entity preferences. Saved to:");
    char pathBuf[512];
    std::snprintf(pathBuf, sizeof(pathBuf), "%s",
                  reinterpret_cast<const char*>(settingsPath().u8string().c_str()));
    ImGui::TextDisabled("%s", pathBuf);
    ImGui::Separator();

    // ----- Playback / Cache -------------------------------------------------
    if (ImGui::CollapsingHeader("Playback", ImGuiTreeNodeFlags_DefaultOpen)) {
        renderFrameCacheRow(m_staged.frameCacheBytes);
    }

    // ----- Color (Phase C.12 #7) -------------------------------------------
    if (ImGui::CollapsingHeader("Color", ImGuiTreeNodeFlags_DefaultOpen)) {
        renderColorSection(m_staged, m_ocioManager, m_browseOcio);
    }

    // ----- OSC Receiver -----------------------------------------------------
    if (ImGui::CollapsingHeader("OSC Receiver", ImGuiTreeNodeFlags_DefaultOpen)) {
        renderOscReceiverSection(m_staged);
    }

    // ----- DMX (Art-Net / sACN / Enttec) -----------------------------------
    if (ImGui::CollapsingHeader("DMX (Art-Net / sACN / Enttec)")) {
        renderDmxSection(m_staged);
    }

    ImGui::Separator();

    const float buttonWidth = 100.0f;
    // Right-align: layout = [Cancel] [OK] flush to the right edge.
    const float availableX = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + availableX - 2 * buttonWidth - ImGui::GetStyle().ItemSpacing.x);

    if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0))) {
        m_open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("OK", ImVec2(buttonWidth, 0))) {
        if (m_apply) m_apply(m_staged);
        m_open = false;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

} // namespace entity
