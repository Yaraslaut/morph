#pragma once
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "executor.hpp"
#include "logger.hpp"

namespace morph {

namespace detail {

template <typename T>
struct CompletionState {
    std::mutex mtx;
    std::optional<T> value;
    std::exception_ptr error;
    bool ready = false;
    std::function<void(T)> onOk;
    std::function<void(std::exception_ptr)> onErr;
    bool onErrAttached = false;
    IExecutor* cbExec = nullptr;

    void setValue(T val) {
        std::function<void()> callback;
        {
            std::lock_guard lock{mtx};
            value = std::move(val);
            ready = true;
            if (onOk) {
                auto savedFn = std::move(onOk);
                auto savedVal = std::move(*value);
                callback = [savedFn = std::move(savedFn), savedVal = std::move(savedVal)]() mutable {
                    savedFn(std::move(savedVal));
                };
            }
        }
        if (callback != nullptr && cbExec != nullptr) {
            cbExec->post(std::move(callback));
        }
    }
    void setException(const std::exception_ptr& exc) {
        std::function<void()> callback;
        {
            std::lock_guard lock{mtx};
            error = exc;
            ready = true;
            if (onErr) {
                auto savedFn = std::move(onErr);
                auto savedErr = error;
                callback = [savedFn = std::move(savedFn), savedErr]() mutable { savedFn(savedErr); };
                onErrAttached = true;
            }
        }
        if (callback != nullptr && cbExec != nullptr) {
            cbExec->post(std::move(callback));
        }
    }
    void attachThen(std::function<void(T)> handler) {
        std::function<void()> fireNow;
        {
            std::lock_guard lock{mtx};
            if (ready && value) {
                auto savedVal = *value;
                fireNow = [handler = std::move(handler), savedVal]() mutable { handler(std::move(savedVal)); };
            } else if (!ready) {
                onOk = std::move(handler);
            }
        }
        if (fireNow != nullptr && cbExec != nullptr) {
            cbExec->post(std::move(fireNow));
        }
    }
    void attachOnError(std::function<void(std::exception_ptr)> handler) {
        std::function<void()> fireNow;
        {
            std::lock_guard lock{mtx};
            onErrAttached = true;
            if (ready && error) {
                auto savedErr = error;
                fireNow = [handler = std::move(handler), savedErr]() mutable { handler(savedErr); };
            } else if (!ready) {
                onErr = std::move(handler);
            }
        }
        if (fireNow != nullptr && cbExec != nullptr) {
            cbExec->post(std::move(fireNow));
        }
    }
    ~CompletionState() {
        if (!ready || !error || onErrAttached) {
            return;
        }
        try {
            std::rethrow_exception(error);
        } catch (const std::exception& exc) {
            try {
                morph::logError("[orphan] unhandled exception: " + std::string{exc.what()});
            } catch (...) {
            }
        } catch (...) {
            try {
                morph::logError("[orphan] unhandled unknown exception");
            } catch (...) {
            }
        }
    }
};

}  // namespace detail

template <typename T>
class Completion {
public:
    Completion() = default;
    Completion(std::shared_ptr<detail::CompletionState<T>> statePtr, IExecutor* execPtr)
        : _state{std::move(statePtr)} {
        if (_state != nullptr) {
            _state->cbExec = execPtr;
        }
    }
    Completion(Completion&&) noexcept = default;
    Completion& operator=(Completion&&) noexcept = default;
    Completion(const Completion&) = delete;
    Completion& operator=(const Completion&) = delete;

    Completion& then(std::function<void(T)> handler) {
        if (_state != nullptr) {
            _state->attachThen(std::move(handler));
        }
        return *this;
    }
    Completion& onError(std::function<void(std::exception_ptr)> handler) {
        if (_state != nullptr) {
            _state->attachOnError(std::move(handler));
        }
        return *this;
    }
    [[nodiscard]] std::shared_ptr<detail::CompletionState<T>> state() const { return _state; }

private:
    std::shared_ptr<detail::CompletionState<T>> _state;
};

}  // namespace morph
