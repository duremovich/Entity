/**
 * Regression tests for the non-ASCII path crash family and two malformed-input
 * crashes (code review 2026-11-14, defects A, B, C).
 *
 * This file is UTF-8; non-ASCII text is written with \u escapes in u8 literals
 * so nothing depends on the compiler's source charset.
 */

#include <gtest/gtest.h>

#include "entity/core/PathUtf8.hpp"
#include "entity/effects/EffectKindRegistry.hpp"
#include "entity/media/ObjLoader.hpp"
#include "entity/project/ContentScanner.hpp"
#include "entity/project/RecentProjects.hpp"
#include "entity/render/RuntimeShaderCompiler.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

// "Andr\u00E9" (Latin-1 range) and a CJK name outside any Western ANSI page.
const std::u8string kAccent = u8"Andr\u00E9";
const std::u8string kCjk    = u8"\u6F22\u5B57\u30C6\u30B9\u30C8";

struct TempTree {
    fs::path root;  // <tmp>/entity_utf8_<tag>_<stamp>/<leaf>
    fs::path top;
    TempTree(const char* tag, const std::u8string& leaf) {
        auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        top = fs::temp_directory_path() /
              (std::string("entity_utf8_") + tag + "_" + std::to_string(stamp));
        root = top / fs::path(leaf);
        fs::create_directories(root);
    }
    ~TempTree() { std::error_code ec; fs::remove_all(top, ec); }
};

std::string asBytes(const std::u8string& s) {
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

}  // namespace

// --- A: path <-> UTF-8 round trip ------------------------------------------

TEST(Utf8Path, RoundTripAccentedAndCjk) {
    for (const auto& name : {kAccent, kCjk}) {
        fs::path p = fs::path(L"C:\\Users") / fs::path(name) / "Show.entity";
        const std::string utf8 = entity::pathToUtf8(p);
        EXPECT_NE(utf8.find(asBytes(name)), std::string::npos);
        EXPECT_EQ(entity::utf8ToPath(utf8), p);
    }
}

TEST(Utf8Path, GenericUsesForwardSlashes) {
    fs::path p = fs::path("content") / fs::path(kCjk) / "clip.mov";
    const std::string g = entity::pathToUtf8Generic(p);
    EXPECT_EQ(g.find('\\'), std::string::npos);
    EXPECT_TRUE(entity::utf8ToPath(g).generic_u8string() == p.generic_u8string());
}

TEST(Utf8Path, EmptyAndAscii) {
    EXPECT_EQ(entity::pathToUtf8(fs::path{}), "");
    EXPECT_TRUE(entity::utf8ToPath("").empty());
    EXPECT_EQ(entity::utf8ToPath("a/b/c.mov"), fs::path("a/b/c.mov"));
}

// --- A: RecentProjects with a non-ASCII path -------------------------------

TEST(Utf8Path, RecentProjectsSavesAndReloadsNonAsciiPath) {
    TempTree tmp("recent", kAccent);
    const fs::path file = tmp.root / "recent_projects.json";

    const std::string project1 = entity::pathToUtf8(tmp.root / fs::path(kCjk) / "a.entity");
    const std::string project2 = entity::pathToUtf8(tmp.root / fs::path(kAccent) / "b.entity");

    entity::RecentProjects rp;
    rp.touch(project1);
    rp.touch(project2);
    bool ok = false;
    ASSERT_NO_THROW(ok = rp.saveTo(file));
    EXPECT_TRUE(ok);

    entity::RecentProjects loaded;
    loaded.loadFrom(file);
    ASSERT_EQ(loaded.size(), 2u);
    EXPECT_EQ(loaded.entries()[0].path, project2);
    EXPECT_EQ(loaded.entries()[1].path, project1);
}

TEST(Utf8Path, RecentProjectsSurvivesInvalidUtf8Entry) {
    // A legacy ANSI byte must not make save() throw.
    TempTree tmp("recent_bad", kAccent);
    entity::RecentProjects rp;
    rp.touch(std::string("C:/Users/Andr\xE9/x.entity"));  // lone Latin-1 byte
    ASSERT_NO_THROW((void)rp.saveTo(tmp.root / "r.json"));
}

// --- A: ContentScanner with non-ASCII file names ---------------------------

