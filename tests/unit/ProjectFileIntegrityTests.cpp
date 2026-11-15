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
 *     network text cannot make the project unsaveable;
 *   - OutputDisplay::sourceProjector round-trips, and the calibration
 *     window's temporary route neither leaks into the file nor loses the
 *     output's own routing;
 *   - same-named Screens/Props survive a reload with their own settings,
 *     Screen::type round-trips, and new names are unique.
 */

#include <gtest/gtest.h>

#include "entity/components/Clip.hpp"
#include "entity/components/OutputDisplay.hpp"
#include "entity/components/Projector.hpp"
#include "entity/components/Prop.hpp"
#include "entity/core/UniqueName.hpp"
#include "entity/components/Screen.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/project/ProjectManager.hpp"
#include "entity/project/ProjectSerializer.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/core/Utf8Sanitize.hpp"
#include "entity/remote/RemoteControlStore.hpp"
#include "entity/ui/NoticeCenter.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

// --- OutputDisplay::sourceProjector (v30) -------------------------------------

namespace {

entt::entity findOutput(entt::registry& reg, const std::string& name) {
    for (auto [e, o] : reg.view<entity::OutputDisplay>().each())
        if (o.name == name) return e;
    return entt::null;
}

std::string projectorName(entt::registry& reg, entt::entity e) {
    if (!reg.valid(e) || !reg.all_of<entity::Projector>(e)) return "<none>";
    return reg.get<entity::Projector>(e).name;
}

std::string screenName(entt::registry& reg, entt::entity e) {
    if (!reg.valid(e) || !reg.all_of<entity::Screen>(e)) return "<none>";
    return reg.get<entity::Screen>(e).name;
}

}  // namespace

TEST(OutputSourceProjector, RoundTripsByProjectorName) {
    TempFile tf("source_projector_roundtrip");
    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        auto s = registry.create();
        registry.emplace<entity::Screen>(s).name = "Wall";
        auto p1 = registry.create();
        registry.emplace<entity::Projector>(p1).name = "Projector 1";
        auto p2 = registry.create();
        registry.emplace<entity::Projector>(p2).name = "Projector 2";

        auto o1 = registry.create();
        auto& out1 = registry.emplace<entity::OutputDisplay>(o1);
        out1.name = "Beamer A";
        out1.sourceProjector = p2;          // picked in the Outputs window
        auto o2 = registry.create();
        auto& out2 = registry.emplace<entity::OutputDisplay>(o2);
        out2.name = "LED feed";
        out2.sourceScreen = s;
        ASSERT_TRUE(entity::ProjectSerializer::save(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();
    }
    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();
        auto o1 = findOutput(registry, "Beamer A");
        auto o2 = findOutput(registry, "LED feed");
        ASSERT_TRUE(registry.valid(o1));
        ASSERT_TRUE(registry.valid(o2));
        const auto& out1 = registry.get<entity::OutputDisplay>(o1);
        const auto& out2 = registry.get<entity::OutputDisplay>(o2);
        EXPECT_EQ(projectorName(registry, out1.sourceProjector), "Projector 2");
        EXPECT_FALSE(registry.valid(out1.sourceScreen));
        EXPECT_FALSE(registry.valid(out2.sourceProjector));
        EXPECT_EQ(screenName(registry, out2.sourceScreen), "Wall");
    }
}

TEST(OutputSourceProjector, PreV30FileLoadsWithoutProjectorSource) {
    TempFile tf("source_projector_v29");
    tf.write(R"({
      "format": "entity_project", "version": 29,
      "outputs": [{"name": "Out", "sourceScreenName": ""}],
      "projectors": [{"name": "Projector 1", "linkedOutputName": "Out"}]
    })");
    entt::registry registry;
    entity::Timeline timeline(registry);
    ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path))
        << entity::ProjectSerializer::getLastError();
    auto o = findOutput(registry, "Out");
    ASSERT_TRUE(registry.valid(o));
    EXPECT_FALSE(registry.valid(registry.get<entity::OutputDisplay>(o).sourceProjector));
}

TEST(CalibrationRoute, UnrouteRestoresProjectorSourcedOutput) {
    // ui.md X-1: an output already sourced from a projector lost it when
    // the calibration window closed (only the screen was saved, and
    // sourceProjector was nulled unconditionally).
    entt::registry reg;
    auto p1 = reg.create(); reg.emplace<entity::Projector>(p1).name = "P1";
    auto p2 = reg.create(); reg.emplace<entity::Projector>(p2).name = "P2";
    entity::OutputDisplay od;
    od.sourceProjector = p1;

    entity::beginCalibrationRoute(od, p2);
    EXPECT_EQ(projectorName(reg, od.sourceProjector), "P2");
    EXPECT_EQ(projectorName(reg, entity::userSourceProjector(od)), "P1");

    entity::endCalibrationRoute(od);
    EXPECT_EQ(projectorName(reg, od.sourceProjector), "P1");
    EXPECT_FALSE(reg.valid(od.sourceScreen));
    EXPECT_FALSE(od.calibrationRouteBackup.active);
    EXPECT_FALSE(od.calibrationOverlay.enabled);
}

