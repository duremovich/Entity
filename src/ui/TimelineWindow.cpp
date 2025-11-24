#include "entity/ui/TimelineWindow.hpp"

namespace entity {

TimelineWindow::TimelineWindow(Timeline* timeline) {
    m_widget = std::make_unique<TimelineWidget>(timeline);
}

void TimelineWindow::render() {
    // Delegate rendering to the TimelineWidget
    if (m_widget) {
        m_widget->render();
    }
}

} // namespace entity
