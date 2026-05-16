#pragma once

#include "EditorWindow.hpp"
#include <imgui.h>
#include <string>

namespace entity {

// Forward declarations
class Engine;
class WindowManager;

/**
 * ModelBinWindow - Displays imported 3D models for use as screens / props.
 *
 * Mirrors the MediaBinWindow flow: external file drops + an Import button
 * route through Engine::onModelFileSelected, which either pins the
 * storage decision from Settings or stashes a PendingObjectImport that
 * this window renders as a Copy/Link modal. Model entities are keyed by
 * logical path (one per `_v<tag>` group); the tooltip lists the version
 * members the underlying ObjectLibraryEntry list carries.
 */
class ModelBinWindow : public EditorWindow {
public:
    explicit ModelBinWindow(Engine* engine, WindowManager* windowManager);
    ~ModelBinWindow() override = default;

    void render() override;
    const char* getName() const override { return "Model Bin"; }

private:
    void renderPendingImportModal();
    void renderPendingDuplicateModal();

    Engine* m_engine{nullptr};
    WindowManager* m_windowManager{nullptr};

    // Unified object-import modal state. Storage mode stored as int to
    // avoid pulling Engine.hpp into this header. Transcode question is
    // not part of the object modal (objects have no transcode flow).
    std::string m_modalLastFilepath;
    int         m_modalChosenMode{1};      // 0=Link, 1=Copy
    char        m_modalSubfolderBuf[128]{0};
    bool        m_modalDontAskStorage{false};

    // Tracks which duplicate prompt we've already opened so we don't
    // re-open it every frame while it's modal.
    std::string m_dupModalLastSource;
};

} // namespace entity
