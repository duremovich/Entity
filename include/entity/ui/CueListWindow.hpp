#pragma once

#include "EditorWindow.hpp"
#include "entity/core/Types.hpp"
#include <imgui.h>

namespace entity {

// Forward declarations
class Engine;

/**
 * CueListWindow — dockable list of all cue tags on the timeline.
 *
 * Columns: number (2-decimal), label, time (SMPTE), actions.
 *  - Double-click row: enqueue FireCueCommand (seek + play).
 *  - Right-click row: same context menu as the ruler-flag right-click
 *    (Fire / Edit / Delete).
 */
class CueListWindow : public EditorWindow {
public:
    explicit CueListWindow(Engine* engine);
    ~CueListWindow() override = default;

    void render() override;
    const char* getName() const override { return "Cues"; }

private:
    Engine* m_engine{nullptr};

    // Edit/Add modal state. -1 in m_editTargetNumber means "Add", any other
    // value means "Edit existing cue with this old number".
    bool   m_openEditModal{false};
    bool   m_isEditMode{false};
    double m_editOldNumber{0.0};
    double m_editNumber{1.0};
    FrameNumber m_editFrame{0};
    char   m_editLabelBuf[256]{0};
};

} // namespace entity
