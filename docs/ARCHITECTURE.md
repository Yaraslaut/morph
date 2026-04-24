# async_framework Architecture

## Overview

`async_framework` is a typed, asynchronous bridge between a GUI thread and business-object models. Models may live in-process (local mode) or in a remote server process (remote mode). The GUI code is identical in both cases — only the `IBackend` implementation changes.

The framework is header-only (C++23, namespace `morph`), depends on Glaze for JSON reflection, and optionally integrates with Qt 6 via a separate target.

## Layer diagram

```
┌─────────────────────────────────────────────────────────────────┐
│  Application / GUI                                              │
│  BridgeHandler<Model>  ←  typed user-facing API                 │
├─────────────────────────────────────────────────────────────────┤
│  Public API  (bridge.hpp, completion.hpp)                       │
│  Bridge · HandlerBinding · BridgeHandler<M> · Completion<T>     │
├─────────────────────────────────────────────────────────────────┤
│  Backend abstraction  (backend.hpp, remote.hpp)                 │
│  IBackend · LocalBackend · SimulatedRemoteBackend               │
│  RemoteServer · QtWebSocketBackend · QtWebSocketServer          │
├─────────────────────────────────────────────────────────────────┤
│  Registry & type erasure  (registry.hpp, model.hpp)             │
│  ActionDispatcher · ModelRegistryFactory · ModelTraits · …      │
├─────────────────────────────────────────────────────────────────┤
│  Internal async core                                            │
│  IExecutor · ThreadPoolExecutor · MainThreadExecutor            │
│                                       (executor.hpp)           │
│  StrandExecutor · ModelId             (strand.hpp)              │
│  detail::Task<T>                      (task.hpp)                │
│  detail::CompletionState<T>           (completion.hpp)          │
├─────────────────────────────────────────────────────────────────┤
│  Cross-cutting                                                  │
│  LogLevel · setLogger · log()         (logger.hpp)              │
└─────────────────────────────────────────────────────────────────┘
```

## Header map

### Core headers (`include/async_framework/`)

| Header | Responsibility |
|---|---|
| `executor.hpp` | `IExecutor`, `ThreadPoolExecutor`, `MainThreadExecutor` |
| `strand.hpp` | `ModelId`, `ModelIdHash`, `StrandExecutor` — serialises tasks per model |
| `task.hpp` | `detail::Task<T>` — coroutine primitive (internal only) |
| `completion.hpp` | `detail::CompletionState<T>`, `Completion<T>` — public result handle |
| `model.hpp` | `IModelHolder`, `ModelHolder<T>`, `ModelFactory` — type-erased model storage |
| `registry.hpp` | `ActionDispatcher`, `ModelRegistryFactory`, `ModelTraits<M>`, `ActionTraits<A>`, registration macros |
| `backend.hpp` | `ActionCall`, `IBackend`, `LocalBackend` |
| `remote.hpp` | `RemoteServer`, `SimulatedRemoteBackend` |
| `bridge.hpp` | `HandlerBinding`, `Bridge`, `BridgeHandler<M>` |
| `logger.hpp` | `LogLevel`, `Logger` (alias), `setLogger`, `setLogLevel`, level helpers |
| `network_monitor.hpp` | `NetworkMonitor` — abstract probe thread, online/offline state machine, configurable thresholds |
| `offline_queue.hpp` | `IOfflineQueue`, `QueueItem`, `InMemoryOfflineQueue` — durable write queue abstraction |
| `sync_worker.hpp` | `SyncWorker`, `SyncResult` — drains offline queue on reconnect via caller-supplied replay |

### Qt integration headers (`include/async_framework/qt/`)

| Header | Responsibility |
|---|---|
| `qt_executor.hpp` | `QtExecutor` — posts callables via `QMetaObject::invokeMethod` |
| `qt_websocket_backend.hpp` | `QtWebSocketBackend` — WebSocket client; implements `IBackend` |
| `qt_websocket_server.hpp` | `QtWebSocketServer` — QObject server; forwards messages to `RemoteServer` |

## Deployment topologies

**Local mode** — model lives in the same process:

```
GUI thread
  └─ BridgeHandler<M>::execute(action)
       └─ Bridge::executeVia<M, A>
            └─ LocalBackend::execute
                 └─ StrandExecutor → worker thread → Model::execute(action)
                      └─ Completion<T>::then callback → GUI executor
```

