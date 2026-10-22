#pragma once
#include "engine/ExecutableValidator.h"
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

namespace chess_review::engine {
class ValidatorTestAccess {
  public:
    static void configure(ExecutableValidator& validator, const QString& scenario,
                          int deadline = 300, qsizetype outputLimit = 1024 * 1024) {
        validator.setTestArguments({QStringLiteral("--scenario"), scenario}, deadline, deadline,
                                   outputLimit);
    }
};
} // namespace chess_review::engine
namespace engine_test {
inline QJsonObject write(const QString& data, int delay = 0) {
    return {{QStringLiteral("type"), QStringLiteral("write")},
            {QStringLiteral("stream"), QStringLiteral("stdout")},
            {QStringLiteral("data"), data},
            {QStringLiteral("afterMs"), delay}};
}
inline QJsonObject step(const QString& command, const QJsonArray& actions) {
    return {{QStringLiteral("expect"), command}, {QStringLiteral("actions"), actions}};
}
inline QString scenario(const QString& path, const QJsonArray& steps) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        qFatal("Cannot create engine scenario");
    }
    const auto bytes =
        QJsonDocument(QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("steps"), steps}})
            .toJson();
    if (file.write(bytes) != bytes.size()) {
        qFatal("Cannot write engine scenario");
    }
    return path;
}
inline QJsonArray handshake(
    const QString& identity = QStringLiteral("id name Test engine\nid author Tests\nuciok\n")) {
    return {step(QStringLiteral("uci"), {write(identity)}),
            step(QStringLiteral("isready"), {write(QStringLiteral("readyok\n"))}),
            step(QStringLiteral("quit"),
                 {QJsonObject{{QStringLiteral("type"), QStringLiteral("exit")}}})};
}
// Ensure even a failed assertion closes the real process and joins its owner thread.
class Validator final {
  public:
    ~Validator() {
        QEventLoop loop;
        QObject::connect(&service, &chess_review::engine::ExecutableValidator::closed, &loop,
                         &QEventLoop::quit);
        bool closed = false;
        QObject::connect(&service, &chess_review::engine::ExecutableValidator::closed, &loop,
                         [&] { closed = true; });
        QTimer::singleShot(6000, &loop, &QEventLoop::quit);
        service.close();
        loop.exec();
        if (!closed) {
            qFatal("Validator did not close");
        }
    }
    chess_review::engine::ExecutableValidator service;
};
} // namespace engine_test
