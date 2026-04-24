#pragma once
#include <QObject>
#include <QSslConfiguration>
#include <QWebSocket>
#include <QWebSocketServer>
#include <async_framework/remote.hpp>
#include <optional>
#include <vector>

namespace morph {

class QtWebSocketServer : public QObject {
    Q_OBJECT
public:
    // tls == nullopt  →  plain ws://   (NonSecureMode)
    // tls has value   →  secure wss:// (SecureMode); caller builds QSslConfiguration
    //                    from cert file, Qt resource, byte array, etc.
    explicit QtWebSocketServer(RemoteServer& server, quint16 port = 0,
                               std::optional<QSslConfiguration> tls = std::nullopt, QObject* parent = nullptr);
    ~QtWebSocketServer() override;

    bool listen();
    [[nodiscard]] quint16 port() const;
    void close();

    Q_SLOT void onNewConnection();
    Q_SLOT void onTextMessage(const QString& message);
    Q_SLOT void onDisconnected();

    RemoteServer& _server;
    quint16 _requestedPort;
    QWebSocketServer _wsServer;
    std::vector<QWebSocket*> _clients;
};

}  // namespace morph