**Simulated-remote mode** — model lives behind an in-process `RemoteServer` (used in tests):

```
GUI thread
  └─ BridgeHandler<M>::execute(action)
       └─ Bridge::executeVia<M, A>
            └─ SimulatedRemoteBackend::execute
                 └─ serialize action → RemoteServer::handle (5-part text protocol)
                      └─ ActionDispatcher → StrandExecutor → Model::execute
                           └─ serialize result → Completion<T>::then → GUI executor
```

**Qt WebSocket mode** — model lives in a separate process:

```
GUI thread (Qt process)                         Server process
  └─ BridgeHandler<M>::execute(action)
       └─ Bridge::executeVia<M, A>
            └─ QtWebSocketBackend::execute
                 └─ assign callId, send JSON  ──► QtWebSocketServer::handle
                                                        └─ RemoteServer::handle (6-part protocol)
                                                             └─ ActionDispatcher → StrandExecutor → Model::execute
                 ◄── JSON reply (ok|callId|result) ──────────────────────────────
            └─ resolve pending Completion
       └─ Completion<T>::then callback → QtExecutor → GUI thread
```

The GUI sees the same `Completion<T>` in all three modes.

## Wire protocol

`RemoteServer::handle` accepts pipe-delimited text messages:

| Format | Direction | Meaning |
|---|---|---|
| `register\|<modelTypeId>` | client → server | Register a model instance; server replies `ok\|<id>` |
| `deregister\|<id>` | client → server | Destroy model instance; server replies `ok` |
| `execute\|<id>\|<modelTy>\|<actionTy>\|<json>` | client → server | 5-part; SimulatedRemote |
| `execute\|<callId>\|<id>\|<modelTy>\|<actionTy>\|<json>` | client → server | 6-part; Qt WebSocket |
| `ok\|<result>` | server → client | Successful execute (SimulatedRemote) |
| `ok\|<callId>\|<result>` | server → client | Successful execute (Qt WebSocket) |
| `err\|<message>` | server → client | Error reply (SimulatedRemote) |
| `err\|<callId>\|<message>` | server → client | Error reply (Qt WebSocket) |

## Component detail

### Executors

All concurrency runs through `IExecutor::post(fn)`:

- **`ThreadPoolExecutor`** — fixed N worker threads, MPMC queue; exceptions are swallowed and logged.
- **`MainThreadExecutor`** — single-threaded queue with `runFor(timeout)` drain; used in non-Qt tests to pump the "GUI" thread.
- **`QtExecutor`** — posts via `QMetaObject::invokeMethod(Qt::QueuedConnection)`; safe from any thread; drops silently if the target object is deleted.

### StrandExecutor

Guarantees that all tasks for the same `ModelId` are serialised while tasks for different models are parallelised. Internally keeps one `std::queue` and a `running` flag per model; tasks are dispatched to the underlying `IExecutor` one at a time.

### Completion<T>

Move-only public result handle. Internally backed by `detail::CompletionState<T>` (mutex-protected value + error + two callback slots). Key invariant: callbacks are always delivered via the `IExecutor` supplied at construction, so the GUI thread is never blocked and callbacks always fire on the right thread.

If a `Completion` is destroyed with an unhandled error (no `.onError` was attached), `CompletionState::~CompletionState` logs the exception through the configured logger.

### Registry & type erasure

`ModelTraits<M>` and `ActionTraits<A>` are specialised by registration macros. They provide:
- String type IDs for protocol routing.
- Glaze-based `toJson` / `fromJson` for actions and results.

`ActionDispatcher` maps `(modelTypeId, actionTypeId)` → a runner lambda that downcasts `IModelHolder`, calls `model.execute(action)`, and returns the result as JSON.

`ModelRegistryFactory` maps `modelTypeId` → a factory closure that constructs an `IModelHolder`.

### HandlerBinding — why it exists

When `BridgeHandler<M>` registers with the `Bridge`, the backend assigns it a `ModelId` (just a `uint64_t`). That ID is backend-local — if the backend is ever replaced, all existing IDs are meaningless.

The problem is that `Bridge` needs to be able to find every live `BridgeHandler` later (e.g. to re-register it on a new backend), but it cannot hold a strong reference to them — that would prevent `BridgeHandler` from being destroyed naturally at the end of its scope.

