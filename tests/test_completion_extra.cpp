#include <async_framework/completion.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include <string>

using namespace morph;

struct SyncExecutor : IExecutor {
    void post(std::function<void()> fn) override { fn(); }
};

// ── CompletionState edge branches ─────────────────────────────────────────────

TEST_CASE("CompletionState: setValue with no callback attached does not crash", "[completion]") {
    auto state = std::make_shared<detail::CompletionState<int>>();
    state->setValue(5);
    REQUIRE(state->value.has_value());
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    REQUIRE(*state->value == 5);
}

TEST_CASE("CompletionState: setException with no callback attached does not crash", "[completion]") {
    // Must attach onError to suppress orphan log
    auto state = std::make_shared<detail::CompletionState<int>>();
    state->attachOnError([](const std::exception_ptr&) {});
    state->setException(std::make_exception_ptr(std::runtime_error{"silent"}));
    REQUIRE(state->error != nullptr);
}

TEST_CASE("CompletionState: attachThen when already errored does not fire handler", "[completion]") {
    SyncExecutor exec;
    auto state = std::make_shared<detail::CompletionState<int>>();
    state->cbExec = &exec;
    state->attachOnError([](const std::exception_ptr&) {});
    state->setException(std::make_exception_ptr(std::runtime_error{"err"}));

    bool thenFired = false;
    state->attachThen([&](int) { thenFired = true; });
    REQUIRE_FALSE(thenFired);
}

TEST_CASE("CompletionState: attachOnError when already resolved with value does not fire", "[completion]") {
    SyncExecutor exec;
    auto state = std::make_shared<detail::CompletionState<int>>();
    state->cbExec = &exec;
    state->setValue(99);

    bool errFired = false;
    state->attachOnError([&](const std::exception_ptr&) { errFired = true; });
    REQUIRE_FALSE(errFired);
}

TEST_CASE("CompletionState: attachOnError when already errored fires immediately", "[completion]") {
    SyncExecutor exec;
    auto state = std::make_shared<detail::CompletionState<int>>();
    state->cbExec = &exec;
    state->setException(std::make_exception_ptr(std::runtime_error{"late"}));

    bool errFired = false;
    state->attachOnError([&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const std::runtime_error& ex) {
            errFired = (std::string{ex.what()} == "late");
        }
    });
    REQUIRE(errFired);
}

TEST_CASE("CompletionState: setValue with no executor does not post callback", "[completion]") {
    // cbExec == nullptr — callback must not be invoked
    auto state = std::make_shared<detail::CompletionState<int>>();
    state->cbExec = nullptr;

    bool fired = false;
    state->onOk = [&](int) { fired = true; };
    state->setValue(3);
    REQUIRE_FALSE(fired);
}

TEST_CASE("CompletionState: orphan destructor logs unknown (non-std) exception", "[completion]") {
    // Throw a non-std::exception type; destructor must not crash
    struct WeirdError {};
    {
        auto state = std::make_shared<detail::CompletionState<int>>();
        // Manually inject a non-std::exception exception_ptr
        try {
            throw WeirdError{};
        } catch (...) {
            state->error = std::current_exception();
            state->ready = true;
            // onErrAttached stays false — will hit the unknown exception branch in dtor
        }
        // state destroyed here → dtor logs "[orphan] unhandled unknown exception"
    }
    REQUIRE(true);  // just verify no crash/terminate
}

// ── Completion public API edge branches ───────────────────────────────────────

TEST_CASE("Completion: default-constructed null state, then/onError are no-ops", "[completion]") {
    Completion<int> comp;
    bool thenFired = false;
    bool errFired = false;
    comp.then([&](int) { thenFired = true; }).onError([&](const std::exception_ptr&) { errFired = true; });
    REQUIRE_FALSE(thenFired);
    REQUIRE_FALSE(errFired);
    REQUIRE(comp.state() == nullptr);
}

TEST_CASE("Completion: move semantics transfer state", "[completion]") {
    SyncExecutor exec;
    auto state = std::make_shared<detail::CompletionState<int>>();
    Completion<int> original{state, &exec};
    Completion<int> moved = std::move(original);

    REQUIRE(moved.state() == state);
    auto movedFromState = original.state();  // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    REQUIRE(movedFromState == nullptr);

    int received = -1;
    moved.then([&](int val) { received = val; });
    state->setValue(77);
    REQUIRE(received == 77);
}

TEST_CASE("Completion: on_error fires when error set after handler attached", "[completion]") {
    SyncExecutor exec;
    auto state = std::make_shared<detail::CompletionState<int>>();
    Completion<int> comp{state, &exec};

    std::string msg;
    comp.onError([&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const std::runtime_error& ex) {
            msg = ex.what();
        }
    });

    state->setException(std::make_exception_ptr(std::runtime_error{"delayed"}));
    REQUIRE(msg == "delayed");
}
