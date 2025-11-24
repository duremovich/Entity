#pragma once

#include "EditorWindow.hpp"
#include <imgui.h>

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
    Engine* m_engine;
};

} // namespace entity