`HandlerBinding` is the indirection that solves both constraints:

```
BridgeHandler<M>  ──owns──►  shared_ptr<HandlerBinding>
Bridge            ──holds──►  weak_ptr<HandlerBinding>   (does NOT keep it alive)
```

`BridgeHandler` holds the `shared_ptr`, so the `HandlerBinding` lives exactly as long as the handler does. `Bridge` holds a `weak_ptr`, so it can observe whether the handler is still alive — but it cannot prevent its destruction.

**RAII lifetime:** When `BridgeHandler` goes out of scope, its destructor calls `Bridge::deregisterHandler`, which tells the backend to destroy the model instance and removes the stale `weak_ptr` from the Bridge's list. No manual cleanup is required from application code.

**`atomic<uint64_t> currentId`:** Every call to `executeVia` reads `binding->currentId` to find out which backend-assigned ID to use. The value is an atomic so it can be updated during a backend switch without holding the bridge mutex for the duration of every execute call. A value of `0` is the sentinel for "not bound" — `executeVia` returns an immediate error in that case.

**Backend switching — implemented:** `Bridge::switchBackend(newBackend)` acquires the bridge
mutex, re-registers every live `HandlerBinding` on the new backend (writing the new `ModelId`
atomically into `binding->currentId`), replaces `_backend`, then calls
`newBackend->notifyBackendChanged()`. `LocalBackend` forwards this to every live model that
implements `IBackendChangedSink` (detected at compile time via the `BackendChangedNotifiable`
concept). `SimulatedRemoteBackend` is a no-op — its models live inside `RemoteServer`.

### Logger

`logger.hpp` provides a global, mutex-protected, replaceable sink (`std::function<void(LogLevel, std::string_view)>`). `LogLevel` is a `uint8_t`-backed enum (`debug < info < warn < error < off`). All framework internals route through `morph::log(level, msg)`; call `setLogger` at startup to redirect to spdlog, Qt logging, or a test spy.

### NetworkMonitor

`network_monitor.hpp` provides a background probe thread that tracks connectivity and fires
callbacks on transitions. The framework supplies no probe implementation — the caller provides
a `bool()` callable that returns `true` when the system is reachable (TCP connect, HTTP ping,
custom check, etc.). This keeps the monitor fully portable across OS and transport.

**State machine:**
- Starts online (assumes connectivity until proven otherwise).
- Goes offline after `failureThreshold` consecutive probe failures → fires `onOffline`.
- Returns online after `onlineThreshold` consecutive probe successes → fires `onOnline`.
- Each transition fires its callback exactly once.
- Callbacks run on the probe thread and must not block.

**Thread safety:** `isOnline()` reads an `std::atomic<bool>` — safe from any thread.
`stop()` is idempotent and safe to call multiple times or from any thread.
The destructor calls `stop()`, so explicit shutdown is optional.

**Intended integration with switchBackend:**
```cpp
NetworkMonitor monitor{
    myTcpProbe,
    [&] { bridge.switchBackend(std::make_unique<LocalBackend>(localPool)); },   // go offline
    [&] { bridge.switchBackend(std::make_unique<RemoteBackend>(remotePool)); }  // recover
};
```

### IOfflineQueue + InMemoryOfflineQueue

`offline_queue.hpp` provides an interface over three operations:

| Method | Semantics |
|---|---|
| `enqueue(payload)` | Append an opaque string item; returns a stable `uint64_t` id |
| `drain()` | Return all pending items in enqueue order; does **not** remove them |
| `markDone(id)` | Remove item by id; no-op if unknown |

`drain()` returning items without removing them is deliberate — items survive a crash between
`drain()` and `markDone()`. A SQL-backed implementation (not in this repository) can persist items
across process restarts by storing them in a table with a UNIQUE constraint on the payload.

`InMemoryOfflineQueue` implements the interface with a `std::deque` protected by a mutex.
It does not deduplicate.

### SyncWorker

`sync_worker.hpp` drains an `IOfflineQueue` on reconnect. The caller supplies a
`ReplayFunction` (`bool(const std::string& payload)`) that knows how to process each item —
the framework has no knowledge of what replay means (insert to DB, POST to API, etc.).

- Returns `true` → `markDone` called, item removed.
- Returns `false` or throws → item left in queue for the next `run()`.
- `stop()` signals the current `run()` to abort after the current item. Resets at the
  start of each `run()`, so it is one-shot.
