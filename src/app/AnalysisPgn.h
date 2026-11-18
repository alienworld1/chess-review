#pragma once

#include "app/BoardState.h"
#include "pgn/Pgn.h"
#include <map>

namespace chess_review::app {
using AnalysisDrawings = std::map<chess::NodeId, std::vector<BoardAnnotation>>;
QString analysisComment(std::string_view comment, std::vector<BoardAnnotation>* drawings = nullptr);
std::string analysisCommentWithDrawings(QString comment,
                                        const std::vector<BoardAnnotation>& drawings);
QString writeAnalysisPgn(const QString& filename, const chess::GameTree& tree,
                         const pgn::PgnDocument* source, const AnalysisDrawings& drawings);
} // namespace chess_review::app
