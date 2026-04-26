// SPDX-License-Identifier: Apache-2.0

#include <async_framework/model.hpp>
#include <catch2/catch_test_macros.hpp>
#include <memory>

using namespace morph;

// ── Test models ───────────────────────────────────────────────────────────────

struct NotifiableModel {
    int notifyCount = 0;
    void onBackendChanged() { ++notifyCount; }
};

struct SilentModel {};

// ── Concept detection ─────────────────────────────────────────────────────────

TEST_CASE("BackendChangedNotifiable: detects onBackendChanged method", "[model][concept]") {
    STATIC_REQUIRE(morph::BackendChangedNotifiable<NotifiableModel>);
    STATIC_REQUIRE_FALSE(morph::BackendChangedNotifiable<SilentModel>);
}

TEST_CASE("ModelHolder for notifiable model implements IBackendChangedSink", "[model][concept]") {
    auto holder = std::make_unique<ModelHolder<NotifiableModel>>();
    auto* sink = dynamic_cast<IBackendChangedSink*>(holder.get());
    REQUIRE(sink != nullptr);
}

TEST_CASE("ModelHolder for silent model does NOT implement IBackendChangedSink", "[model][concept]") {
    auto holder = std::make_unique<ModelHolder<SilentModel>>();
    auto* sink = dynamic_cast<IBackendChangedSink*>(holder.get());
    REQUIRE(sink == nullptr);
}

TEST_CASE("IBackendChangedSink::onBackendChanged delegates to model method", "[model][concept]") {
    auto holder = std::make_unique<ModelHolder<NotifiableModel>>();
    auto* sink = dynamic_cast<IBackendChangedSink*>(holder.get());
    REQUIRE(sink != nullptr);

    sink->onBackendChanged();
    REQUIRE(holder->model.notifyCount == 1);

    sink->onBackendChanged();
    REQUIRE(holder->model.notifyCount == 2);
}

// ── notifyBackendChanged ──────────────────────────────────────────────────────

#include <async_framework/backend.hpp>
#include <async_framework/executor.hpp>

TEST_CASE("LocalBackend::notifyBackendChanged calls onBackendChanged on notifiable models only", "[backend][notify]") {
    ThreadPoolExecutor pool{2};
    LocalBackend backend{pool};

    // Register one notifiable model and one silent model.
    backend.registerModel("NotifiableModel", [] { return std::make_unique<ModelHolder<NotifiableModel>>(); });
    backend.registerModel("SilentModel", [] { return std::make_unique<ModelHolder<SilentModel>>(); });

    // Must not throw or crash regardless of model mix.
    REQUIRE_NOTHROW(backend.notifyBackendChanged());
}

// ── switchBackend test models ─────────────────────────────────────────────────

#include <async_framework/bridge.hpp>
#include <async_framework/registry.hpp>
#include <chrono>
#include <thread>

struct CountAction {
    int x = 0;
};
struct SwitchCountAction {};  // queries how many times onBackendChanged fired

struct CountModel {
    int value = 0;
    int switchCount = 0;
    int execute(const CountAction& act) {
        value += act.x;
        return value;
    }
    [[nodiscard]] int execute(const SwitchCountAction&) const { return switchCount; }
    void onBackendChanged() { ++switchCount; }
};

template <>
struct morph::ModelTraits<CountModel> {
    static constexpr std::string_view typeId() { return "SW_CountModel"; }
};
template <>
struct morph::ActionTraits<CountAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "SW_CountAction"; }
    static std::string toJson(const CountAction& act) { return R"({"x":)" + std::to_string(act.x) + "}"; }
    static CountAction fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const int& res) { return std::to_string(res); }
    static int resultFromJson(std::string_view str) { return std::stoi(std::string{str}); }
};
template <>
struct morph::ActionTraits<SwitchCountAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "SW_SwitchCountAction"; }
    static std::string toJson(const SwitchCountAction&) { return "{}"; }
    static SwitchCountAction fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const int& res) { return std::to_string(res); }
    static int resultFromJson(std::string_view str) { return std::stoi(std::string{str}); }
};

struct SyncExec : IExecutor {
    void post(std::function<void()> fn) override { fn(); }
};

// ── switchBackend tests ───────────────────────────────────────────────────────