- Concurrent `run()` calls are serialised by an internal mutex.

**Typical wiring with NetworkMonitor:**

```cpp
SyncWorker syncWorker{queue, myDomainReplayFn};

NetworkMonitor monitor{
    myProbe,
    [&] { bridge.switchBackend(std::make_unique<LocalBackend>(localPool)); },
    [&] {
        syncWorker.run();  // drain queue before switching to remote
        bridge.switchBackend(std::make_unique<RemoteBackend>(remotePool));
    }
};
```

### Conflict Resolution — a domain concern, not a framework concern

Conflict resolution during offline-to-online sync belongs entirely in the model.
The framework's role is to fire `onBackendChanged()` on the new model instance and
then step back. How the model handles that notification is its own business.

**Pattern — the model owns the logic:**

```cpp
class OrderModel {
public:
    using ConflictChecker  = std::function<bool(const std::string&)>;
    using ConflictResolver = std::function<std::string(const std::string&)>;

    OrderModel(IOfflineQueue& queue, ConflictChecker check, ConflictResolver resolve);

    void onBackendChanged() {
        for (auto& item : _queue.drain()) {
            if (_check(item.payload)) {
                std::string merged = _resolve(item.payload);
                if (!merged.empty()) applyMerged(merged);
                // discard or merge — either way, remove from queue
            }
            _queue.markDone(item.id);
        }
    }
};
```

**Why the model, not the framework:**
- The framework cannot know what "conflict" means for your domain.
- The framework cannot know how to merge conflicting states.
- The model already holds the context it needs: domain services, database handles,
  resolvers — a queue reference is no different.
- `onBackendChanged()` is a lifecycle hook. The model is free to do nothing, replay
  everything, or run arbitrarily complex reconciliation.

**What the framework guarantees:**
- `onBackendChanged()` fires exactly once per `switchBackend()` call.
- It fires on the **new** backend's model instance (not the old one).
- It fires after all handlers are re-registered — `execute()` calls issued from
  within `onBackendChanged()` will reach the new backend.
- Each `switchBackend()` creates a fresh model instance via the registered factory.
  If the factory captures dependencies by reference, the new instance shares the
  same queue, resolver, and domain services as the old one.

## Thread safety

| Component | Guarantee |
|---|---|
| `Model::execute` | Never called concurrently for the same `ModelId` (strand). |
| `Completion<T>` / `CompletionState<T>` | Fully mutex-protected; callbacks always marshal to the supplied executor. |
| `Bridge` | Handler list protected by mutex; register/deregister safe from any thread. |
| `Logger` | Sink and level accesses protected by mutex. |
| `StrandExecutor` | Per-strand mutex + atomic running flag; safe from any thread. |
| `Bridge::switchBackend` | Holds bridge mutex for full duration; re-registration and notification are atomic with respect to new `execute` calls. |

## Error propagation

```
Model::execute(action) throws
  └─ Task<T> coroutine catches via co_return / exception
       └─ CompletionState::setException(current_exception())
            └─ .onError(fn) handler posted to GUI executor
                 └─ fn receives exception_ptr; caller rethrows to inspect
```

If the `Completion` is abandoned (no `.onError` attached), the destructor logs the exception. Non-`std::exception` types are logged as "unknown exception".

## Why does each component exist?

A quick reference for the "what problem does this solve?" question for each major type.