TEST(CalibrationRoute, UnrouteRestoresScreenSourcedOutputAndRerouteKeepsFirstBackup) {
    entt::registry reg;
    auto s = reg.create(); reg.emplace<entity::Screen>(s).name = "Wall";
    auto p1 = reg.create(); reg.emplace<entity::Projector>(p1).name = "P1";
    auto p2 = reg.create(); reg.emplace<entity::Projector>(p2).name = "P2";
    entity::OutputDisplay od;
    od.sourceScreen = s;

    entity::beginCalibrationRoute(od, p1);
    entity::beginCalibrationRoute(od, p2);   // re-route without unroute
    EXPECT_EQ(screenName(reg, entity::userSourceScreen(od)), "Wall");
    entity::endCalibrationRoute(od);
    EXPECT_EQ(screenName(reg, od.sourceScreen), "Wall");
    EXPECT_FALSE(reg.valid(od.sourceProjector));

    // A second unroute (window X after "Route to: Off") changes nothing.
    entity::endCalibrationRoute(od);
    EXPECT_EQ(screenName(reg, od.sourceScreen), "Wall");
}

TEST(CalibrationRoute, SaveWhileRoutedWritesTheUsersRouting) {
    TempFile tf("source_projector_routed_save");
    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        auto s = registry.create();
        registry.emplace<entity::Screen>(s).name = "Wall";
        auto pUser = registry.create();
        registry.emplace<entity::Projector>(pUser).name = "Users Projector";
        auto pCal = registry.create();
        registry.emplace<entity::Projector>(pCal).name = "Being Calibrated";

        auto oa = registry.create();
        registry.emplace<entity::OutputDisplay>(oa).name = "Screen-sourced";
        registry.get<entity::OutputDisplay>(oa).sourceScreen = s;
        auto ob = registry.create();
        registry.emplace<entity::OutputDisplay>(ob).name = "Projector-sourced";
        registry.get<entity::OutputDisplay>(ob).sourceProjector = pUser;

        // Calibration window routes both (one at a time in practice; the
        // component state is what the serializer sees).
        entity::beginCalibrationRoute(registry.get<entity::OutputDisplay>(oa), pCal);
        entity::beginCalibrationRoute(registry.get<entity::OutputDisplay>(ob), pCal);
        ASSERT_TRUE(entity::ProjectSerializer::save(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();
    }
    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();
        const auto& a = registry.get<entity::OutputDisplay>(findOutput(registry, "Screen-sourced"));
        const auto& b = registry.get<entity::OutputDisplay>(findOutput(registry, "Projector-sourced"));
        EXPECT_EQ(screenName(registry, a.sourceScreen), "Wall");
        EXPECT_FALSE(registry.valid(a.sourceProjector));
        EXPECT_EQ(projectorName(registry, b.sourceProjector), "Users Projector");
        EXPECT_FALSE(registry.valid(b.sourceScreen));
    }
}

// --- Screen / Prop identity (v31) ---------------------------------------------

namespace {

std::vector<const entity::Screen*> screensNamed(entt::registry& reg, const std::string& name) {
    std::vector<const entity::Screen*> out;
    for (auto [e, s] : reg.view<entity::Screen>().each())
        if (s.name == name) out.push_back(&s);
    return out;
}

}  // namespace

TEST(ScreenIdentity, SameNamedScreensBothSurviveReloadWithOwnSettings) {
    TempFile tf("dup_screens");
    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        auto a = registry.create();
        auto& sa = registry.emplace<entity::Screen>(a);
        sa.name = "New Screen"; sa.width = 1280; sa.position = {1.0f, 0.0f, 0.0f};
        auto b = registry.create();
        auto& sb = registry.emplace<entity::Screen>(b);
        sb.name = "New Screen"; sb.width = 3840; sb.position = {-5.0f, 2.0f, 0.0f};
        sb.type = entity::ScreenType::ProjectionSurface;
        ASSERT_TRUE(entity::ProjectSerializer::save(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();
    }

    // Fresh registry with the editor's default screen (the reopen case),
    // and a registry already holding both (same-session reload).
    for (int existing : {1, 2}) {
        entt::registry registry;
        entity::Timeline timeline(registry);
        if (existing == 1) {
            registry.emplace<entity::Screen>(registry.create()).name = "Main Screen";
        } else {
            registry.emplace<entity::Screen>(registry.create()).name = "New Screen";
            registry.emplace<entity::Screen>(registry.create()).name = "New Screen";
        }
        ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path))
            << entity::ProjectSerializer::getLastError();

        EXPECT_EQ(registry.view<entity::Screen>().size(), 2u) << "existing=" << existing;
        auto dups = screensNamed(registry, "New Screen");
        ASSERT_EQ(dups.size(), 2u) << "existing=" << existing;
        std::vector<uint32_t> widths{dups[0]->width, dups[1]->width};
        std::sort(widths.begin(), widths.end());
        EXPECT_EQ(widths, (std::vector<uint32_t>{1280u, 3840u}));
        for (const auto* sc : dups) {
            if (sc->width == 3840u) {
                EXPECT_FLOAT_EQ(sc->position[0], -5.0f);
                EXPECT_EQ(sc->type, entity::ScreenType::ProjectionSurface);
            } else {
                EXPECT_FLOAT_EQ(sc->position[0], 1.0f);
                EXPECT_EQ(sc->type, entity::ScreenType::LEDWall);
            }
        }
    }
}

