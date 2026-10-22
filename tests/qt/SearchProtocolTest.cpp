#include "engine/private/SearchProtocol.h"
#include "TestEnvironment.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTest>
#include <cstddef>
#include <limits>
#include <random>
#include <utility>
#include <variant>
#include <vector>

using namespace chess_review;
using namespace chess_review::engine;
namespace {
QStringList tokens(const QString& text) {
    return text.simplified().split(u' ', Qt::SkipEmptyParts);
}
AnalysisRequest request() {
    AnalysisRequest result;
    result.id = QUuid::createUuid();
    return result;
}
} // namespace
class SearchProtocolTest final : public QObject {
    Q_OBJECT
  private slots:
    void init() { QTest::failOnWarning(); }
    void invalidRecords_data() {
        QTest::addColumn<QString>("text");
        const QStringList cases{"",
                                "depth 1",
                                "bestmove e2e4",
                                "info depth",
                                "info depth 1 depth 2",
                                "info score cp 1 score cp 2",
                                "info pv e2e4 pv d2d4",
                                "info multipv 0",
                                "info multipv 2",
                                "info score cp 9223372036854775808",
                                "info score cp -9223372036854775809",
                                "info nodes 18446744073709551616",
                                "info depth -1",
                                "info score cp +",
                                "info score cp 1 lowerbound upperbound",
                                "info upperbound",
                                "info wdl 1000 1 0",
                                "info wdl -1 1 1000",
                                "info wdl 1 999",
                                "info wdl 0 0 1000 wdl 0 0 1000",
                                "info hashfull 1001",
                                "info currmove",
                                "info currmove e2e5",
                                "info currmove e2e4 currmove d2d4",
                                "info currmovenumber",
                                "info currmovenumber 0",
                                "info currmovenumber -1",
                                "info currmovenumber 2147483648",
                                "info currmovenumber 1 currmovenumber 2",
                                "info cpuload",
                                "info cpuload 1001",
                                "info cpuload -1",
                                "info cpuload 1 cpuload 1",
                                "info sbhits",
                                "info sbhits -1",
                                "info sbhits 18446744073709551616",
                                "info sbhits 0 sbhits 1",
                                "info refutation",
                                "info refutation depth 1",
                                "info refutation e2e5",
                                "info refutation e2e4 e7e",
                                "info currline 0",
                                "info currline -1",
                                "info currline +1",
                                "info currline 2147483648",
                                "info currline e2e4 e7e5qq",
                                "info pv e2e4 e7e5garbage",
                                "info pv e2e4 e7e5 e2e3"};
        for (const auto& text : cases) {
            QTest::newRow(qPrintable(text)) << text;
        }
    }
    void invalidRecords() {
        QFETCH(QString, text);
        auto search = request();
        const auto before = search.position.context();
        QVERIFY(std::holds_alternative<Error>(parseSearchInfo(tokens(text), search, 1)));
        QCOMPARE(search.position.context(), before);
    }
    void auxiliaryFieldsDoNotCreateEvidence() {
        const auto search = request();
        const auto parsed = parseSearchInfo(
            tokens(QStringLiteral(
                "info currmove e2e4 currmovenumber 1 cpuload 1000 sbhits 18446744073709551615 "
                "refutation d2d4 d7d5 refutation e2e4 currline 2 e2e4 e7e5 "
                "currline g1f3 currline 1 currline")),
            search, 1);
        QVERIFY(std::holds_alternative<InfoUpdate>(parsed));
        const auto& update = std::get<InfoUpdate>(parsed);
        QVERIFY(!update.hasEvidence);
        QVERIFY(update.line.pv.empty());
        AnalysisSnapshot snapshot;
        updateEvidence(snapshot, update);
        QVERIFY(snapshot.lines.empty());
        QCOMPARE(snapshot.achievedWidth, 0);
        QVERIFY(!snapshot.commonDepth);
    }
    void mixedFieldsAndAtomicFailure() {
        const auto search = request();
        const auto parsed = parseSearchInfo(
            tokens(QStringLiteral(
                "info vendor foo currline e2e4 e7e5 depth 4 refutation d2d4 d7d5 "
                "score mate -2 upperbound wdl 0 0 1000 pv e2e4 e7e5 extension value "
                "nodes 18446744073709551615 time 0 nps 0 hashfull 0 seldepth 7 tbhits 0 multipv 1 "
                "currmove e2e4 cpuload 0 currmovenumber 20 sbhits 0 string score broken pv e2e5")),
            search, 1);
        QVERIFY(std::holds_alternative<InfoUpdate>(parsed));
        const auto& line = std::get<InfoUpdate>(parsed).line;
        QCOMPARE(line.pv.size(), std::size_t(2));
        QCOMPARE(line.score->type, ScoreType::Mate);
        QCOMPARE(line.score->value, -2);
        QCOMPARE(line.score->bound, ScoreBound::Upper);
        QCOMPARE(line.wdl->losses, 1000);
        QCOMPARE(line.statistics.nodes, std::numeric_limits<std::uint64_t>::max());
        AnalysisSnapshot snapshot;
        updateEvidence(snapshot, std::get<InfoUpdate>(parsed));
        QCOMPARE(snapshot.achievedWidth, 1);
        QCOMPARE(snapshot.commonDepth, 4);
        const auto auxiliary = parseSearchInfo(
            tokens(QStringLiteral("info currline d2d4 d7d5 refutation e2e4 cpuload 500 sbhits 9")),
            search, 1);
        QVERIFY(std::holds_alternative<InfoUpdate>(auxiliary));
        updateEvidence(snapshot, std::get<InfoUpdate>(auxiliary));
        QCOMPARE(snapshot.lines.front().score->value, -2);
        QCOMPARE(snapshot.statistics.nodes, std::numeric_limits<std::uint64_t>::max());
        QCOMPARE(snapshot.achievedWidth, 1);
        QCOMPARE(snapshot.commonDepth, 4);
        const auto invalid = parseSearchInfo(
            tokens(QStringLiteral("info score cp 999 pv d2d4 currmove e2e5")), search, 1);
        QVERIFY(std::holds_alternative<Error>(invalid));
        QCOMPARE(snapshot.lines.front().score->value, -2);
        auto stats = parseSearchInfo(tokens(QStringLiteral("info depth 8 nodes 22")), search, 1);
        updateEvidence(snapshot, std::get<InfoUpdate>(stats));
        QCOMPARE(snapshot.lines.front().statistics.depth, 4);
        QCOMPARE(snapshot.statistics.depth, 8);
        const auto partial =
            parseSearchInfo(tokens(QStringLiteral("info score cp +12")), search, 1);
        updateEvidence(snapshot, std::get<InfoUpdate>(partial));
        QVERIFY(snapshot.lines.front().pv.empty());
        QCOMPARE(snapshot.achievedWidth, 0);
        QVERIFY(!snapshot.commonDepth);
        QVERIFY(std::holds_alternative<InfoUpdate>(parseSearchInfo(
            tokens(QStringLiteral("info string currmove broken depth -1")), search, 1)));
        QVERIFY(std::holds_alternative<Error>(parseSearchInfo({"info"}, search, 0)));
        const auto minimum = parseSearchInfo(
            tokens(QStringLiteral("info score cp -9223372036854775808 lowerbound")), search, 1);
        QCOMPARE(std::get<InfoUpdate>(minimum).line.score->value,
                 std::numeric_limits<std::int64_t>::min());
    }
    void restrictionsAndSpecialMoves() {
        auto search = request();
        search.searchMoves = std::vector{std::get<chess::Move>(search.position.parseUci("e2e4"))};
        for (const auto& text : {"info currmove d2d4", "info pv d2d4", "bestmove d2d4"}) {
            if (QString::fromLatin1(text).startsWith(u'i')) {
                QVERIFY(std::holds_alternative<Error>(parseSearchInfo(tokens(text), search, 1)));
            } else {
                QVERIFY(std::holds_alternative<Error>(parseBestMove(tokens(text), search)));
            }
        }
        QVERIFY(std::holds_alternative<InfoUpdate>(parseSearchInfo(
            tokens(QStringLiteral("info currmove e2e4 refutation d2d4 d7d5 currline d2d4 d7d5")),
            search, 1)));
        for (const auto& example : {std::pair{"r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1", "e1g1"},
                                    {"4k3/P7/8/8/8/8/8/4K3 w - - 0 1", "a7a8n"},
                                    {"4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 2", "e5d6"}}) {
            search = request();
            search.position = std::get<chess::Position>(chess::Position::fromFen(example.first));
            const auto before = search.position.context();
            const auto uci = QString::fromLatin1(example.second);
            for (const auto& field : {"pv", "refutation", "currline", "currmove"}) {
                const auto parsed = parseSearchInfo(
                    tokens(QStringLiteral("info %1 %2").arg(QString::fromLatin1(field), uci)),
                    search, 1);
                QVERIFY(std::holds_alternative<InfoUpdate>(parsed));
            }
            const auto repeated = parseSearchInfo(
                tokens(QStringLiteral("info refutation %1 currline %1 pv %1 currmove %1").arg(uci)),
                search, 1);
            QVERIFY(std::holds_alternative<InfoUpdate>(repeated));
            QCOMPARE(std::get<InfoUpdate>(repeated).line.pv,
                     std::vector{std::get<chess::Move>(search.position.parseUci(example.second))});
            QVERIFY(std::holds_alternative<BestMove>(
                parseBestMove(tokens(QStringLiteral("bestmove ") + uci), search)));
            QCOMPARE(search.position.context(), before);
        }
    }
    void continuationBounds() {
        const auto search = request();
        for (const auto& field : {"pv", "refutation", "currline"}) {
            const auto text = QStringLiteral("info %1 ").arg(QString::fromLatin1(field)) +
                              QStringLiteral("g1f3 g8f6 f3g1 f6g8 ").repeated(256);
            QVERIFY(std::holds_alternative<InfoUpdate>(parseSearchInfo(tokens(text), search, 1)));
            QVERIFY(std::holds_alternative<Error>(
                parseSearchInfo(tokens(text + QStringLiteral("g1f3")), search, 1)));
        }
    }
    void repeatedAuxiliaryLinesWithLongHistory() {
        auto search = request();
        for (int cycle = 0; cycle < 3000; ++cycle) {
            for (const auto* uci : {"g1f3", "g8f6", "f3g1", "f6g8"}) {
                const auto parsed = search.position.parseUci(uci);
                QVERIFY(std::holds_alternative<chess::Move>(parsed));
                QVERIFY(std::holds_alternative<std::monostate>(
                    search.position.play(std::get<chess::Move>(parsed))));
            }
        }
        const auto before = search.position.context();
        for (const auto& fields : {QStringLiteral("currline ").repeated(6000),
                                   QStringLiteral("refutation e2e4 ").repeated(3000)}) {
            const auto text = QStringLiteral("info ") + fields +
                              QStringLiteral("depth 4 score cp 12 pv d2d4 d7d5");
            QVERIFY(text.size() < 65536);
            const auto input = tokens(text);
            QElapsedTimer timer;
            timer.start();
            const auto parsed = parseSearchInfo(input, search, 1);
            const auto elapsed = timer.elapsed();
            QVERIFY(std::holds_alternative<InfoUpdate>(parsed));
            const auto& update = std::get<InfoUpdate>(parsed);
            QCOMPARE(update.line.score->value, 12);
            QCOMPARE(update.line.pv.size(), std::size_t(2));
            QCOMPARE(update.line.pv.front(),
                     std::get<chess::Move>(search.position.parseUci("d2d4")));
            QCOMPARE(search.position.context(), before);
            qInfo("Auxiliary replay: %lld bytes, %lld root plies, %lld ms",
                  static_cast<long long>(text.size()), static_cast<long long>(before.moves.size()),
                  static_cast<long long>(elapsed));
        }
    }
    void independentReplayAndGeneratedRecords() {
        std::mt19937 random(0x71c042U);
        for (int sample = 0; sample < 200; ++sample) {
            auto search = request();
            for (int ply = 0; ply < sample % 17; ++ply) {
                const auto legal = search.position.legalMoves();
                if (legal.empty()) {
                    break;
                }
                QVERIFY(std::holds_alternative<std::monostate>(
                    search.position.play(legal[random() % legal.size()])));
            }
            const auto before = search.position.context();
            auto position = search.position;
            std::vector<chess::Move> expected;
            QString continuation;
            for (int ply = 0; ply < 1 + sample % 9; ++ply) {
                const auto legal = position.legalMoves();
                if (legal.empty()) {
                    break;
                }
                const auto move = legal[random() % legal.size()];
                continuation +=
                    QString::fromStdString(std::get<std::string>(position.uci(move))) + u' ';
                expected.push_back(move);
                QVERIFY(std::holds_alternative<std::monostate>(position.play(move)));
            }
            if (expected.empty()) {
                continue;
            }
            const auto text =
                QStringLiteral("info refutation ") + continuation + QStringLiteral("currline 1 ") +
                continuation + QStringLiteral("pv ") + continuation + QStringLiteral("currline ") +
                continuation + QStringLiteral("refutation ") + continuation +
                QStringLiteral("depth 5 score cp -17 lowerbound wdl 1 998 1 nodes 123");
            const auto parsed = parseSearchInfo(tokens(text), search, 1);
            QVERIFY(std::holds_alternative<InfoUpdate>(parsed));
            const auto& line = std::get<InfoUpdate>(parsed).line;
            QCOMPARE(line.pv, expected);
            QCOMPARE(line.score->value, -17);
            QCOMPARE(line.score->bound, ScoreBound::Lower);
            QCOMPARE(line.wdl->draws, 998);
            QCOMPARE(line.statistics.nodes, 123);
            const auto invalid =
                text + QStringLiteral(" refutation ") + continuation + QStringLiteral("a0a1");
            QVERIFY(std::holds_alternative<Error>(parseSearchInfo(tokens(invalid), search, 1)));
            QCOMPARE(search.position.context(), before);
        }
    }
    void deterministicMalformedInput() {
        std::mt19937 random(0xe71d23U);
        const auto search = request();
        const auto before = search.position.context();
        const QStringList corpus{
            QStringLiteral("info depth 4 score cp -12 lowerbound pv e2e4 e7e5"),
            QStringLiteral("info currline 1 e2e4 refutation d2d4 d7d5 nodes 12"),
            QStringLiteral("info wdl 100 800 100 currmove e2e4 currmovenumber 1"),
            QStringLiteral("bestmove e2e4 ponder e7e5")};
        for (int sample = 0; sample < 4000; ++sample) {
            auto text = corpus[static_cast<qsizetype>(random() % 4)];
            const auto index =
                static_cast<qsizetype>(random() % static_cast<unsigned>(text.size()));
            switch (random() % 3) {
            case 0:
                text[index] = QChar(static_cast<char16_t>(random() % 256));
                break;
            case 1:
                text.truncate(index);
                break;
            default:
                text.insert(index, QStringLiteral(" pv a0a1 currline 0 score cp "));
                break;
            }
            const auto input = tokens(text);
            const auto info = parseSearchInfo(input, search, 1);
            if (const auto* update = std::get_if<InfoUpdate>(&info)) {
                QCOMPARE(update->line.rank, 1);
                QVERIFY(update->line.pv.size() <= 1024);
                auto position = search.position;
                for (const auto& move : update->line.pv) {
                    QVERIFY(std::holds_alternative<std::monostate>(position.play(move)));
                }
            } else {
                const auto& error = std::get<Error>(info);
                QCOMPARE(error.code, ErrorCode::Protocol);
                QCOMPARE(error.message, QStringLiteral("The engine returned invalid search "
                                                       "evidence. Reconnect a compatible engine."));
            }
            const auto best = parseBestMove(input, search);
            if (const auto* result = std::get_if<BestMove>(&best)) {
                QVERIFY(result->move);
                auto position = search.position;
                QVERIFY(std::holds_alternative<std::monostate>(position.play(*result->move)));
                if (result->ponder) {
                    QVERIFY(position.isLegal(*result->ponder));
                }
            }
            QCOMPARE(search.position.context(), before);
        }
    }
    void bestMoveGrammar() {
        auto search = request();
        const auto before = search.position.context();
        for (const auto& text :
             {"", "info e2e4", "bestmove", "bestmove e2e5", "bestmove 0000", "bestmove (none)",
              "bestmove e2e4 ponder", "bestmove e2e4 extra", "bestmove e2e4 ponder e2e3",
              "bestmove e2e4 ponder 0000", "bestmove e2e4 ponder e7e5 extra"}) {
            QVERIFY(std::holds_alternative<Error>(parseBestMove(tokens(text), search)));
        }
        const auto parsed =
            parseBestMove(tokens(QStringLiteral("bestmove e2e4 ponder e7e5")), search);
        QVERIFY(std::holds_alternative<BestMove>(parsed));
        QVERIFY(std::get<BestMove>(parsed).ponder);
        QCOMPARE(search.position.context(), before);
        for (const auto& fen :
             {"7k/6Q1/5K2/8/8/8/8/8 b - - 0 1", "7k/5K2/6Q1/8/8/8/8/8 b - - 0 1"}) {
            search.position = std::get<chess::Position>(chess::Position::fromFen(fen));
            for (const auto& null : {"0000", "(none)"}) {
                const auto text = QStringLiteral("bestmove ") + QString::fromLatin1(null);
                const auto result = parseBestMove(tokens(text), search);
                QVERIFY(std::holds_alternative<BestMove>(result));
                QVERIFY(!std::get<BestMove>(result).move);
                QVERIFY(std::holds_alternative<Error>(
                    parseBestMove(tokens(text + QStringLiteral(" ponder h8h7")), search)));
            }
        }
    }
};
int main(int argc, char* argv[]) {
    const TestEnvironment environment;
    if (!environment.isValid()) {
        return 1;
    }
    QCoreApplication application(argc, argv);
    SearchProtocolTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "SearchProtocolTest.moc"
