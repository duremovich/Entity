/**
 * Unit tests for entity::RecentProjects.
 *
 * The launcher's "Recent" panel reads from this list, and the launcher
 * must be robust to missing/corrupt state. So the tests cover not just
 * the happy path (touch / save / load round-trip) but the edge cases
 * the launcher will encounter on real users' machines: a missing file,
 * a malformed file, and a list that's grown past the cap.
 */

#include <gtest/gtest.h>

#include "entity/project/RecentProjects.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

fs::path makeTempJson(const char* tag) {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path()
         / ("entity_recent_projects_test_" + std::string(tag) + "_"
            + std::to_string(stamp) + ".json");
}

struct TempJson {
    fs::path path;
    explicit TempJson(const char* tag) : path(makeTempJson(tag)) {}
    ~TempJson() {
        std::error_code ec;
        fs::remove(path, ec);
    }
};

}  // namespace

TEST(RecentProjects, TouchPutsNewestFirst) {
    entity::RecentProjects rp;
    rp.touch("C:/Entity/A/A.entity");
    rp.touch("C:/Entity/B/B.entity");
    rp.touch("C:/Entity/C/C.entity");

    ASSERT_EQ(rp.size(), 3u);
    EXPECT_EQ(rp.entries()[0].path, "C:/Entity/C/C.entity");
    EXPECT_EQ(rp.entries()[1].path, "C:/Entity/B/B.entity");
    EXPECT_EQ(rp.entries()[2].path, "C:/Entity/A/A.entity");
}

TEST(RecentProjects, TouchDedupsExistingEntry) {
    entity::RecentProjects rp;
    rp.touch("C:/Entity/A/A.entity");
    rp.touch("C:/Entity/B/B.entity");
    rp.touch("C:/Entity/A/A.entity");  // re-touch A — should move to front, not duplicate

    ASSERT_EQ(rp.size(), 2u);
    EXPECT_EQ(rp.entries()[0].path, "C:/Entity/A/A.entity");
    EXPECT_EQ(rp.entries()[1].path, "C:/Entity/B/B.entity");
}

TEST(RecentProjects, TouchCapsAtMaxEntries) {
    entity::RecentProjects rp;
    for (std::size_t i = 0; i < entity::RecentProjects::kMaxEntries + 5; ++i) {
        rp.touch("C:/Entity/" + std::to_string(i) + ".entity");
    }
    EXPECT_EQ(rp.size(), entity::RecentProjects::kMaxEntries);
    // The newest is the highest-numbered one.
    EXPECT_EQ(rp.entries()[0].path,
              "C:/Entity/" + std::to_string(entity::RecentProjects::kMaxEntries + 4) + ".entity");
}

TEST(RecentProjects, TouchRejectsEmptyPath) {
    entity::RecentProjects rp;
    rp.touch("");
    EXPECT_TRUE(rp.empty());
}

TEST(RecentProjects, RemoveDropsMatchingEntry) {
    entity::RecentProjects rp;
    rp.touch("C:/A.entity");
    rp.touch("C:/B.entity");
    rp.remove("C:/A.entity");
    ASSERT_EQ(rp.size(), 1u);
    EXPECT_EQ(rp.entries()[0].path, "C:/B.entity");
}

TEST(RecentProjects, SaveLoadRoundTrip) {
    TempJson tmp("roundtrip");

    {
        entity::RecentProjects rp;
        rp.touch("C:/Entity/Show1/Show1.entity");
        rp.touch("C:/Entity/Show2/Show2.entity");
        ASSERT_TRUE(rp.saveTo(tmp.path));
    }

    entity::RecentProjects rp2;
    rp2.loadFrom(tmp.path);
    ASSERT_EQ(rp2.size(), 2u);
    EXPECT_EQ(rp2.entries()[0].path, "C:/Entity/Show2/Show2.entity");
    EXPECT_EQ(rp2.entries()[1].path, "C:/Entity/Show1/Show1.entity");
    EXPECT_GT(rp2.entries()[0].lastOpenedUnixSeconds, 0);
}

TEST(RecentProjects, LoadFromMissingFileIsEmpty) {
    entity::RecentProjects rp;
    rp.loadFrom("C:/this/path/does/not/exist/recent.json");
    EXPECT_TRUE(rp.empty());
}

TEST(RecentProjects, LoadFromCorruptFileIsEmpty) {
    TempJson tmp("corrupt");
    {
        std::ofstream out(tmp.path);
        out << "this is not valid json {";
    }
    entity::RecentProjects rp;
    rp.loadFrom(tmp.path);
    EXPECT_TRUE(rp.empty());
}

TEST(RecentProjects, LoadIgnoresEntriesPastCap) {
    TempJson tmp("cap");
    {
        // Hand-craft a JSON file with more than kMaxEntries — the loader
        // should stop after kMaxEntries, the same way touch() does.
        std::ofstream out(tmp.path);
        out << "[";
        for (std::size_t i = 0; i < entity::RecentProjects::kMaxEntries + 5; ++i) {
            if (i) out << ",";
            out << "{\"path\":\"C:/E/" << i << ".entity\","
                << "\"lastOpenedUnixSeconds\":" << (1000 + i) << "}";
        }
        out << "]";
    }
    entity::RecentProjects rp;
    rp.loadFrom(tmp.path);
    EXPECT_EQ(rp.size(), entity::RecentProjects::kMaxEntries);
}

TEST(RecentProjects, StoragePathIsPlatformSpecific) {
    auto p = entity::RecentProjects::storagePath();
    // We don't assert the exact prefix (varies per machine), only the leaf
    // shape: <root>/Entity/recent_projects.json.
    ASSERT_FALSE(p.empty());
    EXPECT_EQ(p.filename(), fs::path("recent_projects.json"));
    EXPECT_EQ(p.parent_path().filename(), fs::path("Entity"));
}
