#pragma once
#include <async_framework/executor.hpp>
#include <QCoreApplication>
#include <QMetaObject>
#include <functional>

namespace morph {

// Posts callbacks to the Qt event loop (GUI thread) via QMetaObject::invokeMethod.
// Thread-safe: can be called from any thread. No QObject subclass needed.
class QtExecutor : public IExecutor {
public:
    void post(std::function<void()> fn) override {
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            std::move(fn),
            Qt::QueuedConnection
        );
    }
};

}  // namespace morph