TEST(Utf8Path, ContentScannerHandlesNonAsciiNamesAndRoundTrips) {
    TempTree tmp("scan", kAccent);
    const fs::path content = tmp.root / "content" / fs::path(kCjk);
    fs::create_directories(content);
    const fs::path media = content / fs::path(u8"cl\u00EDp-\u6F22.mov");
    {
        std::ofstream f(media, std::ios::binary);
        f << "0123456789abcdef";
    }

    entity::ContentScanner s;
    s.start(tmp.root);
    s.stop();
    ASSERT_NO_THROW({ s.tickForTesting(); s.tickForTesting(); });
    auto deltas = s.drain();
    ASSERT_EQ(deltas.size(), 1u);
    EXPECT_EQ(deltas[0].kind, entity::ContentScanner::DeltaKind::Added);

    // The registered relative path resolves back to the same file...
    const fs::path resolved = tmp.root / entity::utf8ToPath(deltas[0].relativePath);
    std::error_code ec;
    EXPECT_TRUE(fs::equivalent(resolved, media, ec));
    // ...and survives a strict-UTF-8 JSON dump.
    EXPECT_NO_THROW((void)nlohmann::json(deltas[0].relativePath).dump());
}

// --- B: malformed user effect manifest -------------------------------------

namespace {
constexpr const char* kHlsl = R"(
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 PSMain(PSIn i) : SV_TARGET { return float4(i.uv, 0.5f, 1.0f); }
)";

void writeText(const fs::path& p, const char* c) { std::ofstream f(p); f << c; }
}  // namespace

TEST(Utf8Path, EffectManifestWithWrongTypedFieldIsSkippedOthersLoad) {
    TempTree tmp("effects", kAccent);
    writeText(tmp.root / "ok.hlsl", kHlsl);
    writeText(tmp.root / "good.json",
              R"({"stableId":"user.good","shader":"ok.hlsl","passCount":1})");
    writeText(tmp.root / "bad_pass.json",
              R"({"stableId":"user.bad_pass","shader":"ok.hlsl","passCount":"two"})");
    writeText(tmp.root / "bad_name.json",
              R"({"stableId":"user.bad_name","shader":"ok.hlsl","displayName":42})");
    writeText(tmp.root / "bad_params.json",
              R"({"stableId":"user.bad_params","shader":"ok.hlsl","params":[{"name":"a","min":"lots"}]})");
    writeText(tmp.root / "bad_stable.json", R"({"stableId":7,"shader":"ok.hlsl"})");

    entity::effects::EffectKindRegistry reg;
    entity::RuntimeShaderCompiler compiler;
    ASSERT_NO_THROW(reg.scanUserEffects(tmp.root, compiler));
    EXPECT_NE(reg.find(entity::effects::fnv1a32("user.good")), nullptr);
    EXPECT_EQ(reg.find(entity::effects::fnv1a32("user.bad_pass")), nullptr);
    EXPECT_EQ(reg.find(entity::effects::fnv1a32("user.bad_name")), nullptr);
    EXPECT_EQ(reg.find(entity::effects::fnv1a32("user.bad_params")), nullptr);
}

// --- C: malformed OBJ index ------------------------------------------------

TEST(Utf8Path, ObjLoaderRejectsMalformedFaceIndexWithoutThrowing) {
    const std::string obj =
        "v 0 0 0\nv 1 0 0\nv 0 1 0\n"
        "f 1 2 3 # trailing comment\n";
    entity::MeshData mesh;
    ASSERT_NO_THROW(mesh = entity::ObjLoader::loadFromString(obj, "bad"));
    EXPECT_FALSE(mesh.isValid());

    const std::string huge = "v 0 0 0\nf 99999999999999999999 1 1\n";
    ASSERT_NO_THROW(mesh = entity::ObjLoader::loadFromString(huge, "huge"));
    EXPECT_FALSE(mesh.isValid());
}

TEST(Utf8Path, ObjLoaderStillLoadsValidMesh) {
    const std::string obj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
    auto mesh = entity::ObjLoader::loadFromString(obj, "tri");
    EXPECT_TRUE(mesh.isValid());
    EXPECT_EQ(mesh.indices.size(), 3u);
}
