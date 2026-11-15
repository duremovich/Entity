#pragma once

/**
 * NoticeCenter — user-facing error / warning notices for failures that
 * would otherwise only reach stderr (project open, save, autosave).
 *
 * Two presentations:
 *   - Modal: a centered popup with an OK button. For failures of an
 *     action the user just took (Ctrl+S, File > Open), where an
 *     interruption is the right answer.
 *   - Toast: a small non-blocking panel in the bottom-left corner that
 *     never takes focus or blocks input. For background failures
 *     (autosave) that must not interrupt a running show.
 *
 * A non-empty `key` makes a notice replace any earlier notice with the
 * same key, so a failure that repeats every 30 s shows one toast, not a
 * stack; dismissKey() clears it once the condition recovers.
 *
 * The queue is plain data (unit-testable without ImGui); render() draws
 * it and is called once per ImGui frame by Engine, in both editor and
 * launcher mode. post()/dismiss() are mutex-guarded; render() runs on
 * the editor thread.
 */

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace entity {

class NoticeCenter {
public:
    enum class Severity : std::uint8_t { Info, Warning, Error };
    enum class Presentation : std::uint8_t { Modal, Toast };

    struct Notice {
        std::uint64_t id{0};
        Severity      severity{Severity::Error};
        Presentation  presentation{Presentation::Modal};
        std::string   key;
        std::string   title;
        std::string   message;
    };

    /// Queue a notice. Returns its id. A non-empty key replaces any queued
    /// notice with the same key (the new one keeps its own id).
    std::uint64_t post(Severity severity, Presentation presentation,
                       std::string title, std::string message,
                       std::string key = {});

    void dismiss(std::uint64_t id);
    void dismissKey(std::string_view key);
    void clear();

    /// Snapshot of the queued notices, oldest first.
    std::vector<Notice> snapshot() const;
    std::size_t count() const;
    bool hasKey(std::string_view key) const;

    /// Draw queued notices with ImGui. Must be called between an ImGui
    /// NewFrame/Render pair on the editor thread.
    void render();

private:
    mutable std::mutex  m_mutex;
    std::vector<Notice> m_notices;
    std::uint64_t       m_nextId{1};
    std::uint64_t       m_openModalId{0};   // render(): modal currently shown
};

} // namespace entity
