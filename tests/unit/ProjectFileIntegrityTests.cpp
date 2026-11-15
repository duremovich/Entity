/**
 * Show-file integrity tests (code review 2026-11-14, section 3).
 *
 * The per-field ProjectSerializer tests were written one field at a time,
 * which is how a never-persisted field slipped through. These cover the
 * failure paths and identity rules around save/load:
 *   - a bad file is rejected by ProjectSerializer::validate before any
 *     live state is touched (failed File > Open);
 *   - NoticeCenter queue semantics (how failures reach the UI).
 */

#include <gtest/gtest.h>

#include "entity/components/Clip.hpp"
#include "entity/components/Screen.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/project/ProjectManager.hpp"
#include "entity/project/ProjectSerializer.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/ui/NoticeCenter.hpp"

#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path makeTempPath(const char* tag) {
    auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path()
         / ("entity_integrity_test_" + std::string(tag) + "_" + std::to_string(stamp) + ".entity");
}

struct TempFile {
    fs::path path;
    explicit TempFile(const char* tag) : path(makeTempPath(tag)) {}
    ~TempFile() {
        std::error_code ec;
        fs::remove(path, ec);
    }
    void write(const std::string& body) const {
        std::ofstream out(path, std::ios::binary);
        out << body;
    }
};

}  // namespace

// --- ProjectSerializer::validate (failed File > Open) ------------------------

TEST(ProjectFileValidate, AcceptsAFileTheSerializerWrote) {
    TempFile tf("validate_ok");
    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        timeline.createTrack("Track 0");
        auto e = registry.create();
        registry.emplace<entity::Screen>(e).name = "Front";
        ASSERT_TRUE(entity::ProjectSerializer::save(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();
    }
    EXPECT_TRUE(entity::ProjectSerializer::validate(tf.path))
        << entity::ProjectSerializer::getLastError();
}

TEST(ProjectFileValidate, RejectsMissingFile) {
    TempFile tf("validate_missing");
    EXPECT_FALSE(entity::ProjectSerializer::validate(tf.path));
    EXPECT_NE(entity::ProjectSerializer::getLastError().find("not found"), std::string::npos);
}

TEST(ProjectFileValidate, RejectsTruncatedJson) {
    TempFile tf("validate_truncated");
    tf.write(R"({"format": "entity_project", "version": 29, "screens": [{"name": "Tr)");
    EXPECT_FALSE(entity::ProjectSerializer::validate(tf.path));
}

TEST(ProjectFileValidate, RejectsNonEntityJson) {
    TempFile tf("validate_wrong_format");
    tf.write(R"({"some": "other app"})");
    EXPECT_FALSE(entity::ProjectSerializer::validate(tf.path));
    tf.write(R"([1, 2, 3])");
    EXPECT_FALSE(entity::ProjectSerializer::validate(tf.path));
}

TEST(ProjectFileValidate, RejectsNewerVersion) {
    TempFile tf("validate_newer");
    tf.write(R"({"format": "entity_project", "version": 99999})");
    EXPECT_FALSE(entity::ProjectSerializer::validate(tf.path));
    EXPECT_NE(entity::ProjectSerializer::getLastError().find("newer"), std::string::npos);
}

TEST(ProjectFileValidate, RejectsValueThatThrowsMidParse) {
    // Passes the header checks; the null in "position" throws inside the
    // screen loop — after timeline.clear() in a live load. validate() must
    // catch it so ProjectManager::load never gets that far.
    TempFile tf("validate_midparse");
    tf.write(R"({
      "format": "entity_project", "version": 29,
      "screens": [{"name": "Broken", "position": [null, 0.0, 0.0]}],
      "tracks": []
    })");
    EXPECT_FALSE(entity::ProjectSerializer::validate(tf.path));
}

TEST(ProjectFileValidate, RejectsMidParseFailureInTracks) {
    TempFile tf("validate_midparse_tracks");
    tf.write(R"({
      "format": "entity_project", "version": 29,
      "timeline": {"duration": "not a number"},
      "tracks": []
    })");
    EXPECT_FALSE(entity::ProjectSerializer::validate(tf.path));
}

TEST(ProjectFileValidate, LeavesLiveStateUntouched) {
    // validate() works on scratch state: the caller's timeline, screens and
    // ProjectManager library are not involved even when the file is good.
    TempFile good("validate_untouched_good");
    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        ASSERT_TRUE(entity::ProjectSerializer::save(timeline, good.path));
    }

    entt::registry live;
    entity::Timeline liveTimeline(live);
    liveTimeline.createTrack("Track 0");
    auto s = live.create();
    live.emplace<entity::Screen>(s).name = "Live Screen";

    EXPECT_TRUE(entity::ProjectSerializer::validate(good.path));
    EXPECT_EQ(liveTimeline.getTracks().size(), 1u);
    EXPECT_EQ(live.view<entity::Screen>().size(), 1u);
}

// --- NoticeCenter ------------------------------------------------------------

TEST(NoticeCenter, PostQueuesInOrder) {
    entity::NoticeCenter nc;
    nc.post(entity::NoticeCenter::Severity::Error, entity::NoticeCenter::Presentation::Modal,
            "A", "first");
    nc.post(entity::NoticeCenter::Severity::Warning, entity::NoticeCenter::Presentation::Toast,
            "B", "second");
    auto snap = nc.snapshot();
    ASSERT_EQ(snap.size(), 2u);
    EXPECT_EQ(snap[0].title, "A");
    EXPECT_EQ(snap[1].title, "B");
    EXPECT_EQ(snap[1].presentation, entity::NoticeCenter::Presentation::Toast);
}

TEST(NoticeCenter, KeyedPostReplacesInsteadOfStacking) {
    entity::NoticeCenter nc;
    for (int i = 0; i < 5; ++i) {
        nc.post(entity::NoticeCenter::Severity::Warning,
                entity::NoticeCenter::Presentation::Toast,
                "Autosave failed", "attempt " + std::to_string(i), "autosave");
    }
    ASSERT_EQ(nc.count(), 1u);
    EXPECT_EQ(nc.snapshot()[0].message, "attempt 4");
    EXPECT_TRUE(nc.hasKey("autosave"));
    nc.dismissKey("autosave");
    EXPECT_EQ(nc.count(), 0u);
    EXPECT_FALSE(nc.hasKey("autosave"));
}

TEST(NoticeCenter, DismissById) {
    entity::NoticeCenter nc;
    auto a = nc.post(entity::NoticeCenter::Severity::Error,
                     entity::NoticeCenter::Presentation::Modal, "A", "a");
    nc.post(entity::NoticeCenter::Severity::Error,
            entity::NoticeCenter::Presentation::Modal, "B", "b");
    nc.dismiss(a);
    auto snap = nc.snapshot();
    ASSERT_EQ(snap.size(), 1u);
    EXPECT_EQ(snap[0].title, "B");
}
