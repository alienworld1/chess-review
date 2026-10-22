#include "engine/EngineProfile.h"
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>

#include <utility>

namespace chess_review::engine {
namespace {
Error malformed() {
    return {ErrorCode::Protocol, QStringLiteral("The engine advertised an invalid UCI option.")};
}
bool text(const QString& value, qsizetype limit, bool empty = false) {
    return (empty || !value.isEmpty()) && value.size() <= limit && value.isValidUtf16() &&
           !value.contains(QChar::Null) && !value.contains(u'\n') && !value.contains(u'\r');
}
bool validOption(const UciOption& option) {
    if (!text(option.name, 4096) || !text(option.defaultValue, 65536, true)) {
        return false;
    }
    switch (option.type) {
    case OptionType::Check:
        return !option.minimum && !option.maximum && option.choices.isEmpty() &&
               (option.defaultValue == QStringLiteral("true") ||
                option.defaultValue == QStringLiteral("false"));
    case OptionType::Spin: {
        bool ok = false;
        const auto value = option.defaultValue.toLongLong(&ok);
        return ok && option.minimum && option.maximum && *option.minimum <= value &&
               value <= *option.maximum && option.choices.isEmpty();
    }
    case OptionType::Combo:
        return !option.minimum && !option.maximum && !option.choices.isEmpty() &&
               option.choices.contains(option.defaultValue) && option.choices.size() <= 1024 &&
               std::all_of(option.choices.begin(), option.choices.end(),
                           [](const QString& choice) { return text(choice, 4096); });
    case OptionType::Button:
        return option.defaultValue.isEmpty() && !option.minimum && !option.maximum &&
               option.choices.isEmpty();
    case OptionType::String:
        return !option.minimum && !option.maximum && option.choices.isEmpty();
    }
    return false;
}
} // namespace
std::variant<UciOption, Error> parseOption(const QString& line) {
    if (!text(line, 65536)) {
        return malformed();
    }
    static const QRegularExpression tokenPattern(QStringLiteral("\\S+"));
    QStringList tokens;
    QList<qsizetype> starts;
    QList<qsizetype> ends;
    auto matches = tokenPattern.globalMatch(line);
    while (matches.hasNext()) {
        const auto match = matches.next();
        tokens.append(match.captured());
        starts.append(match.capturedStart());
        ends.append(match.capturedEnd());
    }
    // Slice by token offsets to retain internal whitespace without rescanning the line per choice.
    const auto span = [&](qsizetype begin, qsizetype end) {
        return begin < end ? line.mid(starts[begin], ends[end - 1] - starts[begin]) : QString();
    };
    if (tokens.size() < 5 || tokens[0] != QStringLiteral("option") ||
        tokens[1] != QStringLiteral("name")) {
        return malformed();
    }
    const auto typeAt = tokens.indexOf(QStringLiteral("type"), 2);
    if (typeAt < 3 || typeAt + 1 >= tokens.size()) {
        return malformed();
    }
    UciOption option;
    option.name = tokens.mid(2, typeAt - 2).join(u' ');
    const auto& kind = tokens[typeAt + 1];
    if (kind == QStringLiteral("check")) {
        option.type = OptionType::Check;
    } else if (kind == QStringLiteral("spin")) {
        option.type = OptionType::Spin;
    } else if (kind == QStringLiteral("combo")) {
        option.type = OptionType::Combo;
    } else if (kind == QStringLiteral("button")) {
        option.type = OptionType::Button;
    } else if (kind == QStringLiteral("string")) {
        option.type = OptionType::String;
    } else {
        return malformed();
    }
    auto index = typeAt + 2;
    if (option.type == OptionType::Button) {
        if (index != tokens.size()) {
            return malformed();
        }
    } else {
        if (index >= tokens.size() || tokens[index++] != QStringLiteral("default")) {
            return malformed();
        }
        if (option.type == OptionType::String) {
            option.defaultValue = span(index, tokens.size());
            if (option.defaultValue == QStringLiteral("<empty>")) {
                option.defaultValue.clear();
            }
        } else if (option.type == OptionType::Combo) {
            const auto firstVar = tokens.indexOf(QStringLiteral("var"), index);
            if (firstVar < 0) {
                return malformed();
            }
            option.defaultValue = span(index, firstVar);
            index = firstVar;
            while (index < tokens.size()) {
                ++index;
                const auto next = tokens.indexOf(QStringLiteral("var"), index);
                const auto end = next < 0 ? tokens.size() : next;
                if (option.choices.size() >= 1024 || index == end) {
                    return malformed();
                }
                option.choices.append(span(index, end));
                index = end;
            }
        } else {
            if (index >= tokens.size()) {
                return malformed();
            }
            option.defaultValue = tokens[index++];
            if (option.type == OptionType::Spin) {
                if (index + 4 != tokens.size() || tokens[index] != QStringLiteral("min") ||
                    tokens[index + 2] != QStringLiteral("max")) {
                    return malformed();
                }
                bool low = false;
                bool high = false;
                option.minimum = tokens[index + 1].toLongLong(&low);
                option.maximum = tokens[index + 3].toLongLong(&high);
                if (!low || !high) {
                    return malformed();
                }
                index += 4;
            }
            if (index != tokens.size()) {
                return malformed();
            }
        }
    }
    if (!validOption(option)) {
        return malformed();
    }
    return option;
}
std::optional<Error> appendOption(Capabilities& capabilities, UciOption option) {
    if (capabilities.options.size() >= 1024) {
        return Error{ErrorCode::OutputLimit,
                     QStringLiteral("The engine advertised more than 1,024 UCI options.")};
    }
    if (!validOption(option) ||
        std::any_of(capabilities.options.begin(), capabilities.options.end(),
                    [&](const UciOption& existing) {
                        return existing.name.compare(option.name, Qt::CaseInsensitive) == 0;
                    })) {
        return malformed();
    }
    const auto spin = option.type == OptionType::Spin;
    if (option.name.compare(QStringLiteral("MultiPV"), Qt::CaseInsensitive) == 0) {
        capabilities.supportsMultiPv = spin && *option.maximum >= 2;
    }
    if (option.name.compare(QStringLiteral("UCI_ShowWDL"), Qt::CaseInsensitive) == 0) {
        capabilities.supportsWdl = option.type == OptionType::Check;
    }
    if (option.name.compare(QStringLiteral("Threads"), Qt::CaseInsensitive) == 0) {
        capabilities.supportsThreads = spin && *option.maximum >= 1;
    }
    if (option.name.compare(QStringLiteral("Hash"), Qt::CaseInsensitive) == 0) {
        capabilities.supportsHash = spin && *option.maximum >= 1;
    }
    capabilities.options.push_back(std::move(option));
    return std::nullopt;
}
QByteArray encodeCapabilities(const Capabilities& capabilities) {
    QJsonArray options;
    for (const auto& option : capabilities.options) {
        QJsonObject object{{QStringLiteral("name"), option.name},
                           {QStringLiteral("type"), static_cast<int>(option.type)},
                           {QStringLiteral("default"), option.defaultValue},
                           {QStringLiteral("choices"), QJsonArray::fromStringList(option.choices)}};
        if (option.minimum) {
            object.insert(QStringLiteral("min"), QString::number(*option.minimum));
        }
        if (option.maximum) {
            object.insert(QStringLiteral("max"), QString::number(*option.maximum));
        }
        options.append(object);
    }
    return QJsonDocument(
               QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("options"), options}})
        .toJson(QJsonDocument::Compact);
}
std::optional<Capabilities> decodeCapabilities(const QByteArray& bytes) {
    if (bytes.size() > 1024 * 1024 || !bytes.isValidUtf8()) {
        return std::nullopt;
    }
    const auto document = QJsonDocument::fromJson(bytes);
    const auto root = document.object();
    if (!document.isObject() || root.size() != 2 ||
        root.value(QStringLiteral("version")) != QJsonValue(1) ||
        !root.value(QStringLiteral("options")).isArray()) {
        return std::nullopt;
    }
    const auto options = root.value(QStringLiteral("options")).toArray();
    if (options.size() > 1024) {
        return std::nullopt;
    }
    Capabilities capabilities;
    for (const auto& value : options) {
        if (!value.isObject()) {
            return std::nullopt;
        }
        const auto object = value.toObject();
        const auto type = object.value(QStringLiteral("type"));
        if (!type.isDouble() || type.toInt(-1) < 0 || type.toInt(-1) > 4 ||
            type.toDouble() != type.toInt() || !object.value(QStringLiteral("name")).isString() ||
            !object.value(QStringLiteral("default")).isString() ||
            !object.value(QStringLiteral("choices")).isArray()) {
            return std::nullopt;
        }
        UciOption option;
        option.name = object.value(QStringLiteral("name")).toString();
        option.type = static_cast<OptionType>(type.toInt());
        option.defaultValue = object.value(QStringLiteral("default")).toString();
        for (const auto& choice : object.value(QStringLiteral("choices")).toArray()) {
            if (!choice.isString()) {
                return std::nullopt;
            }
            option.choices.append(choice.toString());
        }
        for (const auto& key : {QStringLiteral("min"), QStringLiteral("max")}) {
            if (object.contains(key)) {
                bool ok = false;
                if (!object.value(key).isString()) {
                    return std::nullopt;
                }
                const auto number = object.value(key).toString().toLongLong(&ok);
                if (!ok) {
                    return std::nullopt;
                }
                (key == QStringLiteral("min") ? option.minimum : option.maximum) = number;
            }
        }
        if (object.size() != 4 + (option.minimum ? 1 : 0) + (option.maximum ? 1 : 0) ||
            appendOption(capabilities, std::move(option))) {
            return std::nullopt;
        }
    }
    return capabilities;
}
bool validEngine(const ValidatedEngine& engine) {
    if (engine.capabilities.options.size() > 1024) {
        return false;
    }
    qsizetype characters = 0;
    for (const auto& option : engine.capabilities.options) {
        if (!validOption(option)) {
            return false;
        }
        characters += option.name.size() + option.defaultValue.size();
        for (const auto& choice : option.choices) {
            characters += choice.size();
        }
        if (characters > 1024 * 1024) {
            return false;
        }
    }
    if (!text(engine.executablePath, 4096) || !QDir::isAbsolutePath(engine.executablePath) ||
        engine.executableSha256.size() != 32 || !text(engine.reportedName, 4096) ||
        engine.reportedName.trimmed().isEmpty() ||
        (engine.reportedAuthor &&
         (!text(*engine.reportedAuthor, 4096) || engine.reportedAuthor->trimmed().isEmpty())) ||
        engine.detectedVersion || !engine.lastValidatedAt.isValid()) {
        return false;
    }
    const auto decoded = decodeCapabilities(encodeCapabilities(engine.capabilities));
    return decoded && *decoded == engine.capabilities;
}
} // namespace chess_review::engine
