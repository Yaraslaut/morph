// SPDX-License-Identifier: Apache-2.0

#include <async_framework/logger.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <print>
#include <string>
#include <thread>
#include <vector>

using namespace morph;

// RAII guard: restores logger + level after each test so tests are isolated.
struct LogGuard {
    LogLevel savedLevel = getLogLevel();
    Logger savedSink;

    LogGuard() {
        std::lock_guard lock{detail::logState().mtx};
        savedLevel = detail::logState().minLevel;
        savedSink = detail::logState().sink;
    }
    ~LogGuard() {
        std::lock_guard lock{detail::logState().mtx};
        detail::logState().minLevel = savedLevel;
        detail::logState().sink = std::move(savedSink);
    }
    LogGuard(const LogGuard&) = delete;
    LogGuard& operator=(const LogGuard&) = delete;
};

// ── levelName ─────────────────────────────────────────────────────────────────

TEST_CASE("levelName returns correct label for every level", "[logger]") {
    REQUIRE(levelName(LogLevel::debug) == "DEBUG");
    REQUIRE(levelName(LogLevel::info) == "INFO ");
    REQUIRE(levelName(LogLevel::warn) == "WARN ");
    REQUIRE(levelName(LogLevel::error) == "ERROR");
    REQUIRE(levelName(LogLevel::off) == "OFF  ");
}

// ── setLogger / custom sink ───────────────────────────────────────────────────

TEST_CASE("setLogger: custom sink receives level and message", "[logger]") {
    LogGuard guard;
    LogLevel capturedLevel = LogLevel::off;
    std::string capturedMsg;

    setLogger([&](LogLevel lvl, std::string_view msg) {
        capturedLevel = lvl;
        capturedMsg = std::string{msg};
    });

    logInfo("hello info");
    REQUIRE(capturedLevel == LogLevel::info);
    REQUIRE(capturedMsg == "hello info");
}

namespace {
struct LogEntry {
    LogLevel level;
    std::string msg;
};
struct SpySink {
    std::vector<LogEntry> entries;
    void write(LogLevel lvl, std::string_view msg) { entries.push_back({.level = lvl, .msg = std::string{msg}}); }
};
}  // namespace

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST_CASE("setLogger: injecting a custom backend (dependency injection)", "[logger]") {
    LogGuard guard;
    SpySink spy;
    setLogger([&spy](LogLevel lvl, std::string_view msg) { spy.write(lvl, msg); });
    setLogLevel(LogLevel::debug);

    logDebug("startup complete");
    logInfo("model registered");
    logWarn("slow action");
    logError("action failed");

    REQUIRE(spy.entries.size() == 4);
    REQUIRE(spy.entries[0].level == LogLevel::debug);
    REQUIRE(spy.entries[0].msg == "startup complete");
    REQUIRE(spy.entries[1].level == LogLevel::info);
    REQUIRE(spy.entries[1].msg == "model registered");
    REQUIRE(spy.entries[2].level == LogLevel::warn);
    REQUIRE(spy.entries[2].msg == "slow action");
    REQUIRE(spy.entries[3].level == LogLevel::error);
    REQUIRE(spy.entries[3].msg == "action failed");
}

TEST_CASE("setLogger: null sink is a no-op and does not crash", "[logger]") {
    LogGuard guard;
    setLogger(nullptr);
    logError("silent");
    REQUIRE(true);
}

// ── setLogLevel / filtering ───────────────────────────────────────────────────

TEST_CASE("setLogLevel: messages below threshold are suppressed", "[logger]") {
    LogGuard guard;
    int callCount = 0;
    setLogger([&](LogLevel, std::string_view) { ++callCount; });
    setLogLevel(LogLevel::warn);

    logDebug("not emitted");
    logInfo("not emitted");
    logWarn("emitted");
    logError("emitted");

    REQUIRE(callCount == 2);
}

TEST_CASE("setLogLevel: off suppresses everything", "[logger]") {
    LogGuard guard;
    int callCount = 0;
    setLogger([&](LogLevel, std::string_view) { ++callCount; });
    setLogLevel(LogLevel::off);

    logDebug("x");
    logInfo("x");
    logWarn("x");
    logError("x");

    REQUIRE(callCount == 0);
}

TEST_CASE("setLogLevel: debug passes all levels through", "[logger]") {
    LogGuard guard;
    int callCount = 0;
    setLogger([&](LogLevel, std::string_view) { ++callCount; });
    setLogLevel(LogLevel::debug);

    logDebug("a");
    logInfo("b");
    logWarn("c");
    logError("d");

    REQUIRE(callCount == 4);
}

TEST_CASE("getLogLevel returns the currently configured level", "[logger]") {
    LogGuard guard;
    setLogLevel(LogLevel::warn);
    REQUIRE(getLogLevel() == LogLevel::warn);

    setLogLevel(LogLevel::error);
    REQUIRE(getLogLevel() == LogLevel::error);
}

// ── Level helpers emit correct LogLevel ───────────────────────────────────────

TEST_CASE("logDebug/Info/Warn/Error pass the right LogLevel to the sink", "[logger]") {
    LogGuard guard;
    setLogLevel(LogLevel::debug);

    std::vector<LogLevel> captured;
    setLogger([&](LogLevel lvl, std::string_view) { captured.push_back(lvl); });

    logDebug("d");
    logInfo("i");
    logWarn("w");
    logError("e");

    REQUIRE(captured.size() == 4);
    REQUIRE(captured[0] == LogLevel::debug);
    REQUIRE(captured[1] == LogLevel::info);
    REQUIRE(captured[2] == LogLevel::warn);
    REQUIRE(captured[3] == LogLevel::error);
}

// ── log(level, msg) core overload ─────────────────────────────────────────────

TEST_CASE("log(level, msg) respects threshold and delegates to sink", "[logger]") {
    LogGuard guard;
    setLogLevel(LogLevel::error);

    std::string last;
    setLogger([&](LogLevel, std::string_view msg) { last = std::string{msg}; });

    log(LogLevel::warn, "suppressed");
    REQUIRE(last.empty());

    log(LogLevel::error, "emitted");
    REQUIRE(last == "emitted");
}

// ── Thread safety ─────────────────────────────────────────────────────────────

TEST_CASE("concurrent log calls are thread-safe", "[logger]") {
    LogGuard guard;
    std::atomic<int> count{0};
    setLogger([&](LogLevel, std::string_view) { count.fetch_add(1, std::memory_order_relaxed); });
    setLogLevel(LogLevel::debug);

    constexpr int numThreads = 8;
    constexpr int msgsPerThread = 200;
    std::vector<std::thread> threads;
    threads.reserve(numThreads);

    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back([&] {
            for (int j = 0; j < msgsPerThread; ++j) {
                logDebug("d");
                logInfo("i");
                logWarn("w");
                logError("e");
            }
        });
    }
    for (auto& thr : threads) {
        thr.join();
    }

    REQUIRE(count.load() == numThreads * msgsPerThread * 4);
}
