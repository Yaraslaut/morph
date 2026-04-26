// SPDX-License-Identifier: Apache-2.0

// Conflict-resolution integration tests.
//
// Core invariant tested: the framework fires onBackendChanged() on the model
// instance living in the NEW backend. All replay, conflict detection, and merge
// logic belongs to the model. The framework is untouched.
//
// Test model: OrderModel
//   Tracks results of onBackendChanged() in observable public counters.
//   The conflict checker and resolver are injected at construction so each
//   test can control the scenario without subclassing.

#include <async_framework/backend.hpp>
#include <async_framework/bridge.hpp>
#include <async_framework/executor.hpp>
#include <async_framework/offline_queue.hpp>
#include <async_framework/registry.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace morph;
using namespace std::chrono_literals;

// ── Infrastructure ────────────────────────────────────────────────────────────

struct SyncExec : IExecutor {
    void post(std::function<void()> fn) override { fn(); }
};

// ── Domain types ──────────────────────────────────────────────────────────────

struct OrderQueryAction {};  // read-only  -  returns notifyCount from the model instance

// ── OrderModel ────────────────────────────────────────────────────────────────
//
// The model holds a reference to a shared IOfflineQueue.
// onBackendChanged() drains the queue and processes each item:
//   - ConflictChecker(payload) → true  : conflict path
//   - ConflictResolver(payload) → ""   : discard (markDone, don't apply)
//   - ConflictResolver(payload) → text : merge (markDone, record merge)
//   - ConflictChecker(payload) → false : clean replay (markDone)
//
// All counters are plain ints  -  onBackendChanged runs on the backend strand
// (single-threaded per model), so no locking is needed here.

class OrderModel {
public:
    using ConflictChecker = std::function<bool(const std::string&)>;
    using ConflictResolver = std::function<std::string(const std::string&)>;

    OrderModel(IOfflineQueue& queue, ConflictChecker check, ConflictResolver resolve)
        : _queue{queue}, _check{std::move(check)}, _resolve{std::move(resolve)} {}

    [[nodiscard]] int execute(const OrderQueryAction&) const { return notifyCount; }

    void onBackendChanged() {
        ++notifyCount;
        for (auto& item : _queue.drain()) {
            if (_check(item.payload)) {
                std::string merged = _resolve(item.payload);
                if (merged.empty()) {
                    ++discarded;
                } else {
                    ++mergedCount;
                }
            } else {
                ++replayed;
            }
            _queue.markDone(item.id);  // always remove from queue after handling
        }
    }

    int notifyCount = 0;
    int replayed = 0;
    int mergedCount = 0;
    int discarded = 0;

private:
    IOfflineQueue& _queue;
    ConflictChecker _check;
    ConflictResolver _resolve;
};

// ── Traits ────────────────────────────────────────────────────────────────────

template <>
struct morph::ModelTraits<OrderModel> {
    static constexpr std::string_view typeId() { return "CR_OrderModel"; }
};

template <>
struct morph::ActionTraits<OrderQueryAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "CR_OrderQueryAction"; }
    static std::string toJson(const OrderQueryAction&) { return "{}"; }
    static OrderQueryAction fromJson(std::string_view) { return {}; }
    static std::string resultToJson(const int& res) { return std::to_string(res); }
    static int resultFromJson(std::string_view str) { return std::stoi(std::string{str}); }
};

// ── Factory helper ────────────────────────────────────────────────────────────
//
// Builds a HandlerBinding whose modelFactory captures the queue and resolution
// callables. Bridge uses this factory to create the model instance for each
// new backend  -  allowing the same queue and resolvers to be shared.

// queue must outlive the returned binding and all model instances created from it
// (captured by reference in the factory lambda).
static std::shared_ptr<HandlerBinding> makeOrderBinding(IOfflineQueue& queue, OrderModel::ConflictChecker check,
                                                        OrderModel::ConflictResolver resolve) {
    auto binding = std::make_shared<HandlerBinding>();
    binding->typeId = std::string{morph::ModelTraits<OrderModel>::typeId()};
    binding->modelFactory = [&queue, check = std::move(check),
                             resolve = std::move(resolve)]() mutable -> std::unique_ptr<IModelHolder> {
        return std::make_unique<ModelHolder<OrderModel>>(queue, check, resolve);
    };
    return binding;
}

// ── Helper: wait for a Completion<int> ───────────────────────────────────────

