#include "entity/director/SystemRateSource.hpp"

namespace entity {

double SystemRateSource::now() const {
    return std::chrono::duration<double>(
        Clock::now().time_since_epoch()).count();
}

} // namespace entity
