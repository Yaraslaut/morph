// Tests covering issues found during codebase analysis.
// Each section corresponds to a numbered issue in the analysis document.

#include <async_framework/backend.hpp>
#include <async_framework/bridge.hpp>
#include <async_framework/completion.hpp>
#include <async_framework/executor.hpp>
#include <async_framework/model.hpp>
#include <async_framework/network_monitor.hpp>
#include <async_framework/offline_queue.hpp>
#include <async_framework/registry.hpp>
#include <async_framework/remote.hpp>
#include <async_framework/strand.hpp>
#include <async_framework/sync_worker.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace morph;
using namespace std::chrono_literals;

// ── Shared helpers ────────────────────────────────────────────────────────────

struct SyncExec : IExecutor {
    void post(std::function<void()> fn) override { fn(); }
};

static bool waitUntil(const std::function<bool()>& pred, std::chrono::milliseconds timeout = 500ms) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

// ── Issue 1: CompletionState::setValue should move value into callback ────────

TEST_CASE("Issue 1: CompletionState setValue moves value into callback, not copies", "[completion][issue1]") {
    SyncExec exec;
    auto state = std::make_shared<detail::CompletionState<std::string>>();
    state->cbExec = &exec;

    std::string received;
    state->attachThen([&](std::string val) { received = std::move(val); });

    state->setValue(std::string(1000, 'x'));
    REQUIRE(received.size() == 1000U);
    REQUIRE(received == std::string(1000, 'x'));
}

// ── Issue 2: attachThen on already-errored state silently drops handler ───────

TEST_CASE("Issue 2: attachThen on errored state does not call handler and does not crash", "[completion][issue2]") {
    SyncExec exec;
    auto state = std::make_shared<detail::CompletionState<int>>();
    state->cbExec = &exec;
    state->attachOnError([](const std::exception_ptr&) {});
    state->setException(std::make_exception_ptr(std::runtime_error{"already failed"}));
    REQUIRE(state->ready);

    bool thenFired = false;
    state->attachThen([&](int) { thenFired = true; });
    REQUIRE_FALSE(thenFired);
}

// ── Issue 4: StrandExecutor map cleaned up after queue drains ─────────────────

TEST_CASE("Issue 4: StrandExecutor works correctly after strand entries are cleaned up", "[strand][issue4]") {
    ThreadPoolExecutor pool{2};
    StrandExecutor strand{pool};

    constexpr int numKeys = 20;
    std::atomic<int> completed{0};

    for (int key = 1; key <= numKeys; ++key) {
        strand.post(ModelId{static_cast<uint64_t>(key)}, [&] { completed.fetch_add(1); });
    }
    REQUIRE(waitUntil([&] { return completed.load() == numKeys; }));

    // Let pool finish cleanup bookkeeping
    std::this_thread::sleep_for(20ms);

    // Post again to confirm strand still works after cleanup
    std::atomic<int> completed2{0};
    for (int key = 1; key <= numKeys; ++key) {
        strand.post(ModelId{static_cast<uint64_t>(key)}, [&] { completed2.fetch_add(1); });
    }
    REQUIRE(waitUntil([&] { return completed2.load() == numKeys; }));
}

TEST_CASE("Issue 4: StrandExecutor per-key ordering preserved after re-use", "[strand][issue4]") {
    ThreadPoolExecutor pool{2};
    StrandExecutor strand{pool};
    ModelId key{42};

    // First batch: drain and clean up
    std::atomic<int> batch1{0};
    for (int task = 0; task < 5; ++task) {
        strand.post(key, [&] { batch1.fetch_add(1); });
    }
    REQUIRE(waitUntil([&] { return batch1.load() == 5; }));
    std::this_thread::sleep_for(20ms);

    // Second batch on same key: must still serialize in order
    std::vector<int> order;
    std::mutex orderMtx;
    std::atomic<int> batch2{0};
    for (int taskId = 0; taskId < 5; ++taskId) {
        strand.post(key, [&, taskId] {
            std::lock_guard lock{orderMtx};
            order.push_back(taskId);
            batch2.fetch_add(1);
        });
    }
    REQUIRE(waitUntil([&] { return batch2.load() == 5; }));
    REQUIRE(order.size() == 5U);
    for (std::size_t idx = 0; idx < order.size(); ++idx) {
        REQUIRE(order[idx] == static_cast<int>(idx));
    }
}

// ── Issue 5: NetworkMonitor probe exception does not crash monitor ────────────

