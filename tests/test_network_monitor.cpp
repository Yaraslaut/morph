#include <async_framework/network_monitor.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>
#include <vector>

using namespace morph;
using namespace std::chrono_literals;

// Poll until condition is true or deadline is reached.
// More robust than a fixed sleep on loaded CI machines.
static bool waitUntil(auto cond, std::chrono::milliseconds timeout = 500ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!cond()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

// ── Skeleton tests ────────────────────────────────────────────────────────────

TEST_CASE("NetworkMonitor: starts online when probe succeeds", "[monitor]") {
    NetworkMonitor mon{[] { return true; }, [] {}, [] {}, NetworkMonitor::Config{.probeInterval = 50ms}};
    std::this_thread::sleep_for(20ms);
    REQUIRE(mon.isOnline());
}

TEST_CASE("NetworkMonitor: starts offline when probe fails", "[monitor]") {
    NetworkMonitor mon{[] { return false; }, [] {}, [] {},
                       NetworkMonitor::Config{.probeInterval = 50ms, .failureThreshold = 1}};
    REQUIRE(waitUntil([&] { return !mon.isOnline(); }));
}

TEST_CASE("NetworkMonitor: stop() is safe to call and joins cleanly", "[monitor]") {
    NetworkMonitor mon{[] { return true; }, [] {}, [] {}, NetworkMonitor::Config{.probeInterval = 50ms}};
    REQUIRE_NOTHROW(mon.stop());
    REQUIRE_NOTHROW(mon.stop());
}

TEST_CASE("NetworkMonitor: destructor joins thread without explicit stop", "[monitor]") {
    REQUIRE_NOTHROW([] {
        NetworkMonitor mon{[] { return true; }, [] {}, [] {}, NetworkMonitor::Config{.probeInterval = 50ms}};
    }());
}

// ── Offline transition ────────────────────────────────────────────────────────

TEST_CASE("NetworkMonitor: fires onOffline after failureThreshold consecutive failures", "[monitor][state]") {
    std::atomic<int> offlineCount{0};
    std::atomic<bool> probeResult{true};

    NetworkMonitor mon{[&] { return probeResult.load(); }, [&] { ++offlineCount; }, [] {},
                       NetworkMonitor::Config{.probeInterval = 30ms, .failureThreshold = 3}};

    // Let one successful probe run first.
    std::this_thread::sleep_for(50ms);
    REQUIRE(offlineCount.load() == 0);

    // Switch probe to failing.
    probeResult.store(false);

    // Wait for the transition (up to 500ms).
    REQUIRE(waitUntil([&] { return offlineCount.load() == 1; }));
    REQUIRE_FALSE(mon.isOnline());
}

TEST_CASE("NetworkMonitor: onOffline fires exactly once per transition", "[monitor][state]") {
    std::atomic<int> offlineCount{0};
    std::atomic<bool> probeResult{false};

    NetworkMonitor mon{[&] { return probeResult.load(); }, [&] { ++offlineCount; }, [] {},
                       NetworkMonitor::Config{.probeInterval = 20ms, .failureThreshold = 2}};

    // Wait for the first transition, then hold well past it.
    REQUIRE(waitUntil([&] { return offlineCount.load() >= 1; }));
    std::this_thread::sleep_for(150ms);

    // Exactly one transition regardless of probe count.
    REQUIRE(offlineCount.load() == 1);
}

TEST_CASE("NetworkMonitor: stays online below failureThreshold", "[monitor][state]") {
    std::atomic<int> callCount{0};

    // threshold=3; fail probes 2 and 3 only, succeed all others.
    NetworkMonitor mon{[&] {
                           int cnt = ++callCount;  // increment and capture atomically in this thread
                           return cnt != 2 && cnt != 3;
                       },
                       [] { FAIL("onOffline must not fire"); }, [] {},
                       NetworkMonitor::Config{.probeInterval = 20ms, .failureThreshold = 3}};

    std::this_thread::sleep_for(200ms);
    REQUIRE(mon.isOnline());
}

// ── Online recovery ───────────────────────────────────────────────────────────

TEST_CASE("NetworkMonitor: fires onOnline after onlineThreshold successes when offline", "[monitor][state]") {
    std::atomic<int> onlineCount{0};
    std::atomic<bool> probeResult{false};

    NetworkMonitor mon{[&] { return probeResult.load(); }, [] {}, [&] { ++onlineCount; },
                       NetworkMonitor::Config{.probeInterval = 20ms, .failureThreshold = 1, .onlineThreshold = 2}};

    // Wait to go offline.
    REQUIRE(waitUntil([&] { return !mon.isOnline(); }));
    REQUIRE(onlineCount.load() == 0);

    // Restore probe and wait for recovery.
    probeResult.store(true);
    REQUIRE(waitUntil([&] { return onlineCount.load() == 1; }));
    REQUIRE(mon.isOnline());
}

TEST_CASE("NetworkMonitor: onOnline fires exactly once per recovery", "[monitor][state]") {
    std::atomic<int> onlineCount{0};
    std::atomic<bool> probeResult{false};

    NetworkMonitor mon{[&] { return probeResult.load(); }, [] {}, [&] { ++onlineCount; },
                       NetworkMonitor::Config{.probeInterval = 20ms, .failureThreshold = 1, .onlineThreshold = 1}};

    REQUIRE(waitUntil([&] { return !mon.isOnline(); }));
    probeResult.store(true);
    REQUIRE(waitUntil([&] { return onlineCount.load() >= 1; }));
    std::this_thread::sleep_for(150ms);

    REQUIRE(onlineCount.load() == 1);
}

TEST_CASE("NetworkMonitor: full cycle  -  online -> offline -> online fires both callbacks once each",
          "[monitor][state]") {
    std::atomic<int> offlineCount{0};
    std::atomic<int> onlineCount{0};
    std::atomic<bool> probeResult{true};

    NetworkMonitor mon{[&] { return probeResult.load(); }, [&] { ++offlineCount; }, [&] { ++onlineCount; },
                       NetworkMonitor::Config{.probeInterval = 20ms, .failureThreshold = 1, .onlineThreshold = 1}};

    // Go offline. Wait until the callback fires before touching probeResult
    // again — avoids a race where we store(true) before the offlineCount
    // check and the monitor fires onOnline before we assert onlineCount==0.
    probeResult.store(false);
    REQUIRE(waitUntil([&] { return offlineCount.load() == 1; }));
    REQUIRE(onlineCount.load() == 0);

    // Recover.
    probeResult.store(true);
    REQUIRE(waitUntil([&] { return onlineCount.load() == 1; }));
    REQUIRE(offlineCount.load() == 1);
}

TEST_CASE("NetworkMonitor: recovery below onlineThreshold does not fire onOnline", "[monitor][state]") {
    // Symmetric to "stays online below failureThreshold".
    // With onlineThreshold=3, only 2 consecutive successes should not trigger onOnline.
    std::atomic<int> onlineCount{0};
    std::atomic<int> probeCallCount{0};

    NetworkMonitor mon{[&] {
                           int cnt = ++probeCallCount;
                           // Fail first probe (go offline), then succeed probes 2 and 3, then fail again.
                           // Two consecutive successes < onlineThreshold=3, so onOnline must not fire.
                           return cnt == 2 || cnt == 3;
                       },
                       [] {}, [&] { ++onlineCount; },
                       NetworkMonitor::Config{.probeInterval = 20ms, .failureThreshold = 1, .onlineThreshold = 3}};

    // Wait long enough for probes 1–4 to all have run.
    std::this_thread::sleep_for(150ms);
    REQUIRE(onlineCount.load() == 0);
}

// ── Thread safety ─────────────────────────────────────────────────────────────

TEST_CASE("NetworkMonitor: callbacks do not run on caller thread", "[monitor][threading]") {
    const auto callerThreadId = std::this_thread::get_id();
    std::atomic<bool> ranOnDifferentThread{false};
    std::atomic<bool> probeResult{true};

    NetworkMonitor mon{[&] { return probeResult.load(); },
                       [&] { ranOnDifferentThread.store(std::this_thread::get_id() != callerThreadId); }, [] {},
                       NetworkMonitor::Config{.probeInterval = 20ms, .failureThreshold = 1}};

    probeResult.store(false);
    REQUIRE(waitUntil([&] { return ranOnDifferentThread.load(); }));
}

TEST_CASE("NetworkMonitor: isOnline safe to call from multiple threads concurrently", "[monitor][threading]") {
    NetworkMonitor mon{[] { return true; }, [] {}, [] {}, NetworkMonitor::Config{.probeInterval = 10ms}};

    std::vector<std::thread> readers;
    readers.reserve(8);
    std::atomic<int> reads{0};
    for (int i = 0; i < 8; ++i) {
        readers.emplace_back([&] {
            for (int j = 0; j < 100; ++j) {
                (void)mon.isOnline();
                ++reads;
            }
        });
    }
    for (auto& thr : readers) {
        thr.join();
    }
    REQUIRE(reads.load() == 800);
}

// ── Edge cases ────────────────────────────────────────────────────────────────

TEST_CASE("NetworkMonitor: null probe function is treated as offline", "[monitor][edge]") {
    // A null probe is a misconfiguration but must not crash. The monitor must
    // eventually transition to offline (null probe returns false every cycle).
    std::atomic<int> offlineCount{0};
    NetworkMonitor mon{nullptr, [&] { ++offlineCount; }, [] {},
                       NetworkMonitor::Config{.probeInterval = 20ms, .failureThreshold = 1}};
    // Wait for the null-probe path to fire and trigger the offline transition.
    REQUIRE(waitUntil([&] { return offlineCount.load() == 1; }));
    REQUIRE_FALSE(mon.isOnline());
}
