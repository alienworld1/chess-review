#pragma once
#include "engine/Analysis.h"
#include <QByteArray>
#include <QStringList>

namespace chess_review::engine {
struct PreparedSearch {
    QByteArray options;
    QByteArray position;
    QByteArray go;
    int effectiveMultiPv = 1;
    bool wdlSupported = false;
};
std::variant<PreparedSearch, Error> prepareSearch(const AnalysisRequest& request,
                                                  const Capabilities& capabilities);
struct InfoUpdate {
    AnalysisLine line;
    bool hasEvidence = false;
};
std::variant<InfoUpdate, Error> parseSearchInfo(const QStringList& tokens,
                                                const AnalysisRequest& request, int width);
struct BestMove {
    std::optional<chess::Move> move;
    std::optional<chess::Move> ponder;
};
std::variant<BestMove, Error> parseBestMove(const QStringList& tokens,
                                            const AnalysisRequest& request);
void mergeStatistics(SearchStatistics& target, const SearchStatistics& update);
void updateEvidence(AnalysisSnapshot& snapshot, InfoUpdate update);
} // namespace chess_review::engine