TEST(ScreenIdentity, ScreenTypeRoundTripsAndDefaultsForOldFiles) {
    TempFile tf("screen_type");
    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        auto e = registry.create();
        auto& s = registry.emplace<entity::Screen>(e);
        s.name = "Cyc";
        s.type = entity::ScreenType::ProjectionSurface;
        ASSERT_TRUE(entity::ProjectSerializer::save(timeline, tf.path));
    }
    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path));
        auto cyc = screensNamed(registry, "Cyc");
        ASSERT_EQ(cyc.size(), 1u);
        EXPECT_EQ(cyc[0]->type, entity::ScreenType::ProjectionSurface);
    }

    TempFile old("screen_type_v30");
    old.write(R"({"format": "entity_project", "version": 30,
                 "screens": [{"name": "Legacy"}]})");
    entt::registry registry;
    entity::Timeline timeline(registry);
    ASSERT_TRUE(entity::ProjectSerializer::load(timeline, old.path));
    auto legacy = screensNamed(registry, "Legacy");
    ASSERT_EQ(legacy.size(), 1u);
    EXPECT_EQ(legacy[0]->type, entity::ScreenType::LEDWall);
}

TEST(ScreenIdentity, ExtraExistingScreensAreStillRemoved) {
    // One-to-one matching must keep the old "destroy unmatched" behavior:
    // three live "X" screens and a file with one "X" end with one.
    TempFile tf("dup_screens_trim");
    tf.write(R"({"format": "entity_project", "version": 31,
                "screens": [{"name": "X", "width": 640}]})");
    entt::registry registry;
    entity::Timeline timeline(registry);
    for (int i = 0; i < 3; ++i)
        registry.emplace<entity::Screen>(registry.create()).name = "X";
    ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path));
    auto xs = screensNamed(registry, "X");
    ASSERT_EQ(xs.size(), 1u);
    EXPECT_EQ(xs[0]->width, 640u);
}

TEST(PropIdentity, SameNamedPropsBothSurviveReload) {
    TempFile tf("dup_props");
    {
        entt::registry registry;
        entity::Timeline timeline(registry);
        auto a = registry.create();
        auto& pa = registry.emplace<entity::Prop>(a);
        pa.name = "Prop"; pa.position = {1.0f, 0.0f, 0.0f};
        auto b = registry.create();
        auto& pb = registry.emplace<entity::Prop>(b);
        pb.name = "Prop"; pb.position = {9.0f, 0.0f, 0.0f};
        ASSERT_TRUE(entity::ProjectSerializer::save(timeline, tf.path));
    }
    entt::registry registry;
    entity::Timeline timeline(registry);
    ASSERT_TRUE(entity::ProjectSerializer::load(timeline, tf.path));
    std::vector<float> xs;
    for (auto [e, p] : registry.view<entity::Prop>().each()) xs.push_back(p.position[0]);
    std::sort(xs.begin(), xs.end());
    EXPECT_EQ(xs, (std::vector<float>{1.0f, 9.0f}));
}

TEST(UniqueName, NewNamesSkipTakenOnes) {
    entt::registry reg;
    EXPECT_EQ(entity::makeUniqueComponentName<entity::Screen>(reg, "New Screen"), "New Screen");
    reg.emplace<entity::Screen>(reg.create()).name = "New Screen";
    EXPECT_EQ(entity::makeUniqueComponentName<entity::Screen>(reg, "New Screen"), "New Screen 2");
    reg.emplace<entity::Screen>(reg.create()).name = "New Screen 2";
    reg.emplace<entity::Screen>(reg.create()).name = "Wall Copy";
    EXPECT_EQ(entity::makeUniqueComponentName<entity::Screen>(reg, "New Screen"), "New Screen 3");
    EXPECT_EQ(entity::makeUniqueComponentName<entity::Screen>(reg, "Wall Copy"), "Wall Copy 2");
    EXPECT_EQ(entity::makeUniqueComponentName<entity::Prop>(reg, "Prop"), "Prop");
}
