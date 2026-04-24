// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "backend.hpp"
#include "completion.hpp"
#include "registry.hpp"

namespace morph {

/// @brief Internal linkage record between a model type and its active backend id.
///
/// Shared between `Bridge` and `BridgeHandler`. `Bridge::switchBackend()` updates
/// `currentId` atomically under its mutex so that concurrent `executeVia()` calls
/// always see a consistent value.
struct HandlerBinding {
    /// @brief String type-id of the model.
    std::string typeId;

    /// @brief Factory used to re-register the model on a backend switch.
    std::function<std::unique_ptr<IModelHolder>()> modelFactory;

    // Written under Bridge::_mtx; read lock-free in executeVia via the
    // atomic snapshot pattern — safe because switchBackend updates currentId
    // under the same mutex before releasing it.
    /// @brief Current `ModelId` value in the active backend (0 = unbound).
    std::atomic<uint64_t> currentId{0};
};

/// @brief Central dispatcher that routes typed actions to an `IBackend`.
///
/// `Bridge` owns exactly one active backend at a time. It tracks all registered
/// `HandlerBinding` instances and re-registers them automatically when
/// `switchBackend()` is called, enabling seamless local ↔ remote transitions.
///
/// @par Thread safety
/// All public methods are thread-safe. `executeVia()` uses a lock-free snapshot
/// of the backend pointer so it does not block `switchBackend()`.
class Bridge {
public:
    /// @brief Constructs a bridge that dispatches through @p backend.
    /// @param backend Initial backend. Ownership is transferred.
    explicit Bridge(std::unique_ptr<IBackend> backend) : _backend{std::shared_ptr<IBackend>(std::move(backend))} {}

    /// @brief Creates and registers a new `HandlerBinding` for `Model`.
    ///
    /// Uses the default `ModelFactory::create<Model>()` factory.
    /// @tparam Model Concrete model type. Must have a registered `ModelTraits` specialisation.
    /// @return Shared pointer to the new binding.
    template <typename Model>
    std::shared_ptr<HandlerBinding> registerHandler() {
        auto binding = std::make_shared<HandlerBinding>();
        binding->typeId = std::string{ModelTraits<Model>::typeId()};
        binding->modelFactory = [] { return ModelFactory::create<Model>(); };
        // Hold the mutex for the full operation: registerModel + push_back
        // must be atomic with respect to switchBackend.
        std::scoped_lock lock{_mtx};
        binding->currentId.store(_backend.load()->registerModel(binding->typeId, binding->modelFactory).v);
        _handlers.push_back(binding);
        return binding;
    }

    /// @brief Registers a pre-built binding with a custom factory.
    ///
    /// Use this overload when the model factory needs to capture external
    /// dependencies that the type-erasing default factory cannot carry.
    /// @param binding Pre-constructed binding. Its `typeId` and `modelFactory` must be set.
    void registerHandler(const std::shared_ptr<HandlerBinding>& binding) {
        std::scoped_lock lock{_mtx};
        binding->currentId.store(_backend.load()->registerModel(binding->typeId, binding->modelFactory).v);
        _handlers.push_back(binding);
    }

