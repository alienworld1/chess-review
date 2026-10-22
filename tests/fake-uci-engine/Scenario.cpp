#include "Scenario.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <utility>

namespace fake_uci {
namespace {
bool keysAre(const QJsonObject& object, std::initializer_list<QStringView> allowed) {
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        bool found = false;
        for (const auto key : allowed) {
            found = found || it.key() == key;
        }
        if (!found) {
            return false;
        }
    }
    return true;
}
bool unsignedInt(const QJsonValue& value, int& result) {
    if (!value.isDouble()) {
        return false;
    }
    const auto number = value.toDouble();
    if (!std::isfinite(number) || number < 0 || number > std::numeric_limits<int>::max() ||
        std::floor(number) != number) {
        return false;
    }
    result = static_cast<int>(number);
    return true;
}
} // namespace

QByteArray normalizedCommand(const QByteArray& command) {
    QByteArray result;
    bool space = false;
    for (const char byte : command) {
        if (byte == ' ' || byte == '\t') {
            space = !result.isEmpty();
        } else {
            if (space) {
                result += ' ';
            }
            result += byte;
            space = false;
        }
    }
    return result;
}

std::optional<Scenario> readScenario(const QString& path, QString& error) {
    error = QStringLiteral("invalid scenario");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("cannot open scenario");
        return std::nullopt;
    }
    const auto bytes = file.read(kMaxBytes + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > kMaxBytes || !file.atEnd() ||
        !bytes.isValidUtf8()) {
        error = QStringLiteral("unreadable, oversized, or non-UTF-8 scenario");
        return std::nullopt;
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    const auto root = document.object();
    int version = 0;
    if (!keysAre(root, {u"version", u"steps"}) || !unsignedInt(root.value(u"version"), version) ||
        version != 1 || !root.value(u"steps").isArray()) {
        return std::nullopt;
    }
    const auto steps = root.value(u"steps").toArray();
    if (steps.isEmpty() || steps.size() > kMaxActions) {
        return std::nullopt;
    }
    Scenario scenario;
    qsizetype totalActions = 0;
    bool hasTerminal = false;
    for (const auto& value : steps) {
        if (!value.isObject()) {
            return std::nullopt;
        }
        const auto object = value.toObject();
        if (!keysAre(object, {u"expect", u"actions"}) || !object.value(u"expect").isString() ||
            !object.value(u"actions").isArray()) {
            return std::nullopt;
        }
        const auto expected = object.value(u"expect").toString().toUtf8();
        if (expected.size() > kMaxLineBytes || expected.contains('\n') || expected.contains('\r') ||
            normalizedCommand(expected).isEmpty()) {
            return std::nullopt;
        }
        Step step{normalizedCommand(expected), {}};
        for (const auto& actionValue : object.value(u"actions").toArray()) {
            if (++totalActions > kMaxActions || !actionValue.isObject()) {
                return std::nullopt;
            }
            const auto actionObject = actionValue.toObject();
            if (!actionObject.value(u"type").isString()) {
                return std::nullopt;
            }
            const auto type = actionObject.value(u"type").toString();
            Action action;
            if (actionObject.contains(u"afterMs") &&
                !unsignedInt(actionObject.value(u"afterMs"), action.afterMs)) {
                return std::nullopt;
            }
            if (type == u"write") {
                if (!keysAre(actionObject, {u"type", u"afterMs", u"stream", u"data"}) ||
                    !actionObject.value(u"data").isString() ||
                    !actionObject.value(u"stream").isString()) {
                    return std::nullopt;
                }
                const auto stream = actionObject.value(u"stream").toString();
                if (stream != u"stdout" && stream != u"stderr") {
                    return std::nullopt;
                }
                action.type = ActionType::Write;
                action.stderrStream = stream == u"stderr";
                action.data = actionObject.value(u"data").toString().toUtf8();
            } else {
                if (!keysAre(actionObject, {u"type", u"afterMs"})) {
                    return std::nullopt;
                }
                if (type == u"cancelPending") {
                    action.type = ActionType::CancelPending;
                    if (action.afterMs != 0) {
                        return std::nullopt;
                    }
                } else if (type == u"exit") {
                    action.type = ActionType::Exit;
                } else if (type == u"crash") {
                    action.type = ActionType::Crash;
                } else if (type == u"hang") {
                    action.type = ActionType::Hang;
                } else {
                    return std::nullopt;
                }
                hasTerminal = hasTerminal || action.type == ActionType::Exit ||
                              action.type == ActionType::Hang || action.type == ActionType::Crash;
            }
            step.actions.push_back(std::move(action));
        }
        scenario.steps.push_back(std::move(step));
    }
    if (!hasTerminal) {
        error = QStringLiteral("scenario requires an exit, crash, or hang action");
        return std::nullopt;
    }
    error.clear();
    return scenario;
}
} // namespace fake_uci
