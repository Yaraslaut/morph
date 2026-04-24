// SPDX-License-Identifier: Apache-2.0

#include <async_framework/offline_queue.hpp>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <thread>
#include <vector>

using namespace morph;

TEST_CASE("InMemoryOfflineQueue: enqueue returns a unique id per item", "[queue]") {
    InMemoryOfflineQueue queue;
    auto id1 = queue.enqueue("first");
    auto id2 = queue.enqueue("second");
    REQUIRE(id1 != id2);
}

TEST_CASE("InMemoryOfflineQueue: drain returns items in enqueue order", "[queue]") {
    InMemoryOfflineQueue queue;
    queue.enqueue("a");
    queue.enqueue("b");
    queue.enqueue("c");

    auto items = queue.drain();
    REQUIRE(items.size() == 3);
    REQUIRE(items[0].payload == "a");
    REQUIRE(items[1].payload == "b");
    REQUIRE(items[2].payload == "c");
}

TEST_CASE("InMemoryOfflineQueue: drain on empty queue returns empty vector", "[queue]") {
    InMemoryOfflineQueue queue;
    REQUIRE(queue.drain().empty());
}

TEST_CASE("InMemoryOfflineQueue: markDone removes item from future drains", "[queue]") {
    InMemoryOfflineQueue queue;
    auto itemId = queue.enqueue("x");
    auto items = queue.drain();
    REQUIRE(items.size() == 1);

    queue.markDone(itemId);

    REQUIRE(queue.drain().empty());
}

TEST_CASE("InMemoryOfflineQueue: markDone on unknown id is a no-op", "[queue]") {
    InMemoryOfflineQueue queue;
    queue.enqueue("y");
    REQUIRE_NOTHROW(queue.markDone(9999));
    REQUIRE(queue.drain().size() == 1);
}

TEST_CASE("InMemoryOfflineQueue: drain does not remove items (items survive until markDone)", "[queue]") {
    InMemoryOfflineQueue queue;
    queue.enqueue("z");

    auto first = queue.drain();
    auto second = queue.drain();

    REQUIRE(first.size() == 1);
    REQUIRE(second.size() == 1);
    REQUIRE(first[0].payload == second[0].payload);
}

TEST_CASE("InMemoryOfflineQueue: concurrent enqueue from multiple threads is safe", "[queue][threading]") {
    InMemoryOfflineQueue queue;
    constexpr int nThreads = 8;
    constexpr int nPerThread = 100;

    std::vector<std::thread> threads;
    threads.reserve(nThreads);
    for (int i = 0; i < nThreads; ++i) {
        threads.emplace_back([&, i] {
            for (int j = 0; j < nPerThread; ++j) {
                queue.enqueue("t" + std::to_string(i) + "_" + std::to_string(j));
            }
        });
    }
    for (auto& thr : threads) {
        thr.join();
    }

    REQUIRE(queue.drain().size() == static_cast<std::size_t>(nThreads * nPerThread));
}