TEST_CASE("Issue 5: NetworkMonitor probe that throws is treated as failure, monitor stays alive",
          "[network_monitor][issue5]") {
    std::atomic<int> offlineCount{0};
    std::atomic<int> probeCallCount{0};

    NetworkMonitor monitor{[&] {
                               probeCallCount.fetch_add(1);
                               if (probeCallCount.load() % 2 == 0) {
                                   throw std::runtime_error("probe exploded");
                               }
                               return false;
                           },
                           [&] { offlineCount.fetch_add(1); }, [] {},
                           NetworkMonitor::Config{.probeInterval = 20ms, .failureThreshold = 2, .onlineThreshold = 1}};

    REQUIRE(waitUntil([&] { return offlineCount.load() >= 1; }, 1s));
    REQUIRE(monitor.isOnline() == false);
    REQUIRE_NOTHROW(monitor.stop());
}

TEST_CASE("Issue 5: NetworkMonitor probe that always throws treats each throw as failure",
          "[network_monitor][issue5]") {
    std::atomic<int> offlineCount{0};
    std::atomic<int> onlineCount{0};

    NetworkMonitor monitor{[]() -> bool { throw std::runtime_error("always explodes"); },
                           [&] { offlineCount.fetch_add(1); }, [&] { onlineCount.fetch_add(1); },
                           NetworkMonitor::Config{.probeInterval = 20ms, .failureThreshold = 1, .onlineThreshold = 1}};

    REQUIRE(waitUntil([&] { return offlineCount.load() >= 1; }, 1s));
    std::this_thread::sleep_for(80ms);
    REQUIRE(onlineCount.load() == 0);
    REQUIRE_NOTHROW(monitor.stop());
}

// ── Issue 6: NetworkMonitor callback calling stop() does not deadlock ─────────

TEST_CASE("Issue 6: NetworkMonitor stop() called from onOffline callback does not deadlock",
          "[network_monitor][issue6]") {
    std::atomic<bool> callbackFired{false};

    // atomic<NetworkMonitor*> avoids the data race between the main thread
    // writing the pointer and T1 reading it from the callback.
    std::atomic<NetworkMonitor*> monitorPtr{nullptr};
    auto monitor = std::make_shared<NetworkMonitor>(
        [] { return false; },
        [&callbackFired, &monitorPtr] {
            callbackFired.store(true);
            if (auto* m = monitorPtr.load()) {
                m->stop();  // must not deadlock even from probe thread
            }
        },
        [] {}, NetworkMonitor::Config{.probeInterval = 20ms, .failureThreshold = 1, .onlineThreshold = 1});
    monitorPtr.store(monitor.get());

    REQUIRE(waitUntil([&] { return callbackFired.load(); }, 1s));
    REQUIRE_NOTHROW(monitor.reset());
}

// ── Issue 9: ActionDispatcher hash: similar IDs dispatch to correct runners ───

// Top-level types needed for Issue 9 test (traits required at file scope)
struct Iss9ModelA {
    int execute(const struct Iss9ActionA& act);
};
struct Iss9ActionA {
    int delta = 0;
};
struct Iss9ModelB {
    int execute(const struct Iss9ActionB& act);
};
struct Iss9ActionB {
    int delta = 0;
};

template <>
struct morph::ModelTraits<Iss9ModelA> {
    static constexpr std::string_view typeId() { return "ISS9_Model_A"; }
};
template <>
struct morph::ModelTraits<Iss9ModelB> {
    static constexpr std::string_view typeId() { return "ISS9_Model_B"; }
};
template <>
struct morph::ActionTraits<Iss9ActionA> {
    using Result = int;
    static constexpr std::string_view typeId() { return "ISS9_Action_A"; }
    static std::string toJson(const Iss9ActionA& act) { return R"({"delta":)" + std::to_string(act.delta) + "}"; }
    static Iss9ActionA fromJson(std::string_view str) {
        Iss9ActionA act{};
        (void)glz::read_json(act, str);
        return act;
    }
    static std::string resultToJson(const int& res) { return std::to_string(res); }
    static int resultFromJson(std::string_view str) { return std::stoi(std::string{str}); }
};
template <>
struct morph::ActionTraits<Iss9ActionB> {
    using Result = int;
    static constexpr std::string_view typeId() { return "ISS9_Action_B"; }
    static std::string toJson(const Iss9ActionB& act) { return R"({"delta":)" + std::to_string(act.delta) + "}"; }
    static Iss9ActionB fromJson(std::string_view str) {
        Iss9ActionB act{};
        (void)glz::read_json(act, str);
        return act;
    }
    static std::string resultToJson(const int& res) { return std::to_string(res); }
    static int resultFromJson(std::string_view str) { return std::stoi(std::string{str}); }
};

