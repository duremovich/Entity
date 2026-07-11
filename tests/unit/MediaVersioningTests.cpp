#include "entity/project/MediaVersioning.hpp"

#include <gtest/gtest.h>

using entity::compareVersionTags;
using entity::groupKeyOf;
using entity::parseVersion;
using entity::toLogicalPath;

// ---------------------------------------------------------------------------
// parseVersion — happy paths
// ---------------------------------------------------------------------------

TEST(MediaVersioning, ParseSimpleNumericTag) {
    auto pv = parseVersion("intro_v01");
    EXPECT_EQ(pv.base, "intro");
    EXPECT_EQ(pv.tag,  "01");
}

TEST(MediaVersioning, ParseSingleDigitTag) {
    auto pv = parseVersion("intro_v2");
    EXPECT_EQ(pv.base, "intro");
    EXPECT_EQ(pv.tag,  "2");
}

TEST(MediaVersioning, ParseDateStyleTag) {
    auto pv = parseVersion("intro_v20210602a");
    EXPECT_EQ(pv.base, "intro");
    EXPECT_EQ(pv.tag,  "20210602a");
}

TEST(MediaVersioning, ParseBaseWithDotIsAllowed) {
    auto pv = parseVersion("intro.scene1_v03");
    EXPECT_EQ(pv.base, "intro.scene1");
    EXPECT_EQ(pv.tag,  "03");
}

TEST(MediaVersioning, ParsePicksLastVMarker) {
    // "act_v1_v2" — the rightmost _v wins; base keeps the inner _v.
    auto pv = parseVersion("act_v1_v2");
    EXPECT_EQ(pv.base, "act_v1");
    EXPECT_EQ(pv.tag,  "2");
}

TEST(MediaVersioning, ParseDigitLeadingTagWithSuffix) {
    auto pv = parseVersion("intro_v2a");
    EXPECT_EQ(pv.base, "intro");
    EXPECT_EQ(pv.tag,  "2a");
}

// ---------------------------------------------------------------------------
// parseVersion — non-matches (whole stem is base)
// ---------------------------------------------------------------------------

TEST(MediaVersioning, NoTagWhenNoMarker) {
    auto pv = parseVersion("intro");
    EXPECT_EQ(pv.base, "intro");
    EXPECT_TRUE(pv.tag.empty());
}

TEST(MediaVersioning, RoleSuffixDefeatsMatch) {
    // Underscores are not valid in tags; "_v01_alpha" isn't a version.
    auto pv = parseVersion("intro_v01_alpha");
    EXPECT_EQ(pv.base, "intro_v01_alpha");
    EXPECT_TRUE(pv.tag.empty());
}

TEST(MediaVersioning, EmptyTagDefeatsMatch) {
    auto pv = parseVersion("intro_v");
    EXPECT_EQ(pv.base, "intro_v");
    EXPECT_TRUE(pv.tag.empty());
}

TEST(MediaVersioning, WordSuffixContainingVIsNotAVersion) {
    // Issue #83 regression: `_visual` / `_video` / `_version` contain the
    // `_v` marker but are word suffixes, not version tags. The tag must
    // start with a digit; a letter-leading candidate defeats the match.
    for (const char* stem : {"intro_visual", "title_video", "cut_version"}) {
        auto pv = parseVersion(stem);
        EXPECT_EQ(pv.base, stem);
        EXPECT_TRUE(pv.tag.empty()) << stem;
    }
}

TEST(MediaVersioning, LetterLeadingTagAfterRealVersionDefeatsMatch) {
    // Rightmost `_v` is `_version...` (letter-leading) — no fallback to
    // the earlier `_v2`, since a valid tag must reach the end of stem.
    auto pv = parseVersion("intro_v2_versionB");
    EXPECT_EQ(pv.base, "intro_v2_versionB");
    EXPECT_TRUE(pv.tag.empty());
}

TEST(MediaVersioning, CaseInsensitiveVMarker) {
    // Real-world filenames ship with both `_v01` and `_V01`. Treat them
    // identically (case-insensitive on the marker itself).
    auto lower = parseVersion("intro_v01");
    auto upper = parseVersion("intro_V01");
    EXPECT_EQ(lower.base, "intro");
    EXPECT_EQ(lower.tag,  "01");
    EXPECT_EQ(upper.base, "intro");
    EXPECT_EQ(upper.tag,  "01");
}

TEST(MediaVersioning, MarkerAtPositionZeroIsRejected) {
    // "_v01" alone: would leave empty base, treat as unversioned.
    auto pv = parseVersion("_v01");
    EXPECT_EQ(pv.base, "_v01");
    EXPECT_TRUE(pv.tag.empty());
}

TEST(MediaVersioning, EmptyStem) {
    auto pv = parseVersion("");
    EXPECT_TRUE(pv.base.empty());
    EXPECT_TRUE(pv.tag.empty());
}

// ---------------------------------------------------------------------------
// compareVersionTags — numeric path
// ---------------------------------------------------------------------------

