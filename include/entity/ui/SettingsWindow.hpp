#pragma once

#include "entity/core/Settings.hpp"
#include <functional>

namespace entity {

/**
 * SettingsWindow — modal Preferences dialog.
 *
 * Not an EditorWindow (those are dockable workflow surfaces). Settings is a
 * modal: the user clicks Edit → Preferences..., picks values, clicks OK to
 * save or Cancel to discard. Closed by default; opened on demand by the
 * menu item.
 *
 * Holds a *staged copy* of the settings while open, so partial edits don't
 * touch the live values until the user commits. On OK we hand the staged
 * copy back via `onApply` and the caller is responsible for both updating
 * its in-memory Settings and persisting via saveSettings().
 */
class SettingsWindow {
public:
    using ApplyCallback = std::function<void(const Settings&)>;

    SettingsWindow() = default;

    /** Begin showing the modal. Captures `current` as the starting values. */
    void open(const Settings& current);

    /** Render once per frame. No-op when not open. */
    void render();

    /** Hand back staged values when the user clicks OK. */
    void setApplyCallback(ApplyCallback cb) { m_apply = std::move(cb); }

private:
    bool      m_open{false};
    bool      m_pendingOpen{false}; // OpenPopup must run inside the BeginMenuBar pass
    Settings  m_staged{};
    ApplyCallback m_apply;
};

} // namespace entity