    /// @brief Atomically replaces the active backend with @p newBackend.
    ///
    /// All live bindings are re-registered on the new backend and their
    /// `currentId` values are updated. The old backend is released after the
    /// swap. Any in-flight `Completion` objects targeting the old backend will
    /// fail naturally.
    ///
    /// @note Lock ordering: `Bridge::_mtx` is acquired before
    ///       `LocalBackend::_regMtx`. `onBackendChanged()` implementations must
    ///       **not** call `registerHandler()` or `deregisterHandler()` — those
    ///       also acquire `_mtx` and would deadlock.
    ///
    /// @param newBackend Replacement backend. Ownership is transferred.
    void switchBackend(std::unique_ptr<IBackend> newBackend) {
        auto newShared = std::shared_ptr<IBackend>(std::move(newBackend));
        std::scoped_lock lock{_mtx};

        // Re-register all live handlers on the new backend, purge stale ptrs.
        std::vector<std::weak_ptr<HandlerBinding>> live;
        for (auto& weak : _handlers) {
            auto binding = weak.lock();
            if (!binding) {
                continue;
            }
            ModelId newId = newShared->registerModel(binding->typeId, binding->modelFactory);
            binding->currentId.store(newId.v);
            live.push_back(weak);
        }
        _handlers = std::move(live);

        // Publish the new backend atomically. After this store, executeVia
        // snapshots will see the new backend. Old backend is released here:
        // any in-flight Completions referencing old ModelIds will fail
        // naturally when the old backend destroys its model map.
        _backend.store(newShared);

        // Notify every live model on the new backend. _mtx is still held here
        // (ordering: Bridge::_mtx -> LocalBackend::_regMtx). onBackendChanged()
        // implementations must NOT call registerHandler/deregisterHandler — those
        // also acquire _mtx and would deadlock. executeVia is safe (lock-free snapshot).
        newShared->notifyBackendChanged();
    }

    /// @brief Deregisters @p binding from the active backend and removes it from tracking.
    ///
    /// After this call, any future `executeVia()` using this binding will
    /// complete with an error.
    /// @param binding Binding to remove. Must have been returned by `registerHandler()`.
    void deregisterHandler(const std::shared_ptr<HandlerBinding>& binding) {
        // Hold the mutex for the full operation: currentId read + deregister
        // + weak_ptr removal must be atomic with respect to switchBackend.
        std::scoped_lock lock{_mtx};
        uint64_t raw = binding->currentId.load();
        if (raw != 0U) {
            _backend.load()->deregisterModel(ModelId{raw});
        }
        auto iter = std::ranges::find_if(_handlers, [&](auto& weak) {
            auto sptr = weak.lock();
            return sptr && sptr.get() == binding.get();
        });
        if (iter != _handlers.end()) {
            _handlers.erase(iter);
        }
    }

