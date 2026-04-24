#pragma once
#include <QEventLoop>
#include <QSslConfiguration>
#include <QUrl>
#include <QWebSocket>
#include <async_framework/backend.hpp>
#include <async_framework/registry.hpp>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace morph {

class QtWebSocketBackend : public IBackend {
public:
    // tls == nullopt  →  ws://  (plain)
    // tls has value   →  wss:// (TLS); applied to QWebSocket before open().
    //                    For self-signed certs: set VerifyNone in the config.
    explicit QtWebSocketBackend(QUrl serverUrl, ActionDispatcher& dispatcher = defaultDispatcher(),
                                ModelRegistryFactory& registry = defaultRegistry(),
                                std::optional<QSslConfiguration> tls = std::nullopt);
    ~QtWebSocketBackend() override;

    // Pumps event loop until connected or timeoutMs elapses. Returns false on timeout.
    bool waitForConnected(int timeoutMs = 5000);

    ModelId registerModel(const std::string& typeId, std::function<std::unique_ptr<IModelHolder>()> factory) override;
    void deregisterModel(ModelId mid) override;
    Completion<std::shared_ptr<void>> execute(ModelId mid, ActionCall call, IExecutor* cbExec) override;
    // Remote backend holds no live model objects — no-op per IBackend contract.
    void notifyBackendChanged() override {}

private:
    // Sends msg and blocks (via nested QEventLoop) until a reply arrives.
    // Only used for register/deregister. Must be called on the Qt event loop thread.
    std::string sendSync(const std::string& msg);

    void onTextMessage(const QString& message);

    QWebSocket _socket;
    bool _connected{false};

    std::string _pendingReply;
    QEventLoop* _syncLoop{nullptr};

    struct PendingExecute {
        std::shared_ptr<detail::CompletionState<std::shared_ptr<void>>> state;
        std::function<std::shared_ptr<void>(std::string_view)> deserialize;
        IExecutor* cbExec;
    };
    uint64_t _nextCallId{0};
    std::unordered_map<uint64_t, PendingExecute> _pending;
    std::mutex _pendingMtx;
};

}  // namespace morph
