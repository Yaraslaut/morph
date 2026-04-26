// SPDX-License-Identifier: Apache-2.0

#include <async_framework/bridge.hpp>
#include <async_framework/executor.hpp>
#include <async_framework/registry.hpp>
#include <async_framework/remote.hpp>
#include <iostream>
#include <thread>

using namespace morph;

struct IncrementAction {
    int by = 0;
};
struct FailingAction {};

struct CounterModel {
    int value = 0;
    int execute(IncrementAction action) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        value += action.by;
        return value;
    }
    int execute(FailingAction) { throw std::runtime_error("intentional failure from FailingAction"); }
};

BRIDGE_REGISTER_MODEL(CounterModel, "CounterModel")
BRIDGE_REGISTER_ACTION(CounterModel, IncrementAction, "IncrementAction")
BRIDGE_REGISTER_ACTION(CounterModel, FailingAction, "FailingAction")

namespace {
void runScenario(Bridge& bridge, MainThreadExecutor& gui, const char* label) {
    std::cout << "\n===== " << label << " =====\n";
    BridgeHandler<CounterModel> handler{bridge, &gui};
    std::atomic<int> successCount{0};

    for (int idx = 0; idx < 5; ++idx) {
        handler.execute(IncrementAction{idx + 1})
            .then([&](int val) {
                std::cout << "[gui] counter = " << val << "\n";
                successCount.fetch_add(1);
            })
            .onError([](const std::exception_ptr& exc) {
                try {
                    std::rethrow_exception(exc);
                } catch (const std::exception& ex) {
                    std::cout << "[gui] unexpected: " << ex.what() << "\n";
                }
            });
    }

    handler.execute(FailingAction{})
        .then([](int) { std::cout << "[gui] (should not fire)\n"; })
        .onError([](const std::exception_ptr& exc) {
            try {
                std::rethrow_exception(exc);
            } catch (const std::exception& ex) {
                std::cout << "[gui] caught: " << ex.what() << "\n";
            }
        });

    handler.execute(FailingAction{}).then([](int) {});

    gui.runFor(std::chrono::milliseconds(500));
    std::cout << "[main] " << successCount.load() << " increments completed\n";
}
}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
int main() {
    ThreadPoolExecutor clientPool{4};
    ThreadPoolExecutor serverPool{4};
    MainThreadExecutor gui;

    {
        Bridge bridge{std::make_unique<LocalBackend>(clientPool)};
        runScenario(bridge, gui, "LocalBackend");
    }
    {
        RemoteServer server{serverPool};
        Bridge bridge{std::make_unique<SimulatedRemoteBackend>(server)};
        runScenario(bridge, gui, "SimulatedRemoteBackend");
    }
    return 0;
}