TEST(MediaVersioning, SortNumericNotLexicographic) {
    // Lexicographically "10" < "2"; numerically "10" > "2".
    EXPECT_LT(compareVersionTags("2", "10"), 0);
    EXPECT_GT(compareVersionTags("10", "2"), 0);
}

TEST(MediaVersioning, SortPaddedNumericAlsoNumeric) {
    EXPECT_LT(compareVersionTags("01", "02"), 0);
    EXPECT_LT(compareVersionTags("01", "10"), 0);
}

TEST(MediaVersioning, EqualNumericTags) {
    EXPECT_EQ(compareVersionTags("01", "01"), 0);
    // "1" and "01" parse to the same number — treat as equal.
    EXPECT_EQ(compareVersionTags("1", "01"), 0);
}

// ---------------------------------------------------------------------------
// compareVersionTags — lexicographic path
// ---------------------------------------------------------------------------

TEST(MediaVersioning, SortDateStyleLexicographic) {
    EXPECT_LT(compareVersionTags("20210602a", "20210602b"), 0);
    EXPECT_LT(compareVersionTags("20210601a", "20210602a"), 0);
}

TEST(MediaVersioning, MixedTagsCompareNumericallyOnLeadingDigits) {
    // Issue #83 regression: mixed tags used to fall to pure lexicographic
    // compare, so "10a" < "2a". The leading digit run decides numerically.
    EXPECT_LT(compareVersionTags("2a", "10a"), 0);
    EXPECT_GT(compareVersionTags("10a", "2a"), 0);
    EXPECT_LT(compareVersionTags("2", "10a"), 0);
    EXPECT_LT(compareVersionTags("02a", "10a"), 0);
    EXPECT_EQ(compareVersionTags("02a", "2a"), 0);
    // Bare number sorts before the same number with a suffix.
    EXPECT_LT(compareVersionTags("2", "2a"), 0);
}

TEST(MediaVersioning, EmptyTagSortsBeforeAny) {
    EXPECT_LT(compareVersionTags("", "01"), 0);
    EXPECT_LT(compareVersionTags("", "anything"), 0);
    EXPECT_GT(compareVersionTags("01", ""), 0);
    EXPECT_EQ(compareVersionTags("", ""), 0);
}

// ---------------------------------------------------------------------------
// groupKeyOf
// ---------------------------------------------------------------------------

TEST(MediaVersioning, GroupKeyForUnversioned) {
    EXPECT_EQ(groupKeyOf("intro.mov"), "intro");
}

TEST(MediaVersioning, GroupKeyForVersioned) {
    EXPECT_EQ(groupKeyOf("intro_v03.mov"), "intro");
}

TEST(MediaVersioning, GroupKeyKeepsSubdirPrefix) {
    EXPECT_EQ(groupKeyOf("act1/intro_v03.mov"), "act1/intro");
}

TEST(MediaVersioning, GroupKeyKeepsBackslashSubdirPrefix) {
    EXPECT_EQ(groupKeyOf("act1\\intro_v03.mov"), "act1\\intro");
}

TEST(MediaVersioning, GroupKeyDifferentSubdirsDoNotCollide) {
    EXPECT_NE(groupKeyOf("act1/intro_v03.mov"),
              groupKeyOf("act2/intro_v03.mov"));
}

TEST(MediaVersioning, GroupKeyRoleSuffixedFileDoesNotJoinPrefixGroup) {
    // Issue #83 regression: `intro_visual.mov` used to collapse into the
    // `intro.mov` group (base "intro", bogus tag "isual") and, ranking
    // above the empty tag, hijacked the row as its "latest" version.
    EXPECT_NE(groupKeyOf("intro_visual.mov"), groupKeyOf("intro.mov"));
    EXPECT_EQ(groupKeyOf("intro_visual.mov"), "intro_visual");
}

TEST(MediaVersioning, GroupKeyExtensionIgnored) {
    // Same logical media in two containers — same group.
    EXPECT_EQ(groupKeyOf("intro_v01.mov"),
              groupKeyOf("intro_v01.mp4"));
}

// ---------------------------------------------------------------------------
// toLogicalPath
// ---------------------------------------------------------------------------

TEST(MediaVersioning, ToLogicalStripsTag) {
    EXPECT_EQ(toLogicalPath("intro_v03.mov"), "intro.mov");
}

TEST(MediaVersioning, ToLogicalIdempotentOnUnversioned) {
    EXPECT_EQ(toLogicalPath("intro.mov"), "intro.mov");
}

TEST(MediaVersioning, ToLogicalKeepsSubdir) {
    EXPECT_EQ(toLogicalPath("act1/intro_v03.mov"), "act1/intro.mov");
}

TEST(MediaVersioning, ToLogicalKeepsBackslashSubdir) {
    EXPECT_EQ(toLogicalPath("act1\\intro_v03.mov"), "act1\\intro.mov");
}
