#pragma once
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "logger.hpp"

namespace morph {

struct IExecutor {
    virtual ~IExecutor() = default;
    virtual void post(std::function<void()> task) = 0;
};

class ThreadPoolExecutor : public IExecutor {
public:
    explicit ThreadPoolExecutor(std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            _workers.emplace_back([this] { loop(); });
        }
    }
    ~ThreadPoolExecutor() override {
        {
            std::lock_guard lock{_m};
            _stop = true;
        }
        _cv.notify_all();
        for (auto& worker : _workers) {
            worker.join();
        }
    }
    void post(std::function<void()> task) override {
        {
            std::lock_guard lock{_m};
            _q.push(std::move(task));
        }
        _cv.notify_one();
    }

private:
    void loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock lock{_m};
                _cv.wait(lock, [this] { return _stop || !_q.empty(); });
                if (_stop && _q.empty()) {
                    return;
                }
                task = std::move(_q.front());
                _q.pop();
            }
            try {
                task();
            } catch (...) {
            }
        }
    }
    std::mutex _m;
    std::condition_variable _cv;
    std::queue<std::function<void()>> _q;
    std::vector<std::thread> _workers;
    bool _stop = false;
};

class MainThreadExecutor : public IExecutor {
public:
    void post(std::function<void()> task) override {
        {
            std::lock_guard lock{_m};
            _q.push(std::move(task));
        }
        _cv.notify_all();
    }
    void runFor(std::chrono::milliseconds timeout) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            std::function<void()> task;
            {
                std::unique_lock lock{_m};
                if (!_cv.wait_until(lock, deadline, [this] { return !_q.empty(); })) {
                    return;
                }
                task = std::move(_q.front());
                _q.pop();
            }
            try {
                task();
            } catch (const std::exception& exc) {
                logError("[main-thread] callback threw: " + std::string{exc.what()});
            }
        }
    }

private:
    std::mutex _m;
    std::condition_variable _cv;
    std::queue<std::function<void()>> _q;
};

}  // namespace morph
