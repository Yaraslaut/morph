#pragma once
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace morph::detail {

template <typename T>
struct TaskState {
    std::mutex mtx;
    std::optional<T> value;
    std::exception_ptr error;
    bool ready = false;
    std::function<void(TaskState&)> continuation;

    void setValue(T val) {
        std::function<void(TaskState&)> cont;
        {
            std::lock_guard lock{mtx};
            value = std::move(val);
            ready = true;
            cont = std::move(continuation);
        }
        if (cont) {
            cont(*this);
        }
    }
    void setException(const std::exception_ptr& exc) {
        std::function<void(TaskState&)> cont;
        {
            std::lock_guard lock{mtx};
            error = exc;
            ready = true;
            cont = std::move(continuation);
        }
        if (cont) {
            cont(*this);
        }
    }
    void attach(std::function<void(TaskState&)> cont) {
        {
            std::lock_guard lock{mtx};
            if (!ready) {
                continuation = std::move(cont);
                return;
            }
        }
        cont(*this);
    }
};

template <typename T>
class Task {
public:
    // NOLINTBEGIN(readability-identifier-naming) — coroutine protocol requires these names
    struct promise_type {
        std::shared_ptr<TaskState<T>> state = std::make_shared<TaskState<T>>();
        Task get_return_object() { return Task{state}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_value(T val) { state->setValue(std::move(val)); }
        void unhandled_exception() { state->setException(std::current_exception()); }
    };
    // NOLINTEND(readability-identifier-naming)

    [[nodiscard]] std::shared_ptr<TaskState<T>> state() const { return _state; }

private:
    explicit Task(std::shared_ptr<TaskState<T>> statePtr) : _state{std::move(statePtr)} {}
    std::shared_ptr<TaskState<T>> _state;
};

}  // namespace morph::detail
