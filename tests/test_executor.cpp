#include <async_framework/executor.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

using namespace morph;

TEST_CASE("ThreadPoolExecutor runs posted tasks", "[executor]") {
    ThreadPoolExecutor pool{2};
    std::atomic<int> count{0};
    for (int i = 0; i < 10; ++i) {
        pool.post([&] { count.fetch_add(1); });
    }
    // Give workers time to drain
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(count.load() == 10);
}

TEST_CASE("ThreadPoolExecutor swallows exceptions from tasks", "[executor]") {
    ThreadPoolExecutor pool{1};
    std::atomic<bool> reached{false};
    pool.post([] { throw std::runtime_error("boom"); });
    pool.post([&] { reached.store(true); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(reached.load());
}

TEST_CASE("MainThreadExecutor drains tasks via runFor", "[executor]") {
    MainThreadExecutor exec;
    std::atomic<int> count{0};
    for (int i = 0; i < 5; ++i) {
        exec.post([&] { count.fetch_add(1); });
    }
    exec.runFor(std::chrono::milliseconds(200));
    REQUIRE(count.load() == 5);
}

TEST_CASE("MainThreadExecutor times out when queue is empty", "[executor]") {
    MainThreadExecutor exec;
    auto startTime = std::chrono::steady_clock::now();
    exec.runFor(std::chrono::milliseconds(50));
    auto elapsed = std::chrono::steady_clock::now() - startTime;
    // Should return roughly on time (within 2x)
    REQUIRE(elapsed < std::chrono::milliseconds(200));
}