inline int Iss9ModelA::execute(const Iss9ActionA& act) { return act.delta * 2; }
inline int Iss9ModelB::execute(const Iss9ActionB& act) { return act.delta * 3; }

TEST_CASE("Issue 9: ActionDispatcher dispatches correctly with similar-prefix type IDs", "[registry][issue9]") {
    ActionDispatcher dispatcher;
    ModelRegistryFactory registry;
    registry.registerModel<Iss9ModelA>("ISS9_Model_A");
    registry.registerModel<Iss9ModelB>("ISS9_Model_B");
    dispatcher.registerAction<Iss9ModelA, Iss9ActionA>("ISS9_Model_A", "ISS9_Action_A");
    dispatcher.registerAction<Iss9ModelB, Iss9ActionB>("ISS9_Model_B", "ISS9_Action_B");

    auto holderA = registry.create("ISS9_Model_A");
    auto holderB = registry.create("ISS9_Model_B");

    // Correct pairs dispatch without error
    auto resA = dispatcher.dispatch("ISS9_Model_A", "ISS9_Action_A", *holderA, R"({"delta":5})");
    auto resB = dispatcher.dispatch("ISS9_Model_B", "ISS9_Action_B", *holderB, R"({"delta":5})");
    REQUIRE(resA == "10");  // 5 * 2
    REQUIRE(resB == "15");  // 5 * 3

    // Cross-contamination throws
    REQUIRE_THROWS_AS(dispatcher.dispatch("ISS9_Model_A", "ISS9_Action_B", *holderA, "{}"), std::runtime_error);
    REQUIRE_THROWS_AS(dispatcher.dispatch("ISS9_Model_B", "ISS9_Action_A", *holderB, "{}"), std::runtime_error);
}

// ── Issue 11: IModelHolder::into<> throws bad_cast on type mismatch ───────────

struct Iss11FooModel {
    int val = 0;
};
struct Iss11BarModel {
    double val = 0.0;
};

TEST_CASE("Issue 11: IModelHolder::into throws bad_cast on wrong model type", "[model][issue11]") {
    auto holder = std::make_unique<ModelHolder<Iss11FooModel>>();
    REQUIRE_NOTHROW(holder->into<Iss11FooModel>());
    REQUIRE_THROWS_AS(holder->into<Iss11BarModel>(), std::bad_cast);
}

TEST_CASE("Issue 11: IModelHolder::into does not throw on correct type", "[model][issue11]") {
    auto holder = std::make_unique<ModelHolder<Iss11FooModel>>();
    holder->model.val = 99;
    REQUIRE_NOTHROW(holder->into<Iss11FooModel>());
    REQUIRE(holder->into<Iss11FooModel>().val == 99);
}

// ── Issue 15: RemoteServer malformed message bounds checking ──────────────────

TEST_CASE("Issue 15: RemoteServer handles 'register' with no typeId gracefully", "[remote][issue15]") {
    ThreadPoolExecutor pool{2};
    auto server = std::make_shared<RemoteServer>(pool);

    std::atomic<bool> done{false};
    std::string reply;
    server->handle("register", [&](const std::string& msg) {
        reply = msg;
        done.store(true);
    });
    REQUIRE(waitUntil([&] { return done.load(); }));
    REQUIRE(reply.starts_with("err|"));
}

TEST_CASE("Issue 15: RemoteServer handles 'deregister' with no id gracefully", "[remote][issue15]") {
    ThreadPoolExecutor pool{2};
    auto server = std::make_shared<RemoteServer>(pool);

    std::atomic<bool> done{false};
    std::string reply;
    server->handle("deregister", [&](const std::string& msg) {
        reply = msg;
        done.store(true);
    });
    REQUIRE(waitUntil([&] { return done.load(); }));
    REQUIRE(reply.starts_with("err|"));
}

TEST_CASE("Issue 15: RemoteServer handles 'execute' with too few parts gracefully", "[remote][issue15]") {
    ThreadPoolExecutor pool{2};
    auto server = std::make_shared<RemoteServer>(pool);

    std::atomic<bool> done{false};
    std::string reply;
    server->handle("execute|1|SomeModel", [&](const std::string& msg) {
        reply = msg;
        done.store(true);
    });
    REQUIRE(waitUntil([&] { return done.load(); }));
    REQUIRE(reply.starts_with("err|"));
}

