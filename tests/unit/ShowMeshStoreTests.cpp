// Show-thread mesh store (ADR-0014): the projector output path reads Model
// geometry from a show-owned copy delivered by bus::ProvisionMesh, never from
// the registry the editor frees on delete / hot reload / project close.
//
// Covers the three pieces that carry the guarantee without a GPU:
//   - ShowMeshStore: (id, revision) lookup, release, payload validation
//   - collectShowMeshProvisions + buildSceneSnapshot: the editor-side bake
//     assigns a revision, provisions it, and names it in the snapshot
//   - the wire: ProvisionMesh / ReleaseMesh / ScreenSnapshot::meshRevision

#include <gtest/gtest.h>
#include <entt/entt.hpp>

#include "entity/bus/Message.hpp"
#include "entity/bus/Serialization.hpp"
#include "entity/components/Model.hpp"
#include "entity/components/Screen.hpp"
#include "entity/director/PlaybackTimeAuthority.hpp"
#include "entity/director/ShowMeshProvisioning.hpp"
#include "entity/media/ObjLoader.hpp"
#include "entity/render/ShowMeshStore.hpp"
#include "entity/timeline/Timeline.hpp"

#include <string>

using namespace entity;

namespace {

bus::ProvisionMesh quadMesh(std::uint64_t id, std::uint32_t rev) {
    return makeProvisionMesh(id, rev, createDefaultScreenMesh());
}

const bus::ScreenSnapshot* findScreen(const bus::SceneSnapshot& snap, entt::entity e) {
    for (const auto& s : snap.screens)
        if (s.entity == static_cast<std::uint64_t>(e)) return &s;
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// ShowMeshStore

TEST(ShowMeshStore, FindsExactRevisionOnly) {
    ShowMeshStore store;
    ASSERT_TRUE(store.provision(quadMesh(7, 3)));
    const auto* m = store.find(7, 3);
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->vertexCount(), 4u);
    EXPECT_EQ(m->indices.size(), 6u);
    EXPECT_EQ(m->texCoords.size(), 8u);

    EXPECT_EQ(store.find(7, 2), nullptr) << "older revision (stale snapshot) must miss";
    EXPECT_EQ(store.find(7, 4), nullptr) << "newer revision must miss";
    EXPECT_EQ(store.find(8, 3), nullptr) << "other id must miss";
    EXPECT_EQ(store.find(7, 0), nullptr) << "revision 0 means no mesh";

    // Entity index 0 is a real entity (often the first Model created).
    ASSERT_TRUE(store.provision(quadMesh(0, 5)));
    EXPECT_NE(store.find(0, 5), nullptr);
}

TEST(ShowMeshStore, HotReloadReplacesRevision) {
    ShowMeshStore store;
    ASSERT_TRUE(store.provision(quadMesh(7, 3)));
    ASSERT_TRUE(store.provision(quadMesh(7, 9)));
    EXPECT_EQ(store.size(), 1u);
    EXPECT_EQ(store.find(7, 3), nullptr) << "snapshot baked before the reload skips";
    EXPECT_NE(store.find(7, 9), nullptr);
}

TEST(ShowMeshStore, ReleaseDropsMesh) {
    ShowMeshStore store;
    ASSERT_TRUE(store.provision(quadMesh(7, 3)));
    ASSERT_TRUE(store.provision(quadMesh(8, 4)));
    store.release(7);
    EXPECT_EQ(store.find(7, 3), nullptr) << "snapshot naming a deleted model skips";
    EXPECT_NE(store.find(8, 4), nullptr);
    EXPECT_EQ(store.size(), 1u);
    store.release(12345);  // unknown id is a no-op
    EXPECT_EQ(store.size(), 1u);
}

TEST(ShowMeshStore, RejectsMalformedPayloads) {
    auto expectRejected = [](bus::ProvisionMesh pm, const char* why) {
        ShowMeshStore store;
        EXPECT_FALSE(store.provision(std::move(pm))) << why;
        EXPECT_EQ(store.size(), 0u) << why;
    };
    {
        auto pm = quadMesh(7, 3);
        pm.indices[4] = 4;  // == vertexCount
        expectRejected(std::move(pm), "index out of range");
    }
    {
        auto pm = quadMesh(7, 3);
        pm.indices.pop_back();
        expectRejected(std::move(pm), "partial triangle");
    }
    {
        auto pm = quadMesh(7, 3);
        pm.positions.pop_back();
        expectRejected(std::move(pm), "positions not a multiple of 3");
    }
    {
        auto pm = quadMesh(7, 3);
        pm.texCoords.pop_back();
        expectRejected(std::move(pm), "texCoords count mismatch");
    }
    expectRejected(quadMesh(7, 0), "revision 0");
    {
        bus::ProvisionMesh pm;
        pm.meshId = 7;
        pm.revision = 3;
        expectRejected(std::move(pm), "empty mesh");
    }
}

TEST(ShowMeshStore, RejectedReloadDropsOlderRevision) {
    ShowMeshStore store;
    ASSERT_TRUE(store.provision(quadMesh(7, 3)));
    auto bad = quadMesh(7, 4);
    bad.indices[0] = 99;
    EXPECT_FALSE(store.provision(std::move(bad)));
    EXPECT_EQ(store.find(7, 3), nullptr);
    EXPECT_EQ(store.size(), 0u);
}

// ---------------------------------------------------------------------------
// Editor-side provisioning + snapshot bake

class ShowMeshBakeTest : public ::testing::Test {
protected:
    entt::registry        registry;
    Timeline              timeline{registry};
    PlaybackTimeAuthority auth{registry, &timeline};
    std::uint32_t         nextRevision{1};

