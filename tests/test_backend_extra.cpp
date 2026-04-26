// SPDX-License-Identifier: Apache-2.0

#include <async_framework/backend.hpp>
#include <async_framework/bridge.hpp>
#include <async_framework/executor.hpp>
#include <async_framework/registry.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <stdexcept>
#include <thread>

using namespace morph;

struct CounterAction {
    int delta = 0;
};
struct CounterModel {
    int value = 0;
    int execute(const CounterAction& act) {
        value += act.delta;
        return value;
    }
};

template <>
struct morph::ModelTraits<CounterModel> {
    static constexpr std::string_view typeId() { return "BE_CounterModel"; }
};
template <>
struct morph::ActionTraits<CounterAction> {
    using Result = int;
    static constexpr std::string_view typeId() { return "BE_CounterAction"; }
    static std::string toJson(const CounterAction& action) {
        std::string out;
        if (auto err = glz::write_json(action, out)) {
            throw morph::ParseError{glz::format_error(err, out)};
        }
        return out;
    }
    static CounterAction fromJson(std::string_view json) {
        CounterAction act{};
        if (auto err = glz::read_json(act, json)) {
            throw morph::ParseError{glz::format_error(err, json)};
        }
        return act;
    }
    static std::string resultToJson(const int& result) {
        std::string out;
        if (auto err = glz::write_json(result, out)) {
            throw morph::ParseError{glz::format_error(err, out)};
        }
        return out;
    }
    static int resultFromJson(std::string_view json) {
        int result{};
        if (auto err = glz::read_json(result, json)) {
            throw morph::ParseError{glz::format_error(err, json)};
        }
        return result;
    }
};

struct SyncExecutor : IExecutor {
    void post(std::function<void()> fn) override { fn(); }
};

// ── LocalBackend: model-not-found path ────────────────────────────────────────

TEST_CASE("LocalBackend: execute after deregisterModel delivers error", "[backend][local]") {
    ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    LocalBackend backend{pool};

    auto mid = backend.registerModel("BE_CounterModel", ModelFactory::create<CounterModel>);
    backend.deregisterModel(mid);

    // Build a minimal ActionCall that performs a local op
    ActionCall call;
    call.modelTypeId = "BE_CounterModel";
    call.actionTypeId = "BE_CounterAction";
    call.serializeAction = [] { return std::string{"{}"}; };
    call.deserializeResult = [](std::string_view) -> std::shared_ptr<void> { return {}; };
    call.localOp = [](IModelHolder&) -> std::shared_ptr<void> { return {}; };

    bool errorFired = false;
    backend.execute(mid, std::move(call), &cbExec)
        .then([](const std::shared_ptr<void>&) {})
        .onError([&](const std::exception_ptr& exc) {
            try {
                std::rethrow_exception(exc);
            } catch (const std::runtime_error&) {
                errorFired = true;
            }
        });

    for (int i = 0; i < 50 && !errorFired; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(errorFired);
}

// ── Bridge: deregisterHandler edge cases ─────────────────────────────────────

TEST_CASE("Bridge::deregisterHandler with already-zero currentId is a no-op", "[bridge]") {
    ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    Bridge bridge{std::make_unique<LocalBackend>(pool)};

    auto binding = std::make_shared<HandlerBinding>();
    binding->typeId = "BE_CounterModel";
    binding->modelFactory = ModelFactory::create<CounterModel>;
    binding->currentId.store(0);  // simulate unbound

    // Should not crash or call backend with id=0
    bridge.deregisterHandler(binding);
    REQUIRE(true);
}

TEST_CASE("Bridge::executeVia when handler currentId is zero returns error", "[bridge]") {
    ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    Bridge bridge{std::make_unique<LocalBackend>(pool)};

    // Manually create an unbound binding
    auto binding = std::make_shared<HandlerBinding>();
    binding->typeId = "BE_CounterModel";
    binding->modelFactory = ModelFactory::create<CounterModel>;
    binding->currentId.store(0);

    bool errorFired = false;
    bridge.executeVia<CounterModel, CounterAction>(binding, CounterAction{1}, &cbExec)
        .then([](int) {})
        .onError([&](const std::exception_ptr& exc) {
            try {
                std::rethrow_exception(exc);
            } catch (const std::runtime_error& ex) {
                errorFired = (std::string{ex.what()} == "handler not bound");
            }
        });

    REQUIRE(errorFired);
}

TEST_CASE("BridgeHandler destructor deregisters model cleanly", "[bridge]") {
    ThreadPoolExecutor pool{2};
    SyncExecutor cbExec;
    Bridge bridge{std::make_unique<LocalBackend>(pool)};

    std::atomic<int> result{-1};
    {
        BridgeHandler<CounterModel> handler{bridge, &cbExec};
        handler.execute(CounterAction{10})
            .then([&](int val) { result.store(val); })
            .onError([](const std::exception_ptr&) {});

        for (int i = 0; i < 50 && result.load() == -1; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // handler goes out of scope here — deregister must not crash
    }
    REQUIRE(result.load() == 10);
}
