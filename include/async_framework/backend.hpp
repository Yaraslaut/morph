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

struct ActionCall {
    std::string modelTypeId;
    std::string actionTypeId;
    std::function<std::string()> serializeAction;
    std::function<std::shared_ptr<void>(std::string_view)> deserializeResult;
    std::function<std::shared_ptr<void>(IModelHolder&)> localOp;
};

struct IBackend {
    virtual ~IBackend() = default;
    virtual ModelId registerModel(const std::string& typeId,
                                  std::function<std::unique_ptr<IModelHolder>()> factory) = 0;
    virtual void deregisterModel(ModelId) = 0;
    virtual Completion<std::shared_ptr<void>> execute(ModelId, ActionCall, IExecutor* cbExec) = 0;
    // Called by Bridge::switchBackend after all handlers are re-registered on this backend.
    // Implementations call IBackendChangedSink::onBackendChanged() on every live model
    // that implements it. Remote backends (no live model objects) must no-op.
    virtual void notifyBackendChanged() = 0;
};

class LocalBackend : public IBackend {
public:
    explicit LocalBackend(IExecutor& workerPool) : _strand{workerPool} {}

    ModelId registerModel(const std::string& /*typeId*/,
                          std::function<std::unique_ptr<IModelHolder>()> factory) override {
        ModelId mid{_nextId.fetch_add(1) + 1};
        std::lock_guard lock{_regMtx};
        _models[mid] = factory();
        return mid;
    }
    void deregisterModel(ModelId mid) override {
        std::lock_guard lock{_regMtx};
        _models.erase(mid);
    }
    void notifyBackendChanged() override {
        std::lock_guard lock{_regMtx};
        for (auto& [modelId, holder] : _models) {
            if (auto* sink = dynamic_cast<IBackendChangedSink*>(holder.get())) {
                sink->onBackendChanged();
            }
        }
    }
    Completion<std::shared_ptr<void>> execute(ModelId mid, ActionCall call, IExecutor* cbExec) override {
        auto compState = std::make_shared<detail::CompletionState<std::shared_ptr<void>>>();
        Completion<std::shared_ptr<void>> comp{compState, cbExec};

        std::shared_ptr<IModelHolder> holder;
        {
            std::lock_guard lock{_regMtx};
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