    entt::entity addModel(const std::string& name, MeshData mesh) {
        const entt::entity e = registry.create();
        auto& m = registry.emplace<Model>(e);
        m.name = name;
        m.mesh = std::move(mesh);
        return e;
    }

    entt::entity addScreen(entt::entity model) {
        const entt::entity e = registry.create();
        auto& s = registry.emplace<Screen>(e);
        s.name = "Screen";
        s.modelEntity = model;
        return e;
    }

    std::vector<bus::ProvisionMesh> collect() {
        std::vector<bus::ProvisionMesh> out;
        collectShowMeshProvisions(registry, nextRevision, out);
        return out;
    }

    bus::SceneSnapshot bake() {
        bus::SceneSnapshot snap;
        auth.buildSceneSnapshot(snap);
        return snap;
    }
};

TEST_F(ShowMeshBakeTest, ProvisionsReferencedModelOnceAndBakesRevision) {
    const auto model  = addModel("Plane", createDefaultScreenMesh());
    const auto screen = addScreen(model);

    auto first = collect();
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first[0].meshId, static_cast<std::uint64_t>(model));
    EXPECT_NE(first[0].revision, 0u);
    EXPECT_EQ(first[0].positions.size(), 12u);
    EXPECT_EQ(first[0].texCoords.size(), 8u);
    EXPECT_EQ(first[0].indices.size(), 6u);
    EXPECT_EQ(registry.get<Model>(model).showMeshRevision, first[0].revision);

    EXPECT_TRUE(collect().empty()) << "unchanged geometry is not re-sent";

    const auto snap = bake();
    const auto* ss = findScreen(snap, screen);
    ASSERT_NE(ss, nullptr);
    EXPECT_EQ(ss->modelEntity, static_cast<std::uint64_t>(model));
    EXPECT_EQ(ss->meshRevision, first[0].revision);

    // The store built from the provision resolves exactly what the snapshot names.
    ShowMeshStore store;
    ASSERT_TRUE(store.provision(std::move(first[0])));
    EXPECT_NE(store.find(ss->modelEntity, ss->meshRevision), nullptr);
}

TEST_F(ShowMeshBakeTest, HotReloadReprovisionsUnderFreshRevision) {
    const auto model  = addModel("Plane", createDefaultScreenMesh());
    const auto screen = addScreen(model);
    auto first = collect();
    ASSERT_EQ(first.size(), 1u);
    const auto staleSnap = bake();

    // What Engine::replaceModelMesh does to the component.
    registry.get<Model>(model).mesh = createDefaultScreenMesh();
    registry.get<Model>(model).showMeshRevision = 0;

    auto second = collect();
    ASSERT_EQ(second.size(), 1u);
    EXPECT_GT(second[0].revision, first[0].revision);
    const auto freshSnap = bake();

    ShowMeshStore store;
    ASSERT_TRUE(store.provision(std::move(first[0])));
    ASSERT_TRUE(store.provision(std::move(second[0])));
    const auto* stale = findScreen(staleSnap, screen);
    const auto* fresh = findScreen(freshSnap, screen);
    ASSERT_NE(stale, nullptr);
    ASSERT_NE(fresh, nullptr);
    EXPECT_EQ(store.find(stale->modelEntity, stale->meshRevision), nullptr)
        << "a snapshot baked before the reload must not draw the new geometry";
    EXPECT_NE(store.find(fresh->modelEntity, fresh->meshRevision), nullptr);
}

