#pragma once

#include "EditorWindow.hpp"
#include "entity/core/Types.hpp"
#include <imgui.h>

#include <cstdint>
#include <string>
#include <unordered_map>

namespace entity {

// Forward declaration
class Engine;

/**
 * MediaBinWindow - Displays loaded media files and their properties.
 *
 * Shows a list of all media files that have been imported into the project.
 * Displays metadata such as resolution, duration, framerate, and codec info.
 */
class MediaBinWindow : public EditorWindow {
public:
    explicit MediaBinWindow(Engine* engine);
    ~MediaBinWindow() override = default;

    void render() override;
    const char* getName() const override { return "Media Bin"; }

    // Public so free helpers in the .cpp's anonymous namespace can name
    // the enum without going through `MediaBinWindow::` everywhere.
    enum class CodecTier {
        Unknown,
        Good,  // HAP* — designed for realtime, green
        OK,    // ProRes / DNxHD / image sequences — intra-frame, yellow
        Bad,   // H.264 / H.265 / VP9 / AV1 — interframe, seek-hostile, red
    };

private:
    void renderPendingImportModal();

    Engine* m_engine{nullptr};

    // #32 — unified import modal state. Reset on every new pending
    // import so each file gets a fresh ask. Storage mode stored as int
    // to avoid pulling Engine.hpp into this header.
    std::string m_modalLastFilepath;
    int         m_modalChosenMode{1};      // 0=Link, 1=Copy (matches Engine::ImportMode)
    char        m_modalSubfolderBuf[128]{0};
    int         m_modalChosenTranscode{1}; // 0=as-is, 1=transcode (RadioButton wants int*)
    bool        m_modalDontAskStorage{false};
    bool        m_modalDontAskTranscode{false};

    // Substring filter applied to the bin's logical path. Empty = show all.
    char        m_filterBuf[128]{0};

    // Cache of probe results keyed by the entry's stored filepath.
    // First-row-render opens the file with FFmpeg to extract codec
    // (HAP vs. ProRes vs. PNG seq), resolution, framerate, duration,
    // and alpha-channel presence. Each probe costs 30-40 ms on a 4K
    // ProRes file -- catastrophic to run every frame -- so cache is
    // write-once. Entries are populated lazily; ScrollY in the table
    // means we only pay for visible rows (#27 — auto-discovered files
    // need this so MediaBin shows resolution etc. before any clip
    // is created on the timeline).
    struct ProbeInfo {
        bool        valid{false};
        bool        isHap{false};
        std::uint32_t width{0};
        std::uint32_t height{0};
        double      framerate{0.0};
        FrameNumber totalFrames{0};
        bool        hasAlpha{false};
        std::string sourceCodecName;   // "hap", "prores", "h264", ...
        std::string displayCodecName;  // pretty form: "HAP", "ProRes", "H.264"
        CodecTier   tier{CodecTier::Unknown};
    };
    mutable std::unordered_map<std::string, ProbeInfo> m_probeCache;
};

} // namespace entity
