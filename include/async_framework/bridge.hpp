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

struct HandlerBinding {
    std::string typeId;
    std::function<std::unique_ptr<IModelHolder>()> modelFactory;
    // Written under Bridge::_mtx; read lock-free in executeVia via the
    // atomic snapshot pattern — safe because switchBackend updates currentId
    // under the same mutex before releasing it.
    std::atomic<uint64_t> currentId{0};
};

class Bridge {
public:
    explicit Bridge(std::unique_ptr<IBackend> backend) : _backend{std::shared_ptr<IBackend>(std::move(backend))} {}

    template <typename Model>
    std::shared_ptr<HandlerBinding> registerHandler() {
        auto binding = std::make_shared<HandlerBinding>();
        binding->typeId = std::string{ModelTraits<Model>::typeId()};
        binding->modelFactory = [] { return ModelFactory::create<Model>(); };
        // Hold the mutex for the full operation: registerModel + push_back
        // must be atomic with respect to switchBackend.
        std::lock_guard lock{_mtx};
        binding->currentId.store(_backend.load()->registerModel(binding->typeId, binding->modelFactory).v);
        _handlers.push_back(binding);
        return binding;
    }

    // Registers a pre-built binding whose modelFactory captures external
    // dependencies that the type-erasing default factory cannot carry.
    void registerHandler(const std::shared_ptr<HandlerBinding>& binding) {
        std::lock_guard lock{_mtx};
        binding->currentId.store(_backend.load()->registerModel(binding->typeId, binding->modelFactory).v);
        _handlers.push_back(binding);
    }

    // Lock ordering: Bridge::_mtx must be acquired before
    // LocalBackend::_regMtx. switchBackend and notifyBackendChanged both
    // follow this ordering. No other path may invert it.
    void switchBackend(std::unique_ptr<IBackend> newBackend) {
        auto newShared = std::shared_ptr<IBackend>(std::move(newBackend));
        std::lock_guard lock{_mtx};

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

    void deregisterHandler(const std::shared_ptr<HandlerBinding>& binding) {
        // Hold the mutex for the full operation: currentId read + deregister
        // + weak_ptr removal must be atomic with respect to switchBackend.
        std::lock_guard lock{_mtx};
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

    template <typename Model, typename Action>
    Completion<typename ActionTraits<Action>::Result> executeVia(const std::shared_ptr<HandlerBinding>& binding,
                                                                 Action action, IExecutor* cbExec) {
        using R = typename ActionTraits<Action>::Result;

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

template <typename Model>
class BridgeHandler {
public:
    BridgeHandler(Bridge& bridge, IExecutor* guiExec)
        : _bridge{bridge}, _guiExec{guiExec}, _binding{bridge.template registerHandler<Model>()} {}

    // Accepts a pre-built HandlerBinding — for dependency-injected factories.
    BridgeHandler(Bridge& bridge, IExecutor* guiExec, std::shared_ptr<HandlerBinding> binding)
        : _bridge{bridge}, _guiExec{guiExec}, _binding{std::move(binding)} {
        _bridge.registerHandler(_binding);
    }

    ~BridgeHandler() { _bridge.deregisterHandler(_binding); }
    BridgeHandler(const BridgeHandler&) = delete;
    BridgeHandler& operator=(const BridgeHandler&) = delete;

    template <typename Action>
    Completion<typename ActionTraits<Action>::Result> execute(Action action) {
        return _bridge.template executeVia<Model, Action>(_binding, std::move(action), _guiExec);
    }

    [[nodiscard]] const std::shared_ptr<HandlerBinding>& binding() const { return _binding; }

private:
    Bridge& _bridge;
    IExecutor* _guiExec;
    std::shared_ptr<HandlerBinding> _binding;
};

}  // namespace morph
