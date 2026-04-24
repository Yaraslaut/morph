#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace morph {

// Config is declared outside NetworkMonitor so that its default member
// initializers are fully parsed before any NetworkMonitor constructor
// default argument that names Config{} is evaluated. Compilers (clang,
// GCC) reject "Config cfg = Config{}" in a constructor default argument
// when Config is a nested type whose own defaults are not yet complete.
struct NetworkMonitorConfig {
    std::chrono::milliseconds probeInterval = std::chrono::seconds{5};
    int failureThreshold = 3;  // consecutive failures before going offline
    int onlineThreshold = 1;   // consecutive successes before going online
};

class NetworkMonitor {
public:
    using ProbeFunction = std::function<bool()>;
    using Callback = std::function<void()>;
    using Config = NetworkMonitorConfig;

    // Starts online (assumes connectivity until proven otherwise). The first
    // failureThreshold probes must all fail before the monitor considers itself
    // offline and fires onOffline. Callers that know the initial state is
    // offline should use a probe that starts returning false immediately.
    NetworkMonitor(ProbeFunction probe, Callback onOffline, Callback onOnline, Config cfg = Config{})
        : _probe{std::move(probe)},
          _onOffline{std::move(onOffline)},
          _onOnline{std::move(onOnline)},
          _cfg{cfg},
          _online{true},
          _stopped{false},
          _thread{[this] { run(); }} {}

    ~NetworkMonitor() {
        stop();
        // If stop() was called from within a probe callback (same thread as run()),
        // it detached the thread instead of joining. The thread will exit shortly
        // after setting _stopped. Wait here so run() is not reading members after
        // the object is destroyed.
        while (!_runExited.load()) {
            std::this_thread::yield();
        }
    }

    NetworkMonitor(const NetworkMonitor&) = delete;
    NetworkMonitor& operator=(const NetworkMonitor&) = delete;
    NetworkMonitor(NetworkMonitor&&) = delete;
    NetworkMonitor& operator=(NetworkMonitor&&) = delete;

    [[nodiscard]] bool isOnline() const noexcept { return _online.load(); }

    void stop() {
        {
            // _mtx provides the idempotency guard: only one caller sets _stopped
            // and signals the CV. _stopped is also atomic so run() can read it
            // lock-free inside the wait_for predicate and after waking.
            std::lock_guard lock{_mtx};
            if (_stopped) {
                return;
            }
            _stopped = true;
        }
        _cv.notify_all();
        if (_thread.joinable()) {
            if (_thread.get_id() == std::this_thread::get_id()) {
                // Called from within the monitor thread (e.g. from a probe callback).
                // Joining would deadlock; detach so the thread cleans itself up.
                // The destructor spin-waits on _runExited to ensure run() finishes
                // before the object is destroyed.
                _thread.detach();
            } else {
                _thread.join();
            }
        }
    }

private:
    static bool safeProbe(const ProbeFunction& probe) noexcept {
        try {
            return probe ? probe() : false;
        } catch (...) {
            return false;
        }
    }

    void handleProbeResult(bool probeOk, int& consecutiveFailures, int& consecutiveSuccesses) {
        if (!probeOk) {
            consecutiveSuccesses = 0;
            ++consecutiveFailures;
            if (_online && consecutiveFailures >= _cfg.failureThreshold) {
                // Store before callback: callers may observe isOnline() == false
                // from another thread before the callback returns. This ordering
                // is intentional — the state is canonical, the callback is a
                // notification.
                _online.store(false);
                if (_onOffline) {
                    _onOffline();
                }
            }
        } else {
            consecutiveFailures = 0;
            ++consecutiveSuccesses;
            if (!_online && consecutiveSuccesses >= _cfg.onlineThreshold) {
                _online.store(true);
                if (_onOnline) {
                    _onOnline();
                }
            }
        }
    }

    void run() {
        int consecutiveFailures = 0;
        int consecutiveSuccesses = 0;

        while (true) {
            {
                std::unique_lock lock{_mtx};
                _cv.wait_for(lock, _cfg.probeInterval, [this] { return _stopped.load(); });
                if (_stopped) {
                    break;
                }
            }
            handleProbeResult(safeProbe(_probe), consecutiveFailures, consecutiveSuccesses);
        }
        _runExited.store(true);
    }

    ProbeFunction _probe;
    Callback _onOffline;
    Callback _onOnline;
    Config _cfg;
    std::atomic<bool> _online;
    std::atomic<bool> _stopped;
    std::atomic<bool> _runExited{false};
    std::mutex _mtx;
    std::condition_variable _cv;
    std::thread _thread;
};

}  // namespace morph
