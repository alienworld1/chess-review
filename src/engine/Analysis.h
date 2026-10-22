#pragma once

#include "chess/Position.h"
#include "engine/EngineProfile.h"
#include <QMetaType>
#include <QUuid>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace chess_review::engine {
struct NodeLimit {
    std::uint64_t nodes = 0;
};
struct DepthLimit {
    std::uint32_t depth = 0;
};
struct TimeLimit {
    std::uint64_t milliseconds = 0;
};
struct InfiniteLimit {};
using SearchLimit = std::variant<NodeLimit, DepthLimit, TimeLimit, InfiniteLimit>;
struct AnalysisRequest {
    QUuid id;
    chess::Position position = chess::Position::initial();
    SearchLimit limit = NodeLimit{10000};
    int multiPv = 1;
    bool wantWdl = false;
    // nullopt means unrestricted; an explicitly empty restriction is invalid.
    std::optional<std::vector<chess::Move>> searchMoves;
};
enum class ScoreType { Centipawns, Mate };
enum class ScoreBound { Exact, Lower, Upper };
// Raw UCI evidence, always from the root side-to-move perspective, including WDL.
struct AnalysisScore {
    ScoreType type = ScoreType::Centipawns;
    std::int64_t value = 0;
    ScoreBound bound = ScoreBound::Exact;
};
struct Wdl {
    int wins = 0;
    int draws = 0;
    int losses = 0;
};
struct SearchStatistics {
    std::optional<int> depth;
    std::optional<int> selectiveDepth;
    std::optional<std::uint64_t> nodes;
    std::optional<std::uint64_t> timeMs;
    std::optional<std::uint64_t> nps;
    std::optional<std::uint64_t> tablebaseHits;
    std::optional<int> hashFull;
};
struct AnalysisLine {
    int rank = 1;
    std::optional<AnalysisScore> score;
    std::optional<Wdl> wdl;
    std::vector<chess::Move> pv;
    SearchStatistics statistics;
};
struct AnalysisSnapshot {
    QUuid session;
    QUuid request;
    SearchLimit limit = InfiniteLimit{};
    int requestedMultiPv = 1;
    int effectiveMultiPv = 1;
    bool wdlRequested = false;
    bool wdlSupported = false;
    std::vector<AnalysisLine> lines;
    SearchStatistics statistics;
    int achievedWidth = 0;          // Contiguous score/PV ranks with distinct root moves.
    std::optional<int> commonDepth; // Present only for a full effective-width set.
    std::optional<chess::Move> bestMove;
    std::optional<chess::Move> ponder;
};
enum class SearchPhase { Configuring, Analyzing, Stopping, Synchronizing };
struct SearchState {
    QUuid session;
    QUuid request;
    SearchPhase phase = SearchPhase::Configuring;
};
enum class AnalysisOutcome { Rejected, Completed, Stopped, Failed };
struct AnalysisResult {
    AnalysisSnapshot evidence;
    AnalysisOutcome outcome = AnalysisOutcome::Rejected;
    std::optional<Error> error;
};
} // namespace chess_review::engine
Q_DECLARE_METATYPE(chess_review::engine::AnalysisSnapshot)
Q_DECLARE_METATYPE(chess_review::engine::SearchState)
Q_DECLARE_METATYPE(chess_review::engine::AnalysisResult)
