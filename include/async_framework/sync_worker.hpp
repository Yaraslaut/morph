#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>

#include "offline_queue.hpp"

namespace morph {

struct SyncResult {
    int successful = 0;
    int failed = 0;
};

// SyncWorker drains an IOfflineQueue on reconnect, replaying each item via
// a caller-supplied function. The framework has no knowledge of what "replay"
// means — that is entirely the caller's domain.
//
// ReplayFunction: bool(const std::string& payload)
//   Return true  → item successfully processed, removed from queue.
//   Return false → item failed, left in queue for the next run() call.
//   Throw        → treated as failure; exception is swallowed, item left in queue.
//
// run() is safe to call from any thread. Concurrent calls are serialised by
// an internal mutex — the second caller blocks until the first run() completes.
class SyncWorker {
public:
    using ReplayFunction = std::function<bool(const std::string& payload)>;

    SyncWorker(IOfflineQueue& queue, ReplayFunction replay) : _queue{queue}, _replay{std::move(replay)} {}

    SyncResult run() {
        std::lock_guard runLock{_runMtx};  // serialise concurrent run() calls
        // exchange returns the old value: if stop() was called before run() acquired
        // the lock, we treat that as an immediate abort and reset the flag.
        // store(false) would silently discard a pre-run stop() signal.
        bool wasStoppedBeforeRun = _stopped.exchange(false);
        SyncResult result;
        if (wasStoppedBeforeRun) {
            return result;
        }

        for (auto& item : _queue.drain()) {
            if (_stopped.load()) {
                break;
            }
            bool succeeded = false;
            try {
                succeeded = _replay(item.payload);
            } catch (...) {
                succeeded = false;
            }
            if (succeeded) {
                _queue.markDone(item.id);
                ++result.successful;
            } else {
                ++result.failed;
            }
        }
        return result;
    }

    // Signal an in-progress run() to stop after the current item.
    // Safe to call from any thread. Resets automatically at the start of
    // the next run() call — stopping is one-shot.
    void stop() { _stopped.store(true); }

private:
    IOfflineQueue& _queue;
    ReplayFunction _replay;
    std::mutex _runMtx;
    std::atomic<bool> _stopped{false};
};

}  // namespace morph
