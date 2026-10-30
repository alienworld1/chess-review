#pragma once
#include "persistence/ReviewRepository.h"
namespace chess_review::app {
enum class ReviewPhase {
    Idle,
    Loading,
    Connecting,
    Preparing,
    LookingUp,
    Scanning,
    Refining,
    Saving,
    Finished
};
struct ReviewGameState {
    persistence::GameId gameId = 0;
    QString game;
    QString engine;
    ReviewPhase phase = ReviewPhase::Idle;
    QString message;
    QString error;
    QString notice;
    QString compatibilityNotice;
    std::optional<persistence::ReviewSummary> run;
    std::vector<persistence::ReviewSummary> runs;
    std::optional<persistence::ReviewOverview> overview;
    QString overviewError;
    int refinementDone = 0;
    int refinementTotal = 0;
    std::int64_t page = 0;
    bool hasNext = false;
    bool listing = false;
    bool active = false;
    bool busy = false;
    bool inspecting = false;
    bool ready = false;
    bool cancelling = false;
};
} // namespace chess_review::app