static int waitInt(auto completion) {
    std::atomic<int> result{-999};
    std::move(completion).then([&](int val) { result.store(val); }).onError([](const std::exception_ptr&) {});
    for (int i = 0; i < 100 && result.load() == -999; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    return result.load();
}

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_CASE("ConflictResolution: no conflicts  -  all items markDone on switchBackend", "[conflict]") {
    ThreadPoolExecutor pool1{2};
    ThreadPoolExecutor pool2{2};
    SyncExec cbExec;
    InMemoryOfflineQueue queue;

    queue.enqueue(R"({"amount":10})");
    queue.enqueue(R"({"amount":20})");
    queue.enqueue(R"({"amount":30})");

    auto binding = makeOrderBinding(
        queue, [](const std::string&) { return false; },      // no conflicts
        [](const std::string& payload) { return payload; });  // resolver never called

    Bridge bridge{std::make_unique<LocalBackend>(pool1)};
    BridgeHandler<OrderModel> handler{bridge, &cbExec, binding};

    bridge.switchBackend(std::make_unique<LocalBackend>(pool2));
    std::this_thread::sleep_for(80ms);  // let notifyBackendChanged complete

    // All items removed from queue after clean replay.
    REQUIRE(queue.drain().empty());

    // Query the new model instance's counters via execute.
    REQUIRE(waitInt(handler.execute(OrderQueryAction{})) == 1);  // notifyCount == 1
}

TEST_CASE("ConflictResolution: conflicting items discarded  -  resolver returns empty", "[conflict]") {
    ThreadPoolExecutor pool1{2};
    ThreadPoolExecutor pool2{2};
    SyncExec cbExec;
    InMemoryOfflineQueue queue;

    queue.enqueue("clean");
    queue.enqueue("CONFLICT");
    queue.enqueue("clean2");

    auto binding = makeOrderBinding(
        queue, [](const std::string& payload) { return payload.find("CONFLICT") != std::string::npos; },
        [](const std::string&) -> std::string { return ""; });  // discard

    Bridge bridge{std::make_unique<LocalBackend>(pool1)};
    BridgeHandler<OrderModel> handler{bridge, &cbExec, binding};

    bridge.switchBackend(std::make_unique<LocalBackend>(pool2));
    std::this_thread::sleep_for(80ms);

    // All three items removed regardless of outcome (discard also calls markDone).
    REQUIRE(queue.drain().empty());
}

TEST_CASE("ConflictResolution: conflicting items merged  -  resolver returns non-empty", "[conflict]") {
    ThreadPoolExecutor pool1{2};
    ThreadPoolExecutor pool2{2};
    SyncExec cbExec;
    InMemoryOfflineQueue queue;

    queue.enqueue("CONFLICT_A");
    queue.enqueue("CONFLICT_B");
    queue.enqueue("clean");

    auto binding = makeOrderBinding(
        queue, [](const std::string& payload) { return payload.find("CONFLICT") != std::string::npos; },
        [](const std::string&) -> std::string { return "merged_value"; });  // merge

    Bridge bridge{std::make_unique<LocalBackend>(pool1)};
    BridgeHandler<OrderModel> handler{bridge, &cbExec, binding};

    bridge.switchBackend(std::make_unique<LocalBackend>(pool2));
    std::this_thread::sleep_for(80ms);

    // All three items processed and removed.
    REQUIRE(queue.drain().empty());
}

TEST_CASE("ConflictResolution: framework fires onBackendChanged exactly once per switchBackend", "[conflict]") {
    // Verifies the framework invariant: each switchBackend() call triggers
    // exactly one onBackendChanged() on the new model instance.
    // We switch three times and confirm notifyCount == 1 on each new instance
    // (each switch creates a fresh model  -  its own notifyCount starts at 0).

    ThreadPoolExecutor pool1{2};
    ThreadPoolExecutor pool2{2};
    ThreadPoolExecutor pool3{2};
    ThreadPoolExecutor pool4{2};
    SyncExec cbExec;
    InMemoryOfflineQueue queue;

    auto binding = makeOrderBinding(
        queue, [](const std::string&) { return false; }, [](const std::string& payload) { return payload; });

    Bridge bridge{std::make_unique<LocalBackend>(pool1)};
    BridgeHandler<OrderModel> handler{bridge, &cbExec, binding};

    // Switch once  -  new model instance created, notifyCount becomes 1.
    bridge.switchBackend(std::make_unique<LocalBackend>(pool2));
    std::this_thread::sleep_for(80ms);
    REQUIRE(waitInt(handler.execute(OrderQueryAction{})) == 1);

    // Switch again  -  another fresh instance, again notifyCount == 1.
    bridge.switchBackend(std::make_unique<LocalBackend>(pool3));
    std::this_thread::sleep_for(80ms);
    REQUIRE(waitInt(handler.execute(OrderQueryAction{})) == 1);

    // Third switch.
    bridge.switchBackend(std::make_unique<LocalBackend>(pool4));
    std::this_thread::sleep_for(80ms);
    REQUIRE(waitInt(handler.execute(OrderQueryAction{})) == 1);
}

TEST_CASE("ConflictResolution: full offline scenario  -  accumulate offline, sync on reconnect",
          "[conflict][integration]") {
    // Real-world scenario:
    //   1. App starts offline (local backend).
    //   2. Three orders are placed offline  -  payloads enqueued manually
    //      (representing what the app would have queued).
    //   3. One payload is "stale"  -  server says it conflicts with an update
    //      made by another user while we were offline.
    //   4. Network recovers  -  switchBackend fires onBackendChanged.
    //   5. Model replays two clean items, merges one conflict, removes all from queue.
    //   6. Queue is empty; model is live on the new backend; execute still works.

    ThreadPoolExecutor localPool{2};
    ThreadPoolExecutor remotePool{2};
    SyncExec cbExec;
    InMemoryOfflineQueue queue;

    // Simulate three offline writes.
    queue.enqueue(R"({"item":"order_A"})");        // clean
    queue.enqueue(R"({"item":"order_B_stale"})");  // stale  -  conflicts with server
    queue.enqueue(R"({"item":"order_C"})");        // clean

    auto binding = makeOrderBinding(
        queue,
        // Conflict checker: payloads containing "stale" were superseded.
        [](const std::string& payload) { return payload.find("stale") != std::string::npos; },
        // Resolver: take server version (non-empty → merge applied).
        [](const std::string&) -> std::string { return R"({"item":"order_B_server"})"; });

    Bridge bridge{std::make_unique<LocalBackend>(localPool)};
    BridgeHandler<OrderModel> handler{bridge, &cbExec, binding};

    // Simulate reconnection  -  switch to remote backend.
    bridge.switchBackend(std::make_unique<LocalBackend>(remotePool));
    std::this_thread::sleep_for(80ms);

    // Queue fully drained: 2 clean replays + 1 merge = 3 markDone calls.
    REQUIRE(queue.drain().empty());

    // New model is live  -  execute works.
    REQUIRE(waitInt(handler.execute(OrderQueryAction{})) == 1);
}
