#include "Session.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <thread>
#include <vector>

TEST_CASE("a sequence counter hands out consecutive numbers and wraps at 2^32") {
    fm::SequenceCounter counter(0xFFFF'FFFE);
    CHECK(counter.next() == 0xFFFF'FFFE);
    CHECK(counter.next() == 0xFFFF'FFFF);
    CHECK(counter.next() == 0);
    CHECK(counter.peek() == 1);
}

TEST_CASE("a default session's counter starts at zero") {
    fm::Session session;
    CHECK(session.seq.peek() == 0);
}

TEST_CASE("copies of a session share one counter, so a session keeps one seq sequence") {
    fm::Session session{.peer = fm::Endpoint{}, .sessionId = 7, .seq = fm::SequenceCounter(100)};
    fm::Session copy = session;
    CHECK(session.seq.next() == 100);
    CHECK(copy.seq.next() == 101);
    CHECK(session.seq.peek() == 102);

    copy = fm::Session{.peer = fm::Endpoint{}, .sessionId = 8, .seq = fm::SequenceCounter(5)};
    CHECK(copy.seq.next() == 5);
    CHECK(session.seq.next() == 102);
}

TEST_CASE("threads that share a counter never get the same number") {
    constexpr int THREAD_COUNT = 4;
    constexpr int NUMBERS_PER_THREAD = 10000;
    constexpr std::uint32_t FIRST = 5;
    fm::SequenceCounter counter(FIRST);
    std::vector<std::vector<std::uint32_t>> taken(THREAD_COUNT);
    {
        std::vector<std::thread> threads;
        for (auto& numbers : taken) {
            threads.emplace_back([counter, &numbers]() mutable {
                for (int index = 0; index < NUMBERS_PER_THREAD; ++index) {
                    numbers.push_back(counter.next());
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
    }
    std::vector<std::uint32_t> all;
    for (const auto& numbers : taken) {
        all.insert(all.end(), numbers.begin(), numbers.end());
    }
    std::ranges::sort(all);
    REQUIRE(all.size() == THREAD_COUNT * NUMBERS_PER_THREAD);
    CHECK(std::ranges::adjacent_find(all) == all.end());
    CHECK(all.front() == FIRST);
    CHECK(all.back() == FIRST + THREAD_COUNT * NUMBERS_PER_THREAD - 1);
}
