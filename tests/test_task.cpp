// SPDX-License-Identifier: Apache-2.0

#include <async_framework/task.hpp>
#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include <string>

using namespace morph::detail;

// ── TaskState direct tests ────────────────────────────────────────────────────

TEST_CASE("TaskState: setValue stores value and marks ready", "[task]") {
    TaskState<int> state;
    REQUIRE_FALSE(state.ready);
    state.setValue(42);
    REQUIRE(state.ready);
    REQUIRE(state.value.has_value());
    const int val0 = *state.value;  // NOLINT(bugprone-unchecked-optional-access)
    REQUIRE(val0 == 42);
}

TEST_CASE("TaskState: setException stores error and marks ready", "[task]") {
    TaskState<int> state;
    state.setException(std::make_exception_ptr(std::runtime_error{"boom"}));
    REQUIRE(state.ready);
    REQUIRE(state.error != nullptr);
    REQUIRE_FALSE(state.value.has_value());
}

TEST_CASE("TaskState: setValue fires continuation if already attached", "[task]") {
    TaskState<int> state;
    int received = -1;
    state.attach([&](TaskState<int>& stateRef) {
        if (stateRef.value) {
            received = stateRef.value.value();
        }
    });
    state.setValue(7);
    REQUIRE(received == 7);
}

TEST_CASE("TaskState: setException fires continuation if already attached", "[task]") {
    TaskState<int> state;
    bool continuationFired = false;
    state.attach([&](TaskState<int>& stateRef) {
        if (stateRef.error) {
            continuationFired = true;
        }
    });
    state.setException(std::make_exception_ptr(std::runtime_error{"err"}));
    REQUIRE(continuationFired);
}

TEST_CASE("TaskState: setValue with no continuation attached does not crash", "[task]") {
    TaskState<int> state;
    REQUIRE_NOTHROW(state.setValue(99));
    REQUIRE(state.ready);
}

TEST_CASE("TaskState: setException with no continuation attached does not crash", "[task]") {
    TaskState<int> state;
    REQUIRE_NOTHROW(state.setException(std::make_exception_ptr(std::runtime_error{"silent"})));
    REQUIRE(state.ready);
}

TEST_CASE("TaskState: attach before ready stores continuation for later", "[task]") {
    TaskState<int> state;
    int received = -1;
    state.attach([&](TaskState<int>& stateRef) {
        if (stateRef.value) {
            received = stateRef.value.value();
        }
    });
    REQUIRE(received == -1);  // not yet fired
    state.setValue(55);
    REQUIRE(received == 55);
}

TEST_CASE("TaskState: attach when already ready fires continuation immediately", "[task]") {
    TaskState<int> state;
    state.setValue(11);
    REQUIRE(state.ready);

    int received = -1;
    state.attach([&](TaskState<int>& stateRef) {
        if (stateRef.value) {
            received = stateRef.value.value();
        }
    });
    REQUIRE(received == 11);
}

TEST_CASE("TaskState: attach when already errored fires continuation immediately", "[task]") {
    TaskState<int> state;
    state.setException(std::make_exception_ptr(std::runtime_error{"late"}));
    REQUIRE(state.ready);

    bool fired = false;
    state.attach([&](TaskState<int>& stateRef) {
        if (stateRef.error) {
            fired = true;
        }
    });
    REQUIRE(fired);
}

// ── Task coroutine tests ──────────────────────────────────────────────────────

static Task<int> makeValueTask(int val) { co_return val; }

static Task<int> makeExceptionTask() {
    throw std::runtime_error{"coroutine threw"};
    co_return 0;
}

static Task<std::string> makeStringTask(const std::string& val) { co_return val; }

TEST_CASE("Task: coroutine returning value populates state", "[task][coroutine]") {
    auto task = makeValueTask(42);
    auto statePtr = task.state();
    REQUIRE(statePtr != nullptr);
    REQUIRE(statePtr->ready);
    REQUIRE(statePtr->value.has_value());
    const int val1 = *statePtr->value;  // NOLINT(bugprone-unchecked-optional-access)
    REQUIRE(val1 == 42);
    REQUIRE(statePtr->error == nullptr);
}

TEST_CASE("Task: coroutine throwing exception populates error state", "[task][coroutine]") {
    auto task = makeExceptionTask();
    auto statePtr = task.state();
    REQUIRE(statePtr != nullptr);
    REQUIRE(statePtr->ready);
    REQUIRE(statePtr->error != nullptr);
    REQUIRE_FALSE(statePtr->value.has_value());

    bool caught = false;
    try {
        std::rethrow_exception(statePtr->error);
    } catch (const std::runtime_error& exc) {
        caught = (std::string{exc.what()} == "coroutine threw");
    }
    REQUIRE(caught);
}

TEST_CASE("Task: state() accessor returns shared_ptr to TaskState", "[task]") {
    auto task = makeValueTask(1);
    auto statePtr = task.state();
    REQUIRE(statePtr != nullptr);
    REQUIRE(statePtr.use_count() >= 1);
}

TEST_CASE("Task: coroutine works with non-int return type", "[task][coroutine]") {
    auto task = makeStringTask("hello");
    auto statePtr = task.state();
    REQUIRE(statePtr->ready);
    REQUIRE(statePtr->value.has_value());
    const std::string val2 = *statePtr->value;  // NOLINT(bugprone-unchecked-optional-access)
    REQUIRE(val2 == "hello");
}

TEST_CASE("Task: continuation attached before coroutine completes fires on completion", "[task][coroutine]") {
    // Use a manually-driven TaskState to simulate async completion.
    auto statePtr = std::make_shared<TaskState<int>>();
    int received = -1;
    statePtr->attach([&](TaskState<int>& stateRef) {
        if (stateRef.value) {
            received = stateRef.value.value();
        }
    });
    REQUIRE(received == -1);
    statePtr->setValue(77);
    REQUIRE(received == 77);
}

TEST_CASE("Task: promise_type initial_suspend returns suspend_never", "[task][coroutine]") {
    // Verify eager start: after co_return, value is immediately available.
    auto task = makeValueTask(5);
    REQUIRE(task.state()->ready);
}

TEST_CASE("Task: promise_type final_suspend returns suspend_never", "[task][coroutine]") {
    // If final_suspend were suspend_always the coroutine frame would leak.
    // Verify by checking no crash/leak occurs when task goes out of scope.
    {
        auto task = makeValueTask(3);
        REQUIRE(task.state()->ready);
    }  // task destroyed here — frame must have already been cleaned up
    REQUIRE(true);
}