    /// @brief Dispatches @p action against the model identified by @p binding.
    ///
    /// Uses a lock-free snapshot of the backend and model id so the call does
    /// not block `switchBackend()`. If a backend switch happens concurrently,
    /// the old backend still exists (its `shared_ptr` refcount is > 0) and the
    /// call either succeeds or fails with "model not found" — both are safe.
    ///
    /// @tparam Model  Model type that owns the handler.
    /// @tparam Action Action type to dispatch.
    /// @param binding Binding returned by `registerHandler<Model>()`.
    /// @param action  Action to execute (moved in).
    /// @param cbExec  Executor on which the `Completion` callbacks are posted.
    /// @return Completion that resolves with the typed result or an exception.
    template <typename Model, typename Action>
    Completion<typename ActionTraits<Action>::Result> executeVia(const std::shared_ptr<HandlerBinding>& binding,
                                                                 Action action, IExecutor* cbExec) {
        using R = ActionTraits<Action>::Result;

        // Snapshot the backend and currentId without holding the mutex for the
        // entire execute call (which would block switchBackend). The snapshot
        // is sufficient: if switchBackend runs concurrently after this point,
        // the old backend still exists (its shared_ptr refcount is > 0) and
        // the old ModelId call will either succeed (model still present) or
        // fail with "model not found" — both are safe outcomes.
        auto backend = _backend.load();
        uint64_t raw = binding->currentId.load();

        auto typedState = std::make_shared<detail::CompletionState<R>>();
        Completion<R> typed{typedState, cbExec};
        if (raw == 0U) {
            typedState->setException(std::make_exception_ptr(std::runtime_error("handler not bound")));
            return typed;
        }
        ActionCall call;
        call.modelTypeId = std::string{ModelTraits<Model>::typeId()};
        call.actionTypeId = std::string{ActionTraits<Action>::typeId()};
        // localOp and serializeAction share ownership of action via shared_ptr so
        // only one copy is made regardless of which path (local or remote) runs.
        // Previously serializeAction captured action by value independently, causing
        // a redundant copy even on the local path where serialization never executes.
        auto sharedAction = std::make_shared<Action>(std::move(action));
        call.serializeAction = [sharedAction] { return ActionTraits<Action>::toJson(*sharedAction); };
        call.deserializeResult = [](std::string_view jsonStr) -> std::shared_ptr<void> {
            return std::make_shared<R>(ActionTraits<Action>::resultFromJson(jsonStr));
        };
        call.localOp = [sharedAction](IModelHolder& holder) -> std::shared_ptr<void> {
            auto& model = holder.template into<Model>();
            return std::make_shared<R>(model.execute(*sharedAction));
        };
        auto anyCompletion = backend->execute(ModelId{raw}, std::move(call), cbExec);
        anyCompletion
            .then([typedState](const std::shared_ptr<void>& vAny) {
                typedState->setValue(std::move(*static_cast<R*>(vAny.get())));
            })
            .onError([typedState](const std::exception_ptr& err) { typedState->setException(err); });
        return typed;
    }

private:
    // shared_ptr stored in an atomic so executeVia can snapshot it without
    // holding _mtx for the duration of the backend call. All writes to
    // _backend happen under _mtx; the atomic store/load provides the
    // ordering guarantee needed by concurrent executeVia calls.
    std::atomic<std::shared_ptr<IBackend>> _backend;
    std::mutex _mtx;
    std::vector<std::weak_ptr<HandlerBinding>> _handlers;
};

/// @brief RAII wrapper that binds a single model type to a `Bridge`.
///
/// On construction, registers a `HandlerBinding` on the bridge. On destruction,
/// deregisters it automatically. The handler is non-copyable.
///
/// @tparam Model Concrete model type.
template <typename Model>
class BridgeHandler {
public:
    /// @brief Constructs and registers the handler using the default model factory.
    ///
    /// @param bridge   The bridge to register on.
    /// @param guiExec  Executor used to deliver `Completion` callbacks (e.g. the GUI thread).
    BridgeHandler(Bridge& bridge, IExecutor* guiExec)
        : _bridge{bridge}, _guiExec{guiExec}, _binding{bridge.template registerHandler<Model>()} {}

    /// @brief Constructs the handler with a pre-built binding (for dependency injection).
    ///
    /// @param bridge   The bridge to register on.
    /// @param guiExec  Executor for callback delivery.
    /// @param binding  Pre-built binding whose factory captures injected dependencies.
    BridgeHandler(Bridge& bridge, IExecutor* guiExec, std::shared_ptr<HandlerBinding> binding)
        : _bridge{bridge}, _guiExec{guiExec}, _binding{std::move(binding)} {
        _bridge.registerHandler(_binding);
    }

    /// @brief Deregisters the binding from the bridge.
    ~BridgeHandler() { _bridge.deregisterHandler(_binding); }

    BridgeHandler(const BridgeHandler&) = delete;
    BridgeHandler& operator=(const BridgeHandler&) = delete;

    /// @brief Dispatches @p action via the underlying `Bridge` and returns a `Completion`.
    ///
    /// Equivalent to calling `bridge.executeVia<Model, Action>(binding, action, guiExec)`.
    ///
    /// @tparam Action Concrete action type. Must have a registered `ActionTraits` specialisation.
    /// @param action Action to execute.
    /// @return Completion that resolves on the GUI executor.
    template <typename Action>
    Completion<typename ActionTraits<Action>::Result> execute(Action action) {
        return _bridge.template executeVia<Model, Action>(_binding, std::move(action), _guiExec);
    }

    /// @brief Returns the underlying `HandlerBinding`.
    [[nodiscard]] const std::shared_ptr<HandlerBinding>& binding() const { return _binding; }

private:
    Bridge& _bridge;
    IExecutor* _guiExec;
    std::shared_ptr<HandlerBinding> _binding;
};

}  // namespace morph
