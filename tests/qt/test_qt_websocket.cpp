// SPDX-License-Identifier: Apache-2.0

#include <QCoreApplication>
#include <QFile>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <async_framework/bridge.hpp>
#include <async_framework/executor.hpp>
#include <async_framework/qt/qt_executor.hpp>
#include <async_framework/qt/qt_websocket_backend.hpp>
#include <async_framework/qt/qt_websocket_server.hpp>
#include <async_framework/registry.hpp>
#include <async_framework/remote.hpp>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <stdexcept>
#include <thread>

using namespace morph;

// ── Shared QCoreApplication ──────────────────────────────────────────────────
// Must exist before any Qt networking. Created once for the process.
// Qt requires non-null argv and argc >= 1.
static QCoreApplication* ensureApp() {
    static int argc = 1;
    static const char* argv0 = "async_framework_qt_tests";
    // NOLINTNEXTLINE(modernize-avoid-c-arrays) -- QCoreApplication requires char* argv[]
    static char* argv[] = {const_cast<char*>(argv0), nullptr};
    static QCoreApplication app{argc, argv};
    return &app;
}

// ── Test model (identical scenarios to test_bridge_remote.cpp) ───────────────
struct WsEchoAction {
    int value = 0;
};
struct WsEchoFail {};

struct WsEchoModel {
    int execute(WsEchoAction action) { return action.value; }
    int execute(WsEchoFail) { throw std::runtime_error("echo failed"); }
};

BRIDGE_REGISTER_MODEL(WsEchoModel, "WsEchoModel")
BRIDGE_REGISTER_ACTION(WsEchoModel, WsEchoAction, "WsEchoAction")
BRIDGE_REGISTER_ACTION(WsEchoModel, WsEchoFail, "WsEchoFail")

// ── Poll helper — pumps Qt event loop while waiting ──────────────────────────
static void pumpUntil(const std::function<bool()>& done, int maxIterations = 50) {
    for (int idx = 0; idx < maxIterations && !done(); ++idx) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Drain any deferred deletes so Qt objects can be safely destroyed next.
    QCoreApplication::processEvents(QEventLoop::AllEvents | QEventLoop::ExcludeUserInputEvents);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

// ── TLS config helpers ───────────────────────────────────────────────────────
static QSslConfiguration makeServerTlsConfig() {
    QFile certFile{QStringLiteral(TESTS_CERTS_DIR "/server.crt")};
    QFile keyFile{QStringLiteral(TESTS_CERTS_DIR "/server.key")};
    certFile.open(QIODevice::ReadOnly);
    keyFile.open(QIODevice::ReadOnly);

    QSslConfiguration cfg;
    cfg.setLocalCertificate(QSslCertificate{&certFile, QSsl::Pem});
    cfg.setPrivateKey(QSslKey{&keyFile, QSsl::Rsa, QSsl::Pem});
    return cfg;
}

static QSslConfiguration makeClientTlsConfig() {
    QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
    cfg.setPeerVerifyMode(QSslSocket::VerifyNone);
    return cfg;
}

// ── Plain WebSocket tests ────────────────────────────────────────────────────

TEST_CASE("QtWebSocketBackend: action result delivered via then", "[qt][ws]") {
    ensureApp();
    ThreadPoolExecutor serverPool{2};
    RemoteServer server{serverPool};
    QtWebSocketServer wsServer{server, 0};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};
    auto backendPtr = std::make_unique<QtWebSocketBackend>(url);
    REQUIRE(backendPtr->waitForConnected());

    QtExecutor qtExec;
    Bridge bridge{std::move(backendPtr)};
    std::atomic<int> result{-1};  // declared BEFORE handler so it outlives it
    BridgeHandler<WsEchoModel> handler{bridge, &qtExec};

    handler.execute(WsEchoAction{99}).then([&](int val) { result.store(val); }).onError([](const std::exception_ptr&) {
    });

    pumpUntil([&] { return result.load() != -1; });
    REQUIRE(result.load() == 99);
}

