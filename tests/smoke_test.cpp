#include <expected>
#include <flat_map>
#include <string>

#include <gtest/gtest.h>

TEST(Smoke, ExpectedWorks) {
    std::expected<int, std::string> ok = 7;
    std::expected<int, std::string> err = std::unexpected("boom");
    EXPECT_TRUE(ok.has_value());
    EXPECT_EQ(*ok, 7);
    EXPECT_FALSE(err.has_value());
    EXPECT_EQ(err.error(), "boom");
}

TEST(Smoke, FlatMapWorks) {
    std::flat_map<std::string, int> m;
    m.insert({"b", 2});
    m.insert({"a", 1});
    m.insert({"c", 3});
    ASSERT_EQ(m.size(), 3u);
    auto it = m.begin();
    EXPECT_EQ(it->first, "a"); ++it;
    EXPECT_EQ(it->first, "b"); ++it;
    EXPECT_EQ(it->first, "c");
}
