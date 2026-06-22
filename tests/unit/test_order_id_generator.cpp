/**
 * @file test_order_id_generator.cpp
 * @brief Unit tests for OrderIDGenerator (header-only, <10ns target)
 *
 * Validates: Requirements 5.1, 5.2, 5.3, 5.4
 * PBT Property 5: Order ID Uniqueness
 */

#include <gtest/gtest.h>
#include <chronos/trading/order_id_generator.hpp>
#include <set>
#include <thread>
#include <vector>
#include <mutex>
#include <algorithm>

using namespace chronos::trading;

TEST(OrderIDGeneratorTest, DefaultConstructorStartsAt1) {
    OrderIDGenerator gen;
    EXPECT_EQ(gen.generate(), 1ULL);
    EXPECT_EQ(gen.getCurrentID(), 2ULL);  // advanced past 1
}

TEST(OrderIDGeneratorTest, CustomStartId) {
    OrderIDGenerator gen(1000);
    EXPECT_EQ(gen.generate(), 1000ULL);
    EXPECT_EQ(gen.generate(), 1001ULL);
}

TEST(OrderIDGeneratorTest, MonotonicIncrement) {
    OrderIDGenerator gen;
    uint64_t prev = gen.generate();
    for (int i = 0; i < 10000; ++i) {
        uint64_t next = gen.generate();
        EXPECT_GT(next, prev) << "Non-monotonic at iteration " << i;
        prev = next;
    }
}

TEST(OrderIDGeneratorTest, GetCurrentIdDoesNotConsume) {
    OrderIDGenerator gen;
    uint64_t id1 = gen.getCurrentID();
    uint64_t id2 = gen.getCurrentID();
    EXPECT_EQ(id1, id2);
    uint64_t consumed = gen.generate();
    EXPECT_EQ(consumed, id1);  // generated == previously peeked
    EXPECT_GT(gen.getCurrentID(), id1);
}

TEST(OrderIDGeneratorTest, SingleThreadUniqueness) {
    OrderIDGenerator gen;
    constexpr size_t N = 100000;
    std::vector<uint64_t> ids(N);
    for (size_t i = 0; i < N; ++i) {
        ids[i] = gen.generate();
    }
    std::set<uint64_t> unique(ids.begin(), ids.end());
    EXPECT_EQ(unique.size(), N);
}

TEST(OrderIDGeneratorTest, ConcurrentUniqueness) {
    OrderIDGenerator gen;
    constexpr size_t PER_THREAD = 25000;
    constexpr int THREADS = 4;

    std::vector<std::thread> threads;
    std::vector<std::vector<uint64_t>> results(THREADS);

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&gen, t, &results]() {
            results[t].reserve(PER_THREAD);
            for (size_t i = 0; i < PER_THREAD; ++i) {
                results[t].push_back(gen.generate());
            }
        });
    }

    for (auto& t : threads) t.join();

    // Collect all IDs
    std::set<uint64_t> all_ids;
    for (const auto& vec : results) {
        for (auto id : vec) {
            EXPECT_TRUE(all_ids.insert(id).second)
                << "Duplicate ID: " << id;
        }
    }
    EXPECT_EQ(all_ids.size(), PER_THREAD * THREADS);
}

TEST(OrderIDGeneratorTest, ConcurrentMonotonicityPerThread) {
    OrderIDGenerator gen;
    constexpr size_t PER_THREAD = 10000;
    constexpr int THREADS = 4;

    std::vector<std::thread> threads;
    std::vector<std::vector<uint64_t>> results(THREADS);

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&gen, t, &results]() {
            results[t].reserve(PER_THREAD);
            for (size_t i = 0; i < PER_THREAD; ++i) {
                results[t].push_back(gen.generate());
            }
        });
    }

    for (auto& t : threads) t.join();

    // Each thread's IDs should be strictly increasing
    for (const auto& vec : results) {
        for (size_t i = 1; i < vec.size(); ++i) {
            EXPECT_GT(vec[i], vec[i - 1])
                << "Thread-local non-monotonic";
        }
    }
}

TEST(OrderIDGeneratorTest, ThreadSafetyUnderContention) {
    OrderIDGenerator gen;
    constexpr size_t PER_THREAD = 25000;
    constexpr int THREADS = 8;
    constexpr size_t TOTAL = PER_THREAD * THREADS;

    std::vector<std::thread> threads;
    std::vector<uint64_t> all_ids(TOTAL);
    std::mutex idx_mutex;
    size_t global_idx = 0;

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&]() {
            for (size_t i = 0; i < PER_THREAD; ++i) {
                uint64_t id = gen.generate();
                std::lock_guard<std::mutex> lock(idx_mutex);
                all_ids[global_idx++] = id;
            }
        });
    }

    for (auto& t : threads) t.join();

    EXPECT_EQ(global_idx, TOTAL);

    // Sort + check uniqueness
    std::sort(all_ids.begin(), all_ids.end());
    auto dup = std::adjacent_find(all_ids.begin(), all_ids.end());
    EXPECT_EQ(dup, all_ids.end()) << "Duplicate ID found";
}

TEST(OrderIDGeneratorTest, CompactSanityCheck) {
    OrderIDGenerator gen;
    // 0 is reserved — verify the generator never produces 0
    for (int i = 0; i < 1000; ++i) {
        EXPECT_NE(gen.generate(), 0ULL);
    }
}