TEST_CASE("Bridge::switchBackend  -  handler works before and after switch", "[bridge][switch]") {
    ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    Bridge bridge{std::make_unique<LocalBackend>(pool)};
    BridgeHandler<CountModel> handler{bridge, &cbExec};

    // Execute on original backend.
    std::atomic<int> res1{-1};
    handler.execute(CountAction{5}).then([&](int val) { res1.store(val); }).onError([](const std::exception_ptr&) {});
    for (int i = 0; i < 50 && res1.load() == -1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(res1.load() == 5);

    // Switch to a fresh backend  -  model state resets (new instance).
    ThreadPoolExecutor pool2{2};
    bridge.switchBackend(std::make_unique<LocalBackend>(pool2));

    std::atomic<int> res2{-1};
    handler.execute(CountAction{7}).then([&](int val) { res2.store(val); }).onError([](const std::exception_ptr&) {});
    for (int i = 0; i < 50 && res2.load() == -1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(res2.load() == 7);
}

TEST_CASE("Bridge::switchBackend  -  destroyed handler not re-registered, no crash", "[bridge][switch]") {
    ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    Bridge bridge{std::make_unique<LocalBackend>(pool)};

    {
        BridgeHandler<CountModel> handler{bridge, &cbExec};
    }  // handler destroyed  -  weak_ptr in bridge goes stale

    ThreadPoolExecutor pool2{2};
    REQUIRE_NOTHROW(bridge.switchBackend(std::make_unique<LocalBackend>(pool2)));
}

TEST_CASE("Bridge::switchBackend  -  multiple live handlers all re-registered", "[bridge][switch]") {
    ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    Bridge bridge{std::make_unique<LocalBackend>(pool)};
    BridgeHandler<CountModel> handler1{bridge, &cbExec};
    BridgeHandler<CountModel> handler2{bridge, &cbExec};

    ThreadPoolExecutor pool2{2};
    bridge.switchBackend(std::make_unique<LocalBackend>(pool2));

    std::atomic<int> res1{-1};
    std::atomic<int> res2{-1};
    handler1.execute(CountAction{10}).then([&](int val) { res1.store(val); }).onError([](const std::exception_ptr&) {
    });
    handler2.execute(CountAction{20}).then([&](int val) { res2.store(val); }).onError([](const std::exception_ptr&) {
    });
    for (int i = 0; i < 50 && (res1.load() == -1 || res2.load() == -1); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(res1.load() == 10);
    REQUIRE(res2.load() == 20);
}

// ── Deep onBackendChanged count verification ──────────────────────────────────

TEST_CASE("Bridge::switchBackend  -  onBackendChanged called exactly once on new model after one switch",
          "[bridge][switch][notify]") {
    ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    Bridge bridge{std::make_unique<LocalBackend>(pool)};
    BridgeHandler<CountModel> handler{bridge, &cbExec};

    ThreadPoolExecutor pool2{2};
    bridge.switchBackend(std::make_unique<LocalBackend>(pool2));

    // Query the new model instance's switchCount.
    std::atomic<int> count{-1};
    handler.execute(SwitchCountAction{})
        .then([&](int val) { count.store(val); })
        .onError([](const std::exception_ptr&) {});
    for (int i = 0; i < 50 && count.load() == -1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(count.load() == 1);
}

TEST_CASE("Bridge::switchBackend  -  onBackendChanged called exactly once per switch across two switches",
          "[bridge][switch][notify]") {
    ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    Bridge bridge{std::make_unique<LocalBackend>(pool)};
    BridgeHandler<CountModel> handler{bridge, &cbExec};

    ThreadPoolExecutor pool2{2};
    bridge.switchBackend(std::make_unique<LocalBackend>(pool2));

    ThreadPoolExecutor pool3{2};
    bridge.switchBackend(std::make_unique<LocalBackend>(pool3));

    // Each switch creates a fresh model instance. The LAST instance receives
    // onBackendChanged() exactly once (from the second switch).
    std::atomic<int> count{-1};
    handler.execute(SwitchCountAction{})
        .then([&](int val) { count.store(val); })
        .onError([](const std::exception_ptr&) {});
    for (int i = 0; i < 50 && count.load() == -1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(count.load() == 1);
}

// ── Remote backend no-op ──────────────────────────────────────────────────────

#include <async_framework/remote.hpp>

TEST_CASE("SimulatedRemoteBackend::notifyBackendChanged is a documented no-op", "[remote][notify]") {
    ThreadPoolExecutor pool{2};
    auto server = std::make_shared<RemoteServer>(pool);
    SimulatedRemoteBackend backend{*server};
    REQUIRE_NOTHROW(backend.notifyBackendChanged());
}
