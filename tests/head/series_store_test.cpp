#include "merlion_tsdb/head/series_store.hpp"

#include <gtest/gtest.h>

namespace h = merlion_tsdb::head;
namespace m = merlion_tsdb::model;

TEST(SeriesStore, EmptyStoreIsEmpty) {
    h::SeriesStore store;
    EXPECT_EQ(store.size(), 0u);
    EXPECT_EQ(store.next_ref(), 1u);
    EXPECT_EQ(store.get(0), nullptr);
    EXPECT_EQ(store.get(1), nullptr);
}

TEST(SeriesStore, GetOrCreateAssignsRefsMonotonically) {
    h::SeriesStore store;
    auto [a, fresh_a] = store.get_or_create(m::Labels{{"a", "1"}});
    auto [b, fresh_b] = store.get_or_create(m::Labels{{"b", "2"}});
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_TRUE(fresh_a);
    EXPECT_TRUE(fresh_b);
    EXPECT_EQ(a->ref(), 1u);
    EXPECT_EQ(b->ref(), 2u);
    EXPECT_EQ(store.size(), 2u);
    EXPECT_EQ(store.next_ref(), 3u);
}

TEST(SeriesStore, GetOrCreateIsIdempotent) {
    h::SeriesStore store;
    m::Labels lbls{{"job", "api"}, {"__name__", "metric"}};
    auto [first,  fresh1] = store.get_or_create(lbls);
    auto [second, fresh2] = store.get_or_create(lbls);
    EXPECT_TRUE(fresh1);
    EXPECT_FALSE(fresh2);
    EXPECT_EQ(first, second);
    EXPECT_EQ(store.size(), 1u);
}

TEST(SeriesStore, GetByRefReturnsSamePointerAsCreate) {
    h::SeriesStore store;
    auto [created, _] = store.get_or_create(m::Labels{{"k", "v"}});
    EXPECT_EQ(store.get(created->ref()), created);
    EXPECT_EQ(static_cast<const h::SeriesStore&>(store).get(created->ref()), created);
}

TEST(SeriesStore, InsertWithRefForReplay) {
    h::SeriesStore store;
    ASSERT_TRUE(store.insert_with_ref(5, m::Labels{{"a", "1"}}));
    ASSERT_TRUE(store.insert_with_ref(3, m::Labels{{"b", "2"}}));
    EXPECT_EQ(store.next_ref(), 6u);  // max(5, 3) + 1
    EXPECT_NE(store.get(5), nullptr);
    EXPECT_NE(store.get(3), nullptr);
    EXPECT_EQ(store.get(5)->labels().get("a"), std::optional<std::string_view>{"1"});

    // Re-inserting the same ref with same labels is a no-op success.
    EXPECT_TRUE(store.insert_with_ref(5, m::Labels{{"a", "1"}}));
    // Re-inserting the same ref with different labels is a conflict.
    EXPECT_FALSE(store.insert_with_ref(5, m::Labels{{"a", "OTHER"}}));
    // Re-inserting the same labels with a different ref is also a conflict.
    EXPECT_FALSE(store.insert_with_ref(99, m::Labels{{"a", "1"}}));
}

TEST(SeriesStore, GetOrCreateContinuesAfterReplayedRefs) {
    h::SeriesStore store;
    ASSERT_TRUE(store.insert_with_ref(10, m::Labels{{"a", "1"}}));
    auto [next, fresh] = store.get_or_create(m::Labels{{"b", "2"}});
    EXPECT_TRUE(fresh);
    // get_or_create must dispense a ref strictly above the highest replayed one.
    EXPECT_EQ(next->ref(), 11u);
}

TEST(SeriesStore, AllSeriesEnumeratesAllRefs) {
    h::SeriesStore store;
    store.get_or_create(m::Labels{{"a", "1"}});
    store.get_or_create(m::Labels{{"a", "2"}});
    store.get_or_create(m::Labels{{"a", "3"}});

    auto all = store.all_series();
    EXPECT_EQ(all.size(), 3u);
    EXPECT_EQ(all[0]->ref(), 1u);
    EXPECT_EQ(all[1]->ref(), 2u);
    EXPECT_EQ(all[2]->ref(), 3u);
}

TEST(SeriesStore, GetByZeroIsAlwaysNullptr) {
    h::SeriesStore store;
    store.get_or_create(m::Labels{{"k", "v"}});
    EXPECT_EQ(store.get(0), nullptr);  // 0 is reserved
}

TEST(SeriesStore, AppendThroughSeriesPointerWorks) {
    // End-to-end: after get_or_create, the returned pointer supports
    // sample appends and round-trip queries.
    h::SeriesStore store;
    auto [s, _] = store.get_or_create(m::Labels{{"__name__", "metric"}});
    ASSERT_TRUE(s->append(1000, 1.0));
    ASSERT_TRUE(s->append(2000, 2.0));
    ASSERT_TRUE(s->append(3000, 3.0));
    EXPECT_EQ(s->num_samples(), 3u);
    EXPECT_EQ(s->last_t(), 3000);

    // Looking up by ref returns the same series.
    auto* by_ref = store.get(s->ref());
    EXPECT_EQ(by_ref, s);
    EXPECT_EQ(by_ref->num_samples(), 3u);
}