TEST_F(ShowMeshBakeTest, SkipsUnreferencedAndInvalidModels) {
    addModel("Unused", createDefaultScreenMesh());
    const auto empty  = addModel("Empty", MeshData{});
    const auto screen = addScreen(empty);

    EXPECT_TRUE(collect().empty());
    const auto snap = bake();
    const auto* ss = findScreen(snap, screen);
    ASSERT_NE(ss, nullptr);
    EXPECT_EQ(ss->meshRevision, 0u) << "no valid mesh: flat-quad path";
}

TEST_F(ShowMeshBakeTest, DeletedModelBakesNoRevision) {
    const auto model  = addModel("Plane", createDefaultScreenMesh());
    const auto screen = addScreen(model);
    ASSERT_EQ(collect().size(), 1u);
    registry.destroy(model);

    const auto snap = bake();
    const auto* ss = findScreen(snap, screen);
    ASSERT_NE(ss, nullptr);
    EXPECT_EQ(ss->meshRevision, 0u);
}

TEST_F(ShowMeshBakeTest, RevisionCounterSkipsZero) {
    const auto model = addModel("Plane", createDefaultScreenMesh());
    addScreen(model);
    nextRevision = 0;  // wrapped
    auto out = collect();
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].revision, 1u);
}

// ---------------------------------------------------------------------------
// Wire format

TEST(ShowMeshWire, ProvisionMeshRoundTrip) {
    bus::Message msg = quadMesh(42, 5);
    const auto bytes = bus::serialize(msg);
    const auto decoded = bus::deserialize(bytes);
    ASSERT_TRUE(decoded.has_value());
    ASSERT_TRUE(std::holds_alternative<bus::ProvisionMesh>(*decoded));
    const auto& pm = std::get<bus::ProvisionMesh>(*decoded);
    const auto& orig = std::get<bus::ProvisionMesh>(msg);
    EXPECT_EQ(pm.meshId, 42u);
    EXPECT_EQ(pm.revision, 5u);
    EXPECT_EQ(pm.positions, orig.positions);
    EXPECT_EQ(pm.texCoords, orig.texCoords);
    EXPECT_EQ(pm.indices, orig.indices);
    EXPECT_EQ(bus::serialize(*decoded), bytes) << "byte-stable";
    EXPECT_STREQ(bus::messageTypeName(msg), "ProvisionMesh");
}

TEST(ShowMeshWire, ReleaseMeshRoundTrip) {
    bus::Message msg = bus::ReleaseMesh{77};
    const auto decoded = bus::deserialize(bus::serialize(msg));
    ASSERT_TRUE(decoded.has_value());
    ASSERT_TRUE(std::holds_alternative<bus::ReleaseMesh>(*decoded));
    EXPECT_EQ(std::get<bus::ReleaseMesh>(*decoded).meshId, 77u);
    EXPECT_STREQ(bus::messageTypeName(msg), "ReleaseMesh");
}

TEST(ShowMeshWire, ScreenSnapshotMeshRevisionRoundTripAndLegacyDefault) {
    bus::SceneSnapshot snap;
    bus::ScreenSnapshot s;
    s.entity = 3;
    s.modelEntity = 9;
    s.meshRevision = 11;
    snap.screens.push_back(s);
    const auto decoded = bus::deserialize(bus::serialize(bus::Message{snap}));
    ASSERT_TRUE(decoded.has_value());
    const auto& out = std::get<bus::SceneSnapshot>(*decoded);
    ASSERT_EQ(out.screens.size(), 1u);
    EXPECT_EQ(out.screens[0].meshRevision, 11u);

    // A payload from before the field existed decodes as "no mesh".
    const auto bytes = bus::serialize(bus::Message{snap});
    std::string text(bytes.begin(), bytes.end());
    const std::string key = ",\"meshRevision\":11";
    const auto pos = text.find(key);
    ASSERT_NE(pos, std::string::npos);
    text.erase(pos, key.size());
    const auto legacy = bus::deserialize(std::vector<std::uint8_t>(text.begin(), text.end()));
    ASSERT_TRUE(legacy.has_value());
    EXPECT_EQ(std::get<bus::SceneSnapshot>(*legacy).screens[0].meshRevision, 0u);
}
