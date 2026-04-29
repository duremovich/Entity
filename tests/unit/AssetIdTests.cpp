#include "entity/core/AssetId.hpp"

#include <gtest/gtest.h>

#include <string>
#include <unordered_map>
#include <unordered_set>

using entity::AssetId;

TEST(AssetIdTest, DefaultIsEmpty) {
    AssetId id;
    EXPECT_TRUE(id.empty());
    EXPECT_TRUE(id.value().empty());
}

TEST(AssetIdTest, ConstructFromString) {
    AssetId id{std::string("media/foo.mov")};
    EXPECT_FALSE(id.empty());
    EXPECT_EQ(id.value(), "media/foo.mov");
}

TEST(AssetIdTest, ConstructFromCStr) {
    AssetId id{"hello"};
    EXPECT_EQ(id.value(), std::string("hello"));
}

TEST(AssetIdTest, EqualityHashesSameStringTogether) {
    AssetId a{"x"};
    AssetId b{std::string{"x"}};
    EXPECT_EQ(a, b);
    EXPECT_FALSE(a != b);

    std::unordered_set<AssetId> set;
    set.insert(a);
    EXPECT_EQ(set.count(b), 1u);
}

TEST(AssetIdTest, InequalityForDifferentStrings) {
    AssetId a{"a.mov"};
    AssetId b{"b.mov"};
    EXPECT_NE(a, b);
}

TEST(AssetIdTest, CanonicalizeNormalizesBackslashesToForwardSlashes) {
    AssetId in{R"(C:\media\test.mov)"};
    AssetId out = entity::canonicalize(in);
    EXPECT_EQ(out.value(), "C:/media/test.mov");
}

TEST(AssetIdTest, CanonicalizeIsIdempotent) {
    AssetId in{R"(C:\foo\bar\baz.mov)"};
    AssetId once = entity::canonicalize(in);
    AssetId twice = entity::canonicalize(once);
    EXPECT_EQ(once, twice);
}

TEST(AssetIdTest, CanonicalizeEmptyStaysEmpty) {
    AssetId out = entity::canonicalize(AssetId{});
    EXPECT_TRUE(out.empty());
}

TEST(AssetIdTest, CanonicalizePreservesCase) {
    // Filesystem case-sensitivity varies per OS. Lowercasing here would
    // break Linux call sites; the typedef stays case-preserving.
    AssetId in{"C:/Media/Foo.MOV"};
    AssetId out = entity::canonicalize(in);
    EXPECT_EQ(out.value(), "C:/Media/Foo.MOV");
}

TEST(AssetIdTest, UseAsHashMapKey) {
    std::unordered_map<AssetId, int> map;
    map[AssetId{"a"}] = 1;
    map[AssetId{"b"}] = 2;
    EXPECT_EQ(map.at(AssetId{"a"}), 1);
    EXPECT_EQ(map.at(AssetId{"b"}), 2);
    EXPECT_EQ(map.size(), 2u);
}
