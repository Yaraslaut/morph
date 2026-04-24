#pragma once
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "backend.hpp"

namespace morph {

// NOTE: RemoteServer must be heap-allocated via std::make_shared — handle()
// captures shared_from_this() to prevent use-after-free when the pool outlives
// the server object.
class RemoteServer : public std::enable_shared_from_this<RemoteServer> {
public:
    explicit RemoteServer(IExecutor& workerPool, ActionDispatcher& dispatcher = defaultDispatcher(),
                          ModelRegistryFactory& registry = defaultRegistry())
        : _pool{workerPool}, _strand{workerPool}, _dispatcher{dispatcher}, _registry{registry} {}

    void handle(std::string msg, std::function<void(std::string)> reply) {
        auto self = shared_from_this();
        _pool.post([self, msg = std::move(msg), reply = std::move(reply)]() mutable {
            self->dispatchMessage(msg, reply);
        });
    }

private:
    void dispatchMessage(const std::string& msg, std::function<void(std::string)>& reply) {
        // Split into at most 6 parts to support both the 5-part SimulatedRemote
        // protocol and the 6-part Qt WebSocket protocol (which carries a callId).
        auto parts = split(msg, '|', 6);
        try {
            if (parts[0] == "register") {
                if (parts.size() < 2) { throw std::runtime_error("register requires a typeId"); }
                auto holder = _registry.create(parts[1]);
                ModelId mid{_nextId.fetch_add(1) + 1};
                {
                    std::lock_guard lock{_regMtx};
                    _models[mid] = std::move(holder);
                }
                reply("ok|" + std::to_string(mid.v));
            } else if (parts[0] == "deregister") {
                if (parts.size() < 2) { throw std::runtime_error("deregister requires a modelId"); }
                ModelId mid{std::stoull(parts[1])};
                std::lock_guard lock{_regMtx};
                _models.erase(mid);
                reply("ok");
            } else if (parts[0] == "execute") {
                if (parts.size() == 6) {
                    // Qt WebSocket format: execute|callId|mid|modelTy|actionTy|body
                    std::string callId = parts[1];
                    ModelId mid{std::stoull(parts[2])};
                    dispatchExecute(std::move(callId), mid, parts[3], parts[4], parts[5], reply);
                } else if (parts.size() == 5) {
                    // SimulatedRemote format: execute|mid|modelTy|actionTy|body
                    ModelId mid{std::stoull(parts[1])};
                    dispatchExecute("", mid, parts[2], parts[3], parts[4], reply);
                } else {
                    throw std::runtime_error("execute requires 5 or 6 parts");
                }
            } else {
                reply("err|bad msg type");
            }
        } catch (const std::exception& exc) {
            reply(std::string{"err|"} + exc.what());
        }
    }

    void dispatchExecute(std::string callId, ModelId mid, const std::string& modelTy, const std::string& actionTy,
                         const std::string& body, std::function<void(std::string)> reply) {
        std::shared_ptr<IModelHolder> holder;
        {
            std::lock_guard lock{_regMtx};
            auto iter = _models.find(mid);
            if (iter != _models.end()) {
                holder = iter->second;
            }
        }
        if (!holder) {
            if (callId.empty()) {
                reply("err|model not found");
            } else {
                reply("err|" + callId + "|model not found");
            }
            return;
        }
        _strand.post(mid, [&disp = _dispatcher, callId = std::move(callId), modelTy, actionTy, body,
                           holder = std::move(holder), reply = std::move(reply)]() mutable {
            try {
                auto result = disp.dispatch(modelTy, actionTy, *holder, body);
                if (callId.empty()) {
                    reply("ok|" + result);
                } else {
                    reply("ok|" + callId + "|" + result);
                }
            } catch (const std::exception& exc) {
                if (callId.empty()) {
                    reply(std::string{"err|"} + exc.what());
                } else {
                    reply("err|" + callId + "|" + exc.what());
                }
            }
        });
    }

    static std::vector<std::string> split(const std::string& src, char sep, int maxParts) {
        std::vector<std::string> out;
        std::size_t pos = 0;
        while (static_cast<int>(out.size()) < maxParts - 1) {
            auto found = src.find(sep, pos);
            if (found == std::string::npos) {
                break;
            }
            out.emplace_back(src.substr(pos, found - pos));
            pos = found + 1;
        }
        out.emplace_back(src.substr(pos));
        return out;
    }

    IExecutor& _pool;
    StrandExecutor _strand;
    ActionDispatcher& _dispatcher;
    ModelRegistryFactory& _registry;
    std::mutex _regMtx;
    std::unordered_map<ModelId, std::shared_ptr<IModelHolder>, ModelIdHash> _models;
    std::atomic<uint64_t> _nextId{0};
};

class SimulatedRemoteBackend : public IBackend {
public:
    explicit SimulatedRemoteBackend(RemoteServer& server) : _server{server} {}

    ModelId registerModel(const std::string& typeId, std::function<std::unique_ptr<IModelHolder>()>) override {
        std::promise<ModelId> prom;
        auto fut = prom.get_future();
        _server.handle("register|" + typeId, [&prom](const std::string& reply) {
            if (reply.starts_with("ok|")) {
                prom.set_value(ModelId{std::stoull(reply.substr(3))});
            } else {
                prom.set_exception(std::make_exception_ptr(std::runtime_error("register failed: " + reply)));
            }
        });
        return fut.get();
    }
    void deregisterModel(ModelId mid) override {
        std::promise<void> prom;
        auto fut = prom.get_future();
        _server.handle("deregister|" + std::to_string(mid.v), [&prom](const std::string&) { prom.set_value(); });
        fut.get();
    }
    Completion<std::shared_ptr<void>> execute(ModelId mid, ActionCall call, IExecutor* cbExec) override {
        auto state = std::make_shared<detail::CompletionState<std::shared_ptr<void>>>();
        Completion<std::shared_ptr<void>> comp{state, cbExec};

        std::string body = call.serializeAction();
        std::string msg =
            "execute|" + std::to_string(mid.v) + "|" + call.modelTypeId + "|" + call.actionTypeId + "|" + body;
        auto deser = std::move(call.deserializeResult);

        _server.handle(std::move(msg), [state, deser = std::move(deser)](const std::string& reply) mutable {
            try {
                if (reply.starts_with("ok|")) {
                    state->setValue(deser(reply.substr(3)));
                } else if (reply.starts_with("err|")) {
                    throw std::runtime_error(reply.substr(4));
                } else {
                    throw std::runtime_error("malformed reply: " + reply);
                }
            } catch (...) {
                state->setException(std::current_exception());
            }
        });
        return comp;
    }
    // Models live in RemoteServer, not here — no local objects to notify.
    void notifyBackendChanged() override {}

private:
    RemoteServer& _server;
};

}  // namespace morph