TEST_CASE("QtWebSocketBackend: exception delivered via onError", "[qt][ws]") {
    ensureApp();
    ThreadPoolExecutor serverPool{2};
    RemoteServer server{serverPool};
    QtWebSocketServer wsServer{server, 0};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};
    auto backendPtr = std::make_unique<QtWebSocketBackend>(url);
    REQUIRE(backendPtr->waitForConnected());

    QtExecutor qtExec;
    Bridge bridge{std::move(backendPtr)};
    BridgeHandler<WsEchoModel> handler{bridge, &qtExec};

    std::atomic<bool> errorFired{false};
    handler.execute(WsEchoFail{}).then([](int) {}).onError([&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const std::runtime_error&) {
            errorFired.store(true);
        }
    });

    pumpUntil([&] { return errorFired.load(); });
    REQUIRE(errorFired.load());
}

TEST_CASE("QtWebSocketBackend: multiple actions on same handler", "[qt][ws]") {
    ensureApp();
    ThreadPoolExecutor serverPool{2};
    RemoteServer server{serverPool};
    QtWebSocketServer wsServer{server, 0};
    REQUIRE(wsServer.listen());

    QUrl url{QString("ws://127.0.0.1:%1").arg(wsServer.port())};
    auto backendPtr = std::make_unique<QtWebSocketBackend>(url);
    REQUIRE(backendPtr->waitForConnected());

    QtExecutor qtExec;
    Bridge bridge{std::move(backendPtr)};
    BridgeHandler<WsEchoModel> handler{bridge, &qtExec};

    std::atomic<int> sum{0};
    std::atomic<int> count{0};
    constexpr int numActions = 5;

    for (int idx = 1; idx <= numActions; ++idx) {
        handler.execute(WsEchoAction{idx})
            .then([&](int val) {
                sum.fetch_add(val);
                count.fetch_add(1);
            })
            .onError([](const std::exception_ptr&) {});
    }

    pumpUntil([&] { return count.load() == numActions; }, 100);
    REQUIRE(count.load() == numActions);
    REQUIRE(sum.load() == 15);  // 1+2+3+4+5
}

// ── TLS WebSocket tests ──────────────────────────────────────────────────────

TEST_CASE("QtWebSocketBackend TLS: action result delivered via then", "[qt][wss]") {
    ensureApp();
    ThreadPoolExecutor serverPool{2};
    RemoteServer server{serverPool};
    QtWebSocketServer wsServer{server, 0, makeServerTlsConfig()};
    REQUIRE(wsServer.listen());

    QUrl url{QString("wss://127.0.0.1:%1").arg(wsServer.port())};
    auto backendPtr =
        std::make_unique<QtWebSocketBackend>(url, defaultDispatcher(), defaultRegistry(), makeClientTlsConfig());
    REQUIRE(backendPtr->waitForConnected());

    QtExecutor qtExec;
    Bridge bridge{std::move(backendPtr)};
    BridgeHandler<WsEchoModel> handler{bridge, &qtExec};

    std::atomic<int> result{-1};
    handler.execute(WsEchoAction{99}).then([&](int val) { result.store(val); }).onError([](const std::exception_ptr&) {
    });

    pumpUntil([&] { return result.load() != -1; });
    REQUIRE(result.load() == 99);
}

TEST_CASE("QtWebSocketBackend TLS: exception delivered via onError", "[qt][wss]") {
    ensureApp();
    ThreadPoolExecutor serverPool{2};
    RemoteServer server{serverPool};
    QtWebSocketServer wsServer{server, 0, makeServerTlsConfig()};
    REQUIRE(wsServer.listen());

    QUrl url{QString("wss://127.0.0.1:%1").arg(wsServer.port())};
    auto backendPtr =
        std::make_unique<QtWebSocketBackend>(url, defaultDispatcher(), defaultRegistry(), makeClientTlsConfig());
    REQUIRE(backendPtr->waitForConnected());

    QtExecutor qtExec;
    Bridge bridge{std::move(backendPtr)};
    BridgeHandler<WsEchoModel> handler{bridge, &qtExec};

    std::atomic<bool> errorFired{false};
    handler.execute(WsEchoFail{}).then([](int) {}).onError([&](const std::exception_ptr& exc) {
        try {
            std::rethrow_exception(exc);
        } catch (const std::runtime_error&) {
            errorFired.store(true);
        }
    });

    pumpUntil([&] { return errorFired.load(); });
    REQUIRE(errorFired.load());
}
