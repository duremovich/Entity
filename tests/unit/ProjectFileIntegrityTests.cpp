/**
 * Show-file integrity tests (code review 2026-11-14, section 3).
 *
 * The per-field ProjectSerializer tests were written one field at a time,
 * which is how a never-persisted field slipped through. These cover the
 * failure paths and identity rules around save/load:
 *   - a bad file is rejected by ProjectSerializer::validate before any
 *     live state is touched (failed File > Open);
 *   - NoticeCenter queue semantics (how failures reach the UI);
 *   - save failures are reported (lastSaveError / AutosaveResult) and
 *     network text cannot make the project unsaveable.
 */

#include <gtest/gtest.h>

#include "entity/components/Clip.hpp"
#include "entity/components/Screen.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/project/ProjectManager.hpp"
#include "entity/project/ProjectSerializer.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/core/Utf8Sanitize.hpp"
#include "entity/remote/RemoteControlStore.hpp"
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

// --- Save failures + UTF-8 sanitizing ----------------------------------------

TEST(Utf8Sanitize, ValidTextIsUnchanged) {
    const std::string s = "B\xC3\xBChne \xE2\x80\x93 \xE8\x88\x9E\xE5\x8F\xB0 \xF0\x9F\x8E\xAD plain";
    EXPECT_TRUE(entity::isValidUtf8(s));
    EXPECT_EQ(entity::sanitizeUtf8(s), s);
    EXPECT_EQ(entity::sanitizeUtf8(""), "");
}

TEST(Utf8Sanitize, InvalidBytesBecomeReplacementCharacters) {
    const std::string fffd = "\xEF\xBF\xBD";
    // Latin-1 'ü' (0xFC) as a lone byte — the OSC case from the review.
    EXPECT_FALSE(entity::isValidUtf8("B\xFChne"));
    EXPECT_EQ(entity::sanitizeUtf8("B\xFChne"), "B" + fffd + "hne");
    // Truncated 3-byte sequence at end of input.
    EXPECT_EQ(entity::sanitizeUtf8("ab\xE2\x82"), "ab" + fffd);
    // Overlong encoding of '/' and a UTF-16 surrogate are both ill-formed.
    EXPECT_FALSE(entity::isValidUtf8("\xC0\xAF"));
    EXPECT_FALSE(entity::isValidUtf8("\xED\xA0\x80"));
    EXPECT_TRUE(entity::isValidUtf8(entity::sanitizeUtf8("\xC0\xAF\xED\xA0\x80\xFF")));
    // Stray continuation byte.
    EXPECT_EQ(entity::sanitizeUtf8("x\x80y"), "x" + fffd + "y");
}

TEST(RemoteControlStoreText, NetworkTextIsSanitizedOnEntry) {
    // TextSystem copies engaged remote text into TextLayerState, which the
    // serializer writes; a raw 0xFC made every save throw type_error.316.
    entity::remote::RemoteControlStore store;
    const int slot = store.allocateSlot("lowerthird");
    ASSERT_GE(slot, 0);
    ASSERT_TRUE(store.setTextById("lowerthird", "B\xFChne"));
    std::uint32_t seen = 0;
    auto txt = store.consumeText(slot, seen);
    ASSERT_TRUE(txt.has_value());
    EXPECT_TRUE(entity::isValidUtf8(*txt));
    EXPECT_EQ(*txt, "B\xEF\xBF\xBDhne");
}

TEST(ProjectSaveFailure, InvalidUtf8InProjectStateFailsWithAReason) {
    // Documents the failure mode the sanitizer prevents: one bad byte
    // anywhere in project state fails the whole save, with an error the
    // caller can show. The previous file on disk is not touched.
    TempFile tf("save_bad_utf8");
    tf.write("previous good content");
    entt::registry registry;
    entity::Timeline timeline(registry);
    auto e = registry.create();
    registry.emplace<entity::Screen>(e).name = "B\xFChne";
    EXPECT_FALSE(entity::ProjectSerializer::save(timeline, tf.path));
    EXPECT_FALSE(entity::ProjectSerializer::getLastError().empty());
    {
        std::ifstream in(tf.path, std::ios::binary);
        std::string body{std::istreambuf_iterator<char>(in), {}};
        EXPECT_EQ(body, "previous good content");
    }

    registry.get<entity::Screen>(e).name = entity::sanitizeUtf8("B\xFChne");
    EXPECT_TRUE(entity::ProjectSerializer::save(timeline, tf.path))
        << entity::ProjectSerializer::getLastError();
}

TEST(ProjectSaveFailure, ManagerSaveReportsReasonAndKeepsPath) {
    TempFile good("pm_save_good");
    entt::registry registry;
    entity::Timeline timeline(registry);
    entity::ProjectManager pm;
    pm.initialize(&timeline, &registry, nullptr);
    ASSERT_TRUE(pm.save(good.path)) << pm.lastSaveError();
    EXPECT_TRUE(pm.lastSaveError().empty());
    const fs::path savedPath = pm.projectPath();

    auto e = registry.create();
    registry.emplace<entity::Screen>(e).name = "B\xFChne";
    EXPECT_FALSE(pm.save());
    EXPECT_FALSE(pm.lastSaveError().empty());
    EXPECT_EQ(pm.projectPath(), savedPath);
}

TEST(ProjectSaveFailure, AutosaveReportsFailureThenRecovery) {
    entt::registry registry;
    entity::Timeline timeline(registry);
    entity::ProjectManager pm;
    pm.initialize(&timeline, &registry, nullptr);
    pm.setAutosaveInterval(1.0);

    // Project path inside a folder that does not exist: the autosave
    // write cannot succeed.
    const fs::path missingDir = makeTempPath("autosave_missing_dir");
    pm.setProjectPath(missingDir / "Show.entity");
    using R = entity::ProjectManager::AutosaveResult;
    EXPECT_EQ(pm.tickAutosave(0.5), R::NotDue);
    EXPECT_EQ(pm.tickAutosave(0.6), R::Failed);
    EXPECT_FALSE(pm.lastSaveError().empty());

    TempFile project("autosave_ok");
    pm.setProjectPath(project.path);
    EXPECT_EQ(pm.tickAutosave(1.0), R::Saved);
    fs::path autosave = project.path;
    autosave += ".autosave.entity";
    EXPECT_TRUE(fs::exists(autosave));
    std::error_code ec;
    fs::remove(autosave, ec);
}