| Component | Problem it solves |
|---|---|
| `IExecutor` | Decouples "run this function" from where it runs — worker pool, GUI event loop, Qt queue, or test inline. |
| `ThreadPoolExecutor` | Provides a real parallel worker pool so models execute off the GUI thread. |
| `MainThreadExecutor` | Lets non-Qt tests pump a fake GUI thread with `runFor()`, avoiding the need for a Qt event loop in tests. |
| `QtExecutor` | Bridges `IExecutor::post` to `QMetaObject::invokeMethod` so callbacks land on the Qt GUI thread safely. |
| `StrandExecutor` | Guarantees that a model's `execute` is never called concurrently, so model authors write single-threaded code. |
| `detail::Task<T>` | Wraps a coroutine so exceptions from model code are captured as `exception_ptr` instead of terminating. |
| `detail::CompletionState<T>` | Shared mutable state between the worker (which writes the result) and the GUI (which reads it via callback). |
| `Completion<T>` | User-facing move-only handle for chaining `.then` / `.onError` without blocking the GUI thread. |
| `IModelHolder` / `ModelHolder<T>` | Type-erases the model so the backend can store heterogeneous model types in one registry. |
| `ActionDispatcher` | Routes `(modelTypeId, actionTypeId)` to the right `model.execute(action)` overload at runtime without the caller knowing the concrete type. |
| `ModelRegistryFactory` | Reconstructs any registered model by string ID — needed on the server side where there is no compile-time type. |
| `IBackend` | The seam that makes local and remote execution interchangeable from the GUI's perspective. |
| `LocalBackend` | In-process execution: stores models in a map, runs them through `StrandExecutor`. |
| `RemoteServer` | Server-side dispatcher: parses wire messages, routes to `ActionDispatcher`, streams results back. |
| `SimulatedRemoteBackend` | Lets tests exercise the full remote code path in-process, without a network. |
| `HandlerBinding` | Stable identity for a handler that outlives backend replacement — see detailed section above. |
| `IBackendChangedSink` | Interface implemented by `ModelHolder<M>` when `M` declares `onBackendChanged()` — discovered by `Bridge` via `dynamic_cast`. |
| `BackendChangedNotifiable<M>` | C++20 concept that detects whether `M` declares `void onBackendChanged()` — zero runtime cost, evaluated at compile time. |
| `Bridge` | Owns the backend and the list of live handlers; routes `executeVia` calls to the right backend slot. |
| `BridgeHandler<M>` | Typed, RAII wrapper that auto-registers on construction and auto-deregisters on destruction. |
| `Logger` / `setLogger` | Lets the framework emit diagnostics without hardcoding a logging library, and lets tests capture them. |

## Real-world example: why HandlerBinding matters

Consider a desktop application with a GUI that owns a `BridgeHandler<CounterModel>`. In production it runs against a `LocalBackend`. During a debugging session the developer wants to switch to a `SimulatedRemoteBackend` to inspect the wire protocol — without restarting the application.

**Without `HandlerBinding`** the GUI would need to:
1. Destroy every `BridgeHandler`.
2. Replace the backend.
3. Reconstruct every `BridgeHandler` (which means re-registering all models).

That means the GUI owns backend lifecycle knowledge — it has to know *which handlers exist*, in *what order* to recreate them, and *when* the switch is complete. This couples the GUI tightly to the backend.

**With `HandlerBinding`** (current state) the footwork is already in place:

- `Bridge::_handlers` is a `vector<weak_ptr<HandlerBinding>>` — a ready-made list of every live handler, with expired entries automatically skippable.
- Each `HandlerBinding` stores the `modelFactory` closure needed to re-register on a new backend.
- `currentId` is atomic — it can be updated mid-flight without locking every execute call.

`Bridge::switchBackend(newBackend)` closes the loop — it re-registers every live
`HandlerBinding` on the new backend atomically, then calls `notifyBackendChanged()` so
models can react (flush caches, reconnect, replay queued writes, etc.).

```cpp
// GUI startup — local mode
Bridge bridge{std::make_unique<LocalBackend>(pool)};
BridgeHandler<CounterModel> counter{bridge, &guiExec};
BridgeHandler<SettingsModel> settings{bridge, &guiExec};

// ... user triggers "connect to remote" ...

// Switch to a simulated-remote backend at runtime — GUI code unchanged:
bridge.switchBackend(std::make_unique<SimulatedRemoteBackend>(server));
// Both counter and settings handlers are transparently re-registered.
// CounterModel::onBackendChanged() fires on the new backend's model instance.
// Subsequent execute() calls are routed to the new backend automatically.
```

## Adding a new model and actions

1. Define the model struct with `execute` overloads:

```cpp
struct MyAction { int x = 0; };

struct MyModel {
    int execute(const MyAction& a) { return a.x * 2; }
};
```

2. Specialise traits and register (in a `.cpp` that owns the model):

```cpp
#include <async_framework/registry.hpp>

BRIDGE_REGISTER_MODEL (MyModel,  "MyModel")
BRIDGE_REGISTER_ACTION(MyModel, MyAction, "MyAction")
```

3. Use from the GUI (same code for local and remote):

