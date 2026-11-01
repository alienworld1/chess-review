#pragma once

#include "EngineTestSupport.h"
#include "engine/private/SearchProtocol.h"
#include "pgn/Pgn.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace review_acceptance {
inline std::shared_ptr<const chess_review::pgn::PgnDocument> readGame(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qFatal("Cannot open acceptance PGN");
    }
    const auto bytes = file.readAll();
    chess_review::pgn::PgnReader reader(
        std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
    auto result = reader.next();
    auto* document = std::get_if<chess_review::pgn::PgnDocument>(&result);
    if (!document) {
        qFatal("Invalid acceptance PGN");
    }
    return std::make_shared<const chess_review::pgn::PgnDocument>(std::move(*document));
}

inline QString handshake() {
    return QStringLiteral("id name Acceptance engine\nid author Tests\noption name MultiPV type "
                          "spin default 1 min 1 max 3\nuciok\n");
}

// Coherent synthetic scores deliberately make each non-best move expensive for its mover.
// They are protocol evidence, not a claim about these games' objective chess evaluations.
inline QJsonArray searches(const chess_review::pgn::PgnDocument& document, int first = 0,
                           int end = -1, int delay = 0, bool crash = false) {
    using namespace chess_review;
    const auto ready = [] {
        return engine_test::step(QStringLiteral("isready"),
                                 {engine_test::write(QStringLiteral("readyok\n"))});
    };
    QJsonArray steps{engine_test::step(QStringLiteral("uci"), {engine_test::write(handshake())}),
                     ready()};
    engine::Capabilities capabilities;
    capabilities.supportsMultiPv = true;
    capabilities.options.push_back(
        {QStringLiteral("MultiPV"), engine::OptionType::Spin, QStringLiteral("1"), 1, 3, {}});
    const auto moves = document.originalMainline();
    const int total = static_cast<int>(moves.size()) + 1;
    for (int stage = 0; stage < (end < 0 ? 3 : 1); ++stage) {
        const std::uint64_t budget = stage == 0 ? 100000 : stage == 1 ? 1000000 : 5000000;
        auto position = std::get<chess::Position>(document.tree.position(document.tree.rootId()));
        for (int index = 0; index < total; ++index) {
            auto legal = position.legalMoves();
            // A rank-1 played move with a large child loss creates a search conflict.
            // These general journey fixtures intend ordinary, equal non-best losses;
            // the adaptive queue tests independently exercise conflict prioritization.
            if (index < static_cast<int>(moves.size()) && legal.size() > 1 &&
                legal.front() == moves[static_cast<std::size_t>(index)]) {
                std::swap(legal[0], legal[1]);
            }
            if ((stage != 0 || index >= first) && (end < 0 || index < end) && !legal.empty()) {
                engine::AnalysisRequest request;
                request.id = QUuid::createUuid();
                request.position = position;
                request.wantWdl = true;
                request.limit = engine::NodeLimit{budget};
                request.multiPv = stage == 0 ? 1 : 3;
                const auto prepared =
                    std::get<engine::PreparedSearch>(engine::prepareSearch(request, capabilities));
                for (const auto& command : prepared.options.split('\n')) {
                    if (!command.isEmpty()) {
                        steps.append(engine_test::step(QString::fromUtf8(command), {}));
                    }
                }
                steps.append(engine_test::step(QStringLiteral("ucinewgame"), {}));
                steps.append(ready());
                steps.append(engine_test::step(QString::fromUtf8(prepared.position).trimmed(), {}));
                QJsonArray actions;
                if (crash && index == first + 1) {
                    actions.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("crash")}});
                } else {
                    QString output;
                    for (int rank = 0; rank < prepared.effectiveMultiPv; ++rank) {
                        auto continuation = position;
                        QString pv;
                        for (int ply = 0; ply < 3; ++ply) {
                            const auto choices = continuation.legalMoves();
                            if (choices.empty()) {
                                break;
                            }
                            const auto move =
                                ply == 0 ? legal[static_cast<std::size_t>(rank)] : choices.front();
                            if (!pv.isEmpty()) {
                                pv += QLatin1Char(' ');
                            }
                            pv += QString::fromStdString(
                                std::get<std::string>(continuation.uci(move)));
                            if (!std::holds_alternative<std::monostate>(continuation.play(move))) {
                                qFatal("Invalid acceptance continuation");
                            }
                        }
                        output +=
                            QStringLiteral("info depth 7 nodes %1 multipv %2 score cp 200 pv %3\n")
                                .arg(budget)
                                .arg(rank + 1)
                                .arg(pv);
                    }
                    output += QStringLiteral("bestmove %1\n")
                                  .arg(QString::fromStdString(
                                      std::get<std::string>(position.uci(legal.front()))));
                    actions.append(engine_test::write(output, delay));
                }
                steps.append(engine_test::step(QStringLiteral("go nodes %1").arg(budget), actions));
                steps.append(ready());
            }
            if (index < static_cast<int>(moves.size()) &&
                !std::holds_alternative<std::monostate>(
                    position.play(moves[static_cast<std::size_t>(index)]))) {
                qFatal("Invalid acceptance mainline");
            }
        }
    }
    steps.append(engine_test::step(
        QStringLiteral("quit"), {QJsonObject{{QStringLiteral("type"), QStringLiteral("exit")}}}));
    return steps;
}
} // namespace review_acceptance
