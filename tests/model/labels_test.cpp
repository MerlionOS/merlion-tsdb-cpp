#include "merlion_tsdb/model/labels.hpp"

#include <string>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

namespace m = merlion_tsdb::model;

namespace {

std::vector<std::pair<std::string, std::string>>
to_pairs(const m::Labels& l) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& e : l.entries()) out.emplace_back(e.name, e.value);
    return out;
}

}  // namespace

TEST(Labels, EmptyIsEmpty) {
    m::Labels l;
    EXPECT_TRUE(l.empty());
    EXPECT_EQ(l.size(), 0u);
    EXPECT_FALSE(l.get("__name__").has_value());
}

TEST(Labels, ConstructorSortsByName) {
    m::Labels l{
        {.name = "z",        .value = "last"},
        {.name = "__name__", .value = "metric"},
        {.name = "job",      .value = "api"},
    };
    auto pairs = to_pairs(l);
    ASSERT_EQ(pairs.size(), 3u);
    EXPECT_EQ(pairs[0].first, "__name__");
    EXPECT_EQ(pairs[1].first, "job");
    EXPECT_EQ(pairs[2].first, "z");
}

TEST(Labels, DuplicateNamesKeepLast) {
    m::Labels l{
        {.name = "k", .value = "first"},
        {.name = "k", .value = "second"},
        {.name = "k", .value = "third"},
    };
    ASSERT_EQ(l.size(), 1u);
    EXPECT_EQ(l.get("k"), std::optional<std::string_view>{"third"});
}

TEST(Labels, GetByNameAfterSort) {
    m::Labels l{
        {"job",      "api"},
        {"instance", "10.0.0.1"},
        {"__name__", "http_requests_total"},
    };
    EXPECT_EQ(l.get("__name__"), std::optional<std::string_view>{"http_requests_total"});
    EXPECT_EQ(l.get("instance"), std::optional<std::string_view>{"10.0.0.1"});
    EXPECT_EQ(l.get("job"),      std::optional<std::string_view>{"api"});
    EXPECT_FALSE(l.get("missing").has_value());
    EXPECT_TRUE(l.has("instance"));
    EXPECT_FALSE(l.has("nope"));
}

TEST(Labels, AddBuilderChain) {
    m::Labels l;
    l.add("c", "3").add("a", "1").add("b", "2");
    auto pairs = to_pairs(l);
    ASSERT_EQ(pairs.size(), 3u);
    EXPECT_EQ(pairs[0].first, "a");
    EXPECT_EQ(pairs[1].first, "b");
    EXPECT_EQ(pairs[2].first, "c");
}

TEST(Labels, EqualityIgnoresInsertionOrder) {
    m::Labels a{{"a", "1"}, {"b", "2"}};
    m::Labels b{{"b", "2"}, {"a", "1"}};
    EXPECT_EQ(a, b);
}

TEST(Labels, HashStableAcrossEqualSets) {
    m::Labels a{{"job", "api"}, {"__name__", "metric"}, {"instance", "h:9090"}};
    m::Labels b{{"__name__", "metric"}, {"instance", "h:9090"}, {"job", "api"}};
    EXPECT_EQ(a.hash(), b.hash());
    // And idempotent: calling hash() again returns the same value.
    EXPECT_EQ(a.hash(), a.hash());
}

TEST(Labels, HashDiffersOnDifferentSets) {
    m::Labels a{{"job", "api"}};
    m::Labels b{{"job", "web"}};
    EXPECT_NE(a.hash(), b.hash());

    // Boundary collision check: "ab" / "c" and "a" / "bc" must not hash to
    // the same value (separator byte does its job).
    m::Labels x{{"k", "ab"}, {"l", "c"}};
    m::Labels y{{"k", "a"}, {"l", "bc"}};
    EXPECT_NE(x.hash(), y.hash());

    // Same names, swapped values: must hash differently.
    m::Labels p{{"k", "a"}, {"l", "b"}};
    m::Labels q{{"k", "b"}, {"l", "a"}};
    EXPECT_NE(p.hash(), q.hash());
}

TEST(Labels, UsableAsUnorderedMapKey) {
    std::unordered_map<m::Labels, int, m::LabelsHash> map;
    m::Labels k1{{"a", "1"}};
    m::Labels k2{{"a", "2"}};
    m::Labels k1_again{{"a", "1"}};

    map[k1] = 100;
    map[k2] = 200;
    EXPECT_EQ(map.size(), 2u);
    EXPECT_EQ(map[k1_again], 100);
    map[k1_again] = 101;
    EXPECT_EQ(map.size(), 2u);
    EXPECT_EQ(map[k1], 101);
}

TEST(Labels, EmptyLabelsHashIsStableAndDistinctFromSingletonEmpty) {
    m::Labels empty;
    m::Labels singleton_empty{{"", ""}};  // a single (name="", value="") pair
    EXPECT_NE(empty.hash(), singleton_empty.hash());
}

TEST(Labels, LongLabelValuesWork) {
    std::string big_value(10'000, 'x');
    m::Labels l{{"big", big_value}};
    EXPECT_EQ(l.get("big"), std::optional<std::string_view>{big_value});
    EXPECT_EQ(l.size(), 1u);
}
