#pragma once
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_map>

#include "executor.hpp"

namespace morph {

struct ModelId {
    uint64_t v{0};
    bool operator==(const ModelId& other) const { return v == other.v; }
};
struct ModelIdHash {
    std::size_t operator()(ModelId mid) const noexcept { return std::hash<uint64_t>{}(mid.v); }
};

class StrandExecutor {
public:
    explicit StrandExecutor(IExecutor& base) : _base{&base} {}

    // Waits for all in-flight lambdas to complete before destroying the map.
    // Without this, a pool thread running scheduleNext can access _strands
    // after it has been destroyed (TSan: data race on destructor vs erase).
    ~StrandExecutor() {
        std::unique_lock lock{_mapMtx};
        _cv.wait(lock, [this] { return _inFlight == 0; });
    }

    void post(ModelId key, std::function<void()> task) {
        std::shared_ptr<Strand> strand;
        {
            std::lock_guard lock{_mapMtx};
            auto& slot = _strands[key];
            if (!slot) {
                slot = std::make_shared<Strand>();
                slot->base = _base;
            }
            strand = slot;
        }
        bool schedule = false;
        {
            std::lock_guard lock{strand->mtx};
            strand->pending.push(std::move(task));
            if (!strand->running) {
                strand->running = true;
                schedule = true;
            }
        }
        if (schedule) {
            scheduleNext(strand, key);
        }
    }

private:
    struct Strand {
        IExecutor* base = nullptr;
        std::mutex mtx;
        std::queue<std::function<void()>> pending;
        bool running = false;
    };

    void scheduleNext(const std::shared_ptr<Strand>& strand, ModelId key) {
        {
            std::lock_guard lock{_mapMtx};
            ++_inFlight;
        }
        strand->base->post([this, strand, key] {
            std::function<void()> task;
            {
                std::lock_guard lock{strand->mtx};
                task = std::move(strand->pending.front());
                strand->pending.pop();
            }
            try {
                task();
            } catch (...) {
            }
            bool more = false;
            {
                std::lock_guard lock{strand->mtx};
                more = !strand->pending.empty();
                if (!more) {
                    strand->running = false;
                }
            }
            if (more) {
                scheduleNext(strand, key);
            } else {
                // Queue drained — remove the entry to prevent unbounded map growth.
                std::lock_guard lock{_mapMtx};
                auto iter = _strands.find(key);
                if (iter != _strands.end() && iter->second == strand) {
                    _strands.erase(iter);
                }
            }
            // Decrement after all map access is done; wake destructor if it is waiting.
            {
                std::lock_guard lock{_mapMtx};
                if (--_inFlight == 0) {
                    _cv.notify_all();
                }
            }
        });
    }

    IExecutor* _base;
    std::mutex _mapMtx;
    std::condition_variable _cv;
    int _inFlight{0};
    std::unordered_map<ModelId, std::shared_ptr<Strand>, ModelIdHash> _strands;
};

}  // namespace morph
