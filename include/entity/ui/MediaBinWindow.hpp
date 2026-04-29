#pragma once

#include "EditorWindow.hpp"
#include <imgui.h>

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

private:
    void renderPendingImportModal();

    Engine* m_engine{nullptr};

    // Local "Don't ask again" checkbox state that persists across frames
    // while the popup is open. Reset each time a fresh pending import arrives.
    bool m_modalDontAskAgain{false};
    // Track the filepath the modal was opened for so we can reset the
    // checkbox when a new import triggers a new modal instance.
    std::string m_modalLastFilepath;

    // Cache of "is the source already HAP?" results keyed by filepath. The
    // underlying probe (avformat_open_input + find_stream_info on a .mov)
    // costs 30-40ms per call on a 4K ProRes file -- catastrophic when
    // called every render frame during playback. The codec of a file
    // doesn't change at runtime, so a write-once cache is enough.
    mutable std::unordered_map<std::string, bool> m_isHapCache;
};

} // namespace entity