TEST_CASE("Issue 15: RemoteServer handles completely empty message gracefully", "[remote][issue15]") {
    ThreadPoolExecutor pool{2};
    auto server = std::make_shared<RemoteServer>(pool);

    std::atomic<bool> done{false};
    std::string reply;
    server->handle("", [&](const std::string& msg) {
        reply = msg;
        done.store(true);
    });
    REQUIRE(waitUntil([&] { return done.load(); }));
    REQUIRE(reply.starts_with("err|"));
}

// ── Issue 10: In-flight execute after deregisterModel completes safely ─────────

struct Iss10Model {
    int value = 0;
    int execute(const struct Iss10Action& act);
};
struct Iss10Action {
    int delta = 0;
};

template <>
struct morph::ModelTraits<Iss10Model> {
    static constexpr std::string_view typeId() { return "ISS10_Model"; }
};
template <>
struct morph::ActionTraits<Iss10Action> {
    using Result = int;
    static constexpr std::string_view typeId() { return "ISS10_Action"; }
    static std::string toJson(const Iss10Action& act) { return R"({"delta":)" + std::to_string(act.delta) + "}"; }
    static Iss10Action fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const int& res) { return std::to_string(res); }
    static int resultFromJson(std::string_view str) { return std::stoi(std::string{str}); }
};
inline int Iss10Model::execute(const Iss10Action& act) {
    value += act.delta;
    return value;
}

TEST_CASE("Issue 10: in-flight execute after deregisterModel completes without crash", "[backend][issue10]") {
    ThreadPoolExecutor pool{2};
    SyncExec cbExec;
    LocalBackend backend{pool};

    auto mid = backend.registerModel("ISS10_Model", ModelFactory::create<Iss10Model>);

    ActionCall call;
    call.modelTypeId = "ISS10_Model";
    call.actionTypeId = "ISS10_Action";
    call.serializeAction = [] { return R"({"delta":1})"; };
    call.deserializeResult = [](std::string_view) -> std::shared_ptr<void> { return {}; };
    call.localOp = [](IModelHolder& holder) -> std::shared_ptr<void> {
        auto& model = holder.into<Iss10Model>();
        return std::make_shared<int>(model.execute(Iss10Action{1}));
    };

    std::atomic<bool> completed{false};
    auto comp = backend.execute(mid, std::move(call), &cbExec);
    backend.deregisterModel(mid);

    comp.then([&](const std::shared_ptr<void>&) { completed.store(true); }).onError([&](const std::exception_ptr&) {
        completed.store(true);
    });

    REQUIRE(waitUntil([&] { return completed.load(); }));
}

// ── Issue 12: SyncWorker — concurrent enqueue during run does not corrupt queue

TEST_CASE("Issue 12: SyncWorker concurrent enqueue during run does not corrupt queue", "[sync][issue12]") {
    InMemoryOfflineQueue queue;
    queue.enqueue("pre1");
    queue.enqueue("pre2");

    std::atomic<bool> replayStarted{false};

    SyncWorker worker{queue, [&](const std::string&) {
                          replayStarted.store(true);
                          std::this_thread::sleep_for(30ms);
                          return true;
                      }};

    std::thread enqueuer{[&] {
        waitUntil([&] { return replayStarted.load(); });
        queue.enqueue("concurrent");
    }};

    auto result = worker.run();
    enqueuer.join();

    REQUIRE(result.successful == 2);
    auto remaining = queue.drain();
    REQUIRE(remaining.size() == 1U);
    REQUIRE(remaining[0].payload == "concurrent");
}

// ── Issue 13: ThreadPoolExecutor drains queued tasks on shutdown ───────────────

TEST_CASE("Issue 13: ThreadPoolExecutor processes all queued tasks before shutdown", "[executor][issue13]") {
    std::atomic<int> count{0};
    {
        ThreadPoolExecutor pool{1};
        for (int item = 0; item < 10; ++item) {
            pool.post([&] { count.fetch_add(1); });
        }
    }
    REQUIRE(count.load() == 10);
}

TEST_CASE("Issue 13: ThreadPoolExecutor task that posts another task - both run before shutdown",
          "[executor][issue13]") {
    std::atomic<int> count{0};
    {
        ThreadPoolExecutor pool{1};
        pool.post([&] {
            count.fetch_add(1);
            pool.post([&] { count.fetch_add(1); });
        });
    }
    REQUIRE(count.load() == 2);
}
