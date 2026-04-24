// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "completion.hpp"
#include "model.hpp"
#include "registry.hpp"
#include "strand.hpp"
#include "task.hpp"

namespace morph {

/// @brief All the information needed to dispatch one action call through a backend.
///
/// Backends that execute locally use `localOp` directly. Remote backends
/// serialize via `serializeAction` and deserialize replies via `deserializeResult`.
struct ActionCall {
    /// @brief String id of the target model type (from `ModelTraits`).
    std::string modelTypeId;

    /// @brief String id of the action type (from `ActionTraits`).
    std::string actionTypeId;

    /// @brief Serialises the action to JSON. Called only on the remote path.
    std::function<std::string()> serializeAction;

    /// @brief Deserialises a JSON reply into the opaque result `shared_ptr<void>`.
    std::function<std::shared_ptr<void>(std::string_view)> deserializeResult;

    /// @brief Executes the action directly against a model holder. Used on the local path.
    std::function<std::shared_ptr<void>(IModelHolder&)> localOp;
};

/// @brief Abstract interface for execution backends (local, remote, …).
///
/// A backend owns model instances and dispatches actions against them.
/// `Bridge` holds one active backend at a time and can swap it atomically
/// via `Bridge::switchBackend()`.
struct IBackend {
    virtual ~IBackend() = default;

    /// @brief Registers a new model instance and returns its opaque id.
    ///
    /// @param typeId  String identifier for the model type (from `ModelTraits`).
    /// @param factory Called once to construct the `IModelHolder`.
    /// @return Stable `ModelId` that identifies this instance within the backend.
    virtual ModelId registerModel(const std::string& typeId,
                                  std::function<std::unique_ptr<IModelHolder>()> factory) = 0;

    /// @brief Removes the model identified by @p mid from the backend.
    ///
    /// @p mid must have been returned by a prior `registerModel()` call on this
    /// backend. No-op if the id is not found.
    /// @param mid Id of the model to remove.
    virtual void deregisterModel(ModelId mid) = 0;

    /// @brief Dispatches @p call against the model identified by @p mid.
    ///
    /// @param mid    Target model id.
    /// @param call   Bundled action description.
    /// @param cbExec Executor on which the `Completion` callbacks are posted.
    /// @return A `Completion` that resolves with the opaque result or an error.
    virtual Completion<std::shared_ptr<void>> execute(ModelId mid, ActionCall call, IExecutor* cbExec) = 0;

    /// @brief Called by `Bridge::switchBackend()` after all handlers are re-registered.
    ///
    /// Implementations should call `IBackendChangedSink::onBackendChanged()` on
    /// every live model that implements it. Remote backends (which hold no local
    /// model objects) must no-op.
    virtual void notifyBackendChanged() = 0;
};

/// @brief In-process backend that executes model actions on a thread pool strand.
///
/// Each model instance gets its own strand so actions are serialised per-model
/// without a global lock on the pool.
class LocalBackend : public IBackend {
public:
    /// @brief Constructs the backend using @p workerPool to run model actions.
    /// @param workerPool Executor (typically a `ThreadPoolExecutor`) for model work.
    explicit LocalBackend(IExecutor& workerPool) : _strand{workerPool} {}

    /// @brief Creates a model instance via @p factory and registers it.
    ///
    /// @p typeId is accepted for interface compatibility but not used — the
    /// concrete type is captured by the factory closure.
    /// @param factory  Callable that constructs the `IModelHolder`.
    /// @return Newly assigned `ModelId`.
    ModelId registerModel(const std::string& /*typeId*/,
                          std::function<std::unique_ptr<IModelHolder>()> factory) override {
        ModelId mid{_nextId.fetch_add(1) + 1};
        std::scoped_lock lock{_regMtx};
        _models[mid] = factory();
        return mid;
    }

    /// @brief Removes the model with @p mid. Thread-safe.
    /// @param mid Id returned by a prior `registerModel()` call.
    void deregisterModel(ModelId mid) override {
        std::scoped_lock lock{_regMtx};
        _models.erase(mid);
    }

    /// @brief Notifies every live model that implements `IBackendChangedSink`. Thread-safe.
    void notifyBackendChanged() override {
        std::scoped_lock lock{_regMtx};
        for (auto& [modelId, holder] : _models) {
            if (auto* sink = dynamic_cast<IBackendChangedSink*>(holder.get())) {
                sink->onBackendChanged();
            }
        }
    }

    /// @brief Schedules `call.localOp` on the model's strand and returns a `Completion`.
    ///
    /// The completion resolves with the opaque result on the strand thread and
    /// the callbacks are delivered via @p cbExec.
    ///
    /// @param mid    Target model id.
    /// @param call   Bundled action; `localOp` is the only field used here.
    /// @param cbExec Executor for delivering callbacks.
    /// @return Completion that will carry the result or an exception.
    Completion<std::shared_ptr<void>> execute(ModelId mid, ActionCall call, IExecutor* cbExec) override {
        auto compState = std::make_shared<detail::CompletionState<std::shared_ptr<void>>>();
        Completion<std::shared_ptr<void>> comp{compState, cbExec};

        std::shared_ptr<IModelHolder> holder;
        {
            std::scoped_lock lock{_regMtx};
            auto iter = _models.find(mid);
            if (iter != _models.end()) {
                holder = iter->second;
            }
        }
        if (!holder) {
            compState->setException(
                std::make_exception_ptr(std::runtime_error("model not found: id=" + std::to_string(mid.v))));
            return comp;
        }
        auto localOp = std::move(call.localOp);
        _strand.post(mid, [localOp = std::move(localOp), holder = std::move(holder), compState]() mutable {
            auto task = [&]() -> detail::Task<std::shared_ptr<void>> { co_return localOp(*holder); }();
            task.state()->attach([compState](auto& taskState) {
                if (taskState.value) {
                    compState->setValue(std::move(*taskState.value));
                } else {
                    compState->setException(taskState.error);
                }
            });
        });
        return comp;
    }

private:
    StrandExecutor _strand;
    std::mutex _regMtx;
    std::unordered_map<ModelId, std::shared_ptr<IModelHolder>, ModelIdHash> _models;
    std::atomic<uint64_t> _nextId{0};
};

}  // namespace morph
