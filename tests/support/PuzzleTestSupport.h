#pragma once
#include "engine/EffectiveSearch.h"
#include "engine/private/Fingerprint.h"
#include "persistence/PuzzleRepository.h"
#include <algorithm>

namespace puzzle_test {
using namespace chess_review;
inline engine::ValidatedEngine engine() {
    engine::ValidatedEngine e;
    e.executablePath = QStringLiteral("/missing/puzzle-test-engine");
    e.executableSha256 = QByteArray(32, 'p');
    e.reportedName = QStringLiteral("Puzzle test engine");
    e.lastValidatedAt = QDateTime::currentDateTimeUtc();
    return e;
}
inline engine::EngineFingerprint fingerprint() {
    return engine::makeFingerprint(
        engine(),
        {{QStringLiteral("multipv"), engine::OptionType::Spin, QStringLiteral("1")},
         {QStringLiteral("uci_showwdl"), engine::OptionType::Check, QStringLiteral("true")}});
}
inline chess::Position root() {
    return std::get<chess::Position>(
        chess::Position::fromFen("6k1/5ppp/8/8/8/8/5PPP/3R2K1 w - - 0 1"));
}
inline persistence::PuzzleDocument seed(std::int64_t profile = 1) {
    persistence::PuzzleDocument d;
    d.definition.position = root().context();
    d.definition.policy.baselineNodes = 10;
    d.fingerprint = fingerprint();
    d.engineProfileId = profile;
    return d;
}
inline engine::AnalysisResult pass(const persistence::PuzzleDocument& document,
                                   std::uint64_t nodes) {
    const auto position = root();
    engine::AnalysisResult r;
    r.outcome = engine::AnalysisOutcome::Completed;
    auto& s = r.evidence;
    s.session = QUuid::createUuid();
    s.request = QUuid::createUuid();
    s.limit = engine::NodeLimit{nodes};
    s.requestedMultiPv = s.effectiveMultiPv = s.achievedWidth = 4;
    s.wdlRequested = s.wdlSupported = s.searchStarted = true;
    s.commonDepth = 12;
    s.statistics.depth = 12;
    s.statistics.nodes = nodes;
    const auto best = std::get<chess::Move>(position.parseUci("d1d8"));
    std::vector<chess::Move> roots{best};
    for (const auto m : position.legalMoves()) {
        if (roots.size() == 4) {
            break;
        }
        if (m != best) {
            roots.push_back(m);
        }
    }
    int rank = 0;
    for (const auto move : roots) {
        engine::AnalysisLine line;
        line.rank = ++rank;
        line.score = engine::AnalysisScore{rank == 1 ? engine::ScoreType::Mate
                                                     : engine::ScoreType::Centipawns,
                                           rank == 1 ? 1 : 0, engine::ScoreBound::Exact};
        line.wdl = rank == 1 ? engine::Wdl{1000, 0, 0} : engine::Wdl{500, 0, 500};
        line.statistics = s.statistics;
        line.pv = {move};
        s.lines.push_back(line);
    }
    s.bestMove = best;
    s.provenance = std::make_shared<const engine::AnalysisProvenance>(
        engine::AnalysisProvenance{position.context(),
                                   engine::reviewWidthFingerprint(document.fingerprint, 4),
                                   {},
                                   QDateTime::currentDateTimeUtc()});
    return r;
}
inline persistence::PuzzleDocument complete(std::int64_t profile = 1) {
    auto d = seed(profile);
    d.passes = {pass(d, 20), pass(d, 40)};
    const auto result = persistence::verifyPuzzle(d);
    const auto* verified = std::get_if<puzzles::Verification>(&result);
    if (!verified || verified->outcome != puzzles::Outcome::Accepted) {
        qFatal("Invalid puzzle test evidence");
    }
    d.solution = verified->solution;
    return d;
}
inline engine::AnalysisResult normalPass(const persistence::PuzzleDocument& document,
                                         const chess::Position& position, std::uint64_t nodes,
                                         const char* bestUci, int wins,
                                         std::optional<chess::Move> excluded = std::nullopt) {
    auto r = pass(document, nodes);
    auto& s = r.evidence;
    s.lines.clear();
    const auto best = std::get<chess::Move>(position.parseUci(bestUci));
    std::vector<chess::Move> roots{best};
    const auto width = std::min<std::size_t>(4, position.legalMoves().size());
    for (const auto move : position.legalMoves()) {
        if (roots.size() == width) {
            break;
        }
        if (move != best && move != excluded) {
            roots.push_back(move);
        }
    }
    int rank = 0;
    for (const auto move : roots) {
        engine::AnalysisLine line;
        line.rank = ++rank;
        line.score =
            engine::AnalysisScore{engine::ScoreType::Centipawns, 0, engine::ScoreBound::Exact};
        const int value = rank == 1 ? wins : std::max(0, wins - 200);
        line.wdl = engine::Wdl{value, 0, 1000 - value};
        line.statistics = s.statistics;
        line.pv = {move};
        s.lines.push_back(line);
    }
    s.requestedMultiPv = s.effectiveMultiPv = s.achievedWidth = static_cast<int>(width);
    s.bestMove = best;
    s.provenance = std::make_shared<const engine::AnalysisProvenance>(engine::AnalysisProvenance{
        position.context(),
        engine::reviewWidthFingerprint(document.fingerprint, static_cast<int>(width)),
        {},
        QDateTime::currentDateTimeUtc()});
    return r;
}
inline persistence::PuzzleDocument
normalPuzzle(const engine::EngineFingerprint& engineFingerprint) {
    auto d = seed();
    d.definition.position = chess::Position::initial().context();
    d.fingerprint = engineFingerprint;
    auto position = chess::Position::initial();
    const auto excluded = std::get<chess::Move>(position.parseUci("d2d4"));
    for (int step = 0; step < 3; ++step) {
        const char* uci = step == 0 ? "e2e4" : step == 1 ? "e7e5" : "g1f3";
        for (const auto nodes : {20ULL, 40ULL}) {
            d.passes.push_back(normalPass(d, position, nodes, uci, step == 1 ? 400 : 600,
                                          step == 0 ? std::optional{excluded} : std::nullopt));
        }
        if (step < 2 && std::holds_alternative<chess::Error>(
                            position.play(std::get<chess::Move>(position.parseUci(uci))))) {
            qFatal("Invalid normal puzzle fixture");
        }
    }
    const auto result = persistence::verifyPuzzle(d);
    const auto* verified = std::get_if<puzzles::Verification>(&result);
    if (!verified || verified->outcome != puzzles::Outcome::Accepted) {
        qFatal("Invalid normal puzzle evidence");
    }
    d.solution = verified->solution;
    return d;
}
} // namespace puzzle_test
