#pragma once

#include "EditorWindow.hpp"
#include "entity/core/Types.hpp"
#include "entity/media/MediaProbe.hpp"  // ProbeInfo + CodecTier (shared with MediaProbeWorker)
#include <imgui.h>

#include <string>

namespace entity {

// Forward declaration
class Engine;

/**
 * MediaBinWindow - Displays loaded media files and their properties.
 *
 * Shows a list of all media files that have been imported into the project.
 * Displays metadata such as resolution, duration, framerate, and codec info.
 *
 * Probe metadata (codec, resolution, framerate) is fetched non-blockingly
 * from Engine::probeWorker(); rows render with a "(probing)" placeholder
 * until the worker thread fills the cache.
 */
class MediaBinWindow : public EditorWindow {
public:
    explicit MediaBinWindow(Engine* engine);
    ~MediaBinWindow() override = default;

    void render() override;
    const char* getName() const override { return "Media Bin"; }

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
};

} // namespace entity