```cpp
Bridge bridge{std::make_unique<LocalBackend>(pool)};
BridgeHandler<MyModel> handler{bridge, &guiExecutor};

handler.execute(MyAction{21})
    .then([](int result) { /* runs on GUI thread */ })
    .onError([](std::exception_ptr e) { /* runs on GUI thread */ });
```

## Known limitations

### `RemoteServer` must be heap-allocated

`RemoteServer::handle()` captures `shared_from_this()` to prevent a use-after-free if the
worker pool outlives the server. This means `RemoteServer` **must** be created via
`std::make_shared<RemoteServer>(...)`. Constructing it on the stack and calling `handle()` will
throw `std::bad_weak_ptr` at runtime.

```cpp
// Correct
auto server = std::make_shared<RemoteServer>(pool);

// Wrong — throws std::bad_weak_ptr when handle() is called
RemoteServer server{pool};
```

### `SimulatedRemoteBackend::registerModel` must not be called from a pool thread

`registerModel` and `deregisterModel` post a message to `RemoteServer` and then block on a
`std::future` waiting for the reply. That reply is produced by a task on the same pool. If
`registerModel` is itself called from a task on that pool, the blocking `future::get()` will
deadlock because the thread is occupied waiting for work it would need to dispatch itself.

**Safe:** call `registerModel` / `deregisterModel` from the GUI thread or any thread that is
not a worker in the pool that backs the `RemoteServer`.

**Unsafe:** calling `Bridge::switchBackend` (which internally calls `registerModel` for every
live handler) from within a pool-backed task.

### `NetworkMonitor` callbacks must not block

Callbacks (`onOffline`, `onOnline`) run directly on the probe thread. A blocking call inside a
callback (e.g. synchronous I/O, `std::future::get`, a long computation) will delay or prevent
subsequent probes, and a blocking `stop()` call from within a callback will self-deadlock on
the thread join. `stop()` detects this case and detaches instead of joining; the monitor thread
completes its current iteration and exits, and the destructor spin-waits until it does. The
intent is that callbacks should be short — typically just setting an atomic flag or posting to
an executor.

### `Bridge::switchBackend` must not be called from `onBackendChanged`

`switchBackend` holds `Bridge::_mtx` for its entire duration and calls
`notifyBackendChanged()` while still holding it. `onBackendChanged()` is invoked from inside
that call. If an `onBackendChanged()` implementation calls `switchBackend` or
`registerHandler` / `deregisterHandler` (which also acquire `_mtx`), the thread will
self-deadlock. `executeVia` is safe to call from `onBackendChanged()` because it uses a
lock-free snapshot of the backend.

### `CompletionState` requires a non-null executor before callbacks fire

The `cbExec` pointer on `CompletionState` must be set before `setValue` / `setException` is
called (or before `attachThen` / `attachOnError` if the state is already ready). If `cbExec`
is null when a callback would fire, the callback is silently discarded — no error is raised.
This is enforced by the `Completion<T>` constructor, which accepts an `IExecutor*`; the hazard
only arises when using `CompletionState` directly (an internal type).

### `MainThreadExecutor::runFor` does not drain on timeout

If `runFor(timeout)` returns because the timeout expired rather than because the queue
emptied, any remaining tasks stay enqueued. A subsequent `runFor` call will process them. This
is intentional — `runFor` is a pump, not a flush — but callers that expect all posted work to
complete should either call `runFor` again or ensure tasks are posted with enough headroom.

## Key design decisions

| Decision | Rationale |
|---|---|
| Header-only library | Zero build-system friction; include and use. |
| `StrandExecutor` per `ModelId` | Parallelism across models; serial within one model — model authors write single-threaded code. |
| `Completion<T>` not `std::future<T>` | Callbacks marshal to a specific executor; futures do not. |
| `IBackend` seam | Single GUI code path for local, simulated-remote, and Qt WebSocket; switching is one constructor argument. |
| `HandlerBinding` with atomic `currentId` | Handlers survive backend replacement without re-registering from application code. |
| `LogLevel : uint8_t` | Minimises storage; 5 levels fit in one byte. |
| Glaze for JSON | Reflects aggregate types automatically; no hand-written serialisation per action. |
| `detail::Task<T>` internal only | Keeps the public API free of coroutine machinery; implementation can change without breaking callers. |
| 6-part Qt wire protocol | Carries a `callId` so async WebSocket replies can be correlated back to pending `Completion` objects. |
