#include "entity/precomp/PrecompLibrary.hpp"

#include "entity/timeline/Timeline.hpp"

#include <iostream>
#include <random>

namespace entity {

PrecompLibrary::PrecompLibrary(entt::registry& registry)
    : m_registry(registry) {}

PrecompLibrary::~PrecompLibrary() = default;

PrecompDefinition* PrecompLibrary::createDefinition(const std::string& name,
                                                    std::uint32_t canvasWidth,
                                                    std::uint32_t canvasHeight,
                                                    double frameRate,
                                                    FrameNumber durationFrames) {
    auto def = std::make_unique<PrecompDefinition>();
    def->name         = name;
    def->canvasWidth  = canvasWidth;
    def->canvasHeight = canvasHeight;
    def->frameRate    = (frameRate > 0.0) ? frameRate : 30.0;
    def->durationFrames = (durationFrames >= 0) ? durationFrames : 0;

    do {
        def->id = generateId();
    } while (findDefinition(def->id) != nullptr);

    def->timeline = std::make_unique<Timeline>(m_registry);
    def->timeline->setFrameRate(def->frameRate);
    def->timeline->setDuration(def->timeline->frameToTime(def->durationFrames));

    m_definitions.push_back(std::move(def));
    PrecompDefinition* created = m_definitions.back().get();
    std::cout << "[PrecompLibrary] Created definition '" << created->name
              << "' id=" << created->id
              << " canvas=" << created->canvasWidth << "x" << created->canvasHeight
              << " fps=" << created->frameRate
              << " durationFrames=" << created->durationFrames << std::endl;
    return created;
}

PrecompDefinition* PrecompLibrary::findDefinition(const std::string& id) {
    for (auto& def : m_definitions) {
        if (def->id == id) return def.get();
    }
    return nullptr;
}

const PrecompDefinition* PrecompLibrary::findDefinition(const std::string& id) const {
    for (const auto& def : m_definitions) {
        if (def->id == id) return def.get();
    }
    return nullptr;
}

bool PrecompLibrary::touch(const std::string& id) {
    PrecompDefinition* def = findDefinition(id);
    if (!def) return false;
    ++def->version;
    return true;
}

bool PrecompLibrary::canDelete(const std::string& id) const {
    // Phase C: replace with an instance-refcount check (v1 rule — a
    // definition cannot be deleted while instances reference it).
    return findDefinition(id) != nullptr;
}

std::string PrecompLibrary::generateId() const {
    // No uuid utility exists in-tree; ids only need in-project uniqueness
    // (stability comes from being persisted strings — Phase F).
    static std::mt19937_64 rng{std::random_device{}()};
    static constexpr char kHex[] = "0123456789abcdef";
    std::uniform_int_distribution<int> dist(0, 15);
    std::string id = "precomp-";
    for (int i = 0; i < 16; ++i) id += kHex[dist(rng)];
    return id;
}

} // namespace entity
