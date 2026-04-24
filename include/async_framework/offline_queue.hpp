#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace morph {

// An item stored in the queue. The payload is an opaque string — the caller
// controls serialisation format (JSON, binary-hex, plain text, etc.).
struct QueueItem {
    uint64_t id;
    std::string payload;
};

// ── Interface ─────────────────────────────────────────────────────────────────

struct IOfflineQueue {
    virtual ~IOfflineQueue() = default;

    // Append an item. Returns a stable id that can be passed to markDone.
    // The in-memory impl assigns a monotonically increasing id.
    virtual uint64_t enqueue(std::string payload) = 0;

    // Return all pending items in enqueue order. Does NOT remove them —
    // items remain until markDone is called. Safe to call multiple times.
    // This is intentional: items survive a crash between drain() and markDone().
    virtual std::vector<QueueItem> drain() = 0;

    // Remove the item with the given id. No-op if id is unknown.
    virtual void markDone(uint64_t itemId) = 0;
};

// ── In-memory implementation ──────────────────────────────────────────────────

class InMemoryOfflineQueue : public IOfflineQueue {
public:
    uint64_t enqueue(std::string payload) override {
        std::lock_guard lock{_mtx};
        uint64_t itemId = ++_nextId;
        _items.push_back(QueueItem{.id = itemId, .payload = std::move(payload)});
        return itemId;
    }

    std::vector<QueueItem> drain() override {
        std::lock_guard lock{_mtx};
        return std::vector<QueueItem>{_items.begin(), _items.end()};
    }

    void markDone(uint64_t itemId) override {
        std::lock_guard lock{_mtx};
        auto iter = std::ranges::find_if(_items, [itemId](const QueueItem& item) { return item.id == itemId; });
        if (iter != _items.end()) {
            _items.erase(iter);
        }
    }

private:
    std::mutex _mtx;
    std::deque<QueueItem> _items;
    uint64_t _nextId{0};
};

}  // namespace morph
