#include "app/GameImportService.h"
#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <exception>
#include <utility>

namespace chess_review::app {
using namespace persistence;
struct GameImportService::ParseState {
    ParseState(std::string input, std::size_t games, ImportMode importMode)
        : bytes(std::move(input)), mode(importMode) {
        limits.maxGames = mode == ImportMode::OneCompletedGame ? 1 : games;
        limits.requireTermination = mode == ImportMode::OneCompletedGame;
    }
    pgn::PgnOutcome read() {
        if (!reader) {
            reader = std::make_unique<pgn::PgnReader>(bytes, limits, stop.get_token());
        }
        auto outcome = reader->next();
        if (mode == ImportMode::Batch) {
            return outcome;
        }
        if (auto* game = std::get_if<pgn::PgnDocument>(&outcome)) {
            auto tail = reader->next();
            if (std::holds_alternative<pgn::Cancelled>(tail)) {
                return tail;
            }
            if (game->result != pgn::GameResult::Unknown &&
                std::holds_alternative<pgn::EndOfInput>(tail)) {
                return outcome;
            }
        } else if (auto* failure = std::get_if<pgn::PgnFailure>(&outcome)) {
            failure->unreadableTail = true;
            return outcome;
        } else if (std::holds_alternative<pgn::Cancelled>(outcome)) {
            return outcome;
        }
        return pgn::PgnFailure{1,
                               {0, bytes.size()},
                               {pgn::DiagnosticCode::InvalidSyntax,
                                "The download must contain one completed game.",
                                {},
                                std::nullopt},
                               true};
    }
    pgn::PgnLimits limits;
    std::string bytes;
    ImportMode mode;
    std::stop_source stop;
    std::unique_ptr<pgn::PgnReader> reader; // Used only by the single parser worker.
};
GameImportService::GameImportService(Database& database, QObject* parent)
    : QObject(parent), database_(database) {
    pool_.setMaxThreadCount(1);
    connect(&database_, &Database::gameInserted, this,
            [this](RequestId request, const InsertGameResult& result) {
                if (!active_ || request != storageRequest_ || request.isNull() ||
                    pendingItems_.size() != 1) {
                    return;
                }
                storageRequest_ = {};
                const auto meta = std::move(pendingItems_.front());
                pendingItems_.clear();
                handleInsertOutcome(meta, result);
                if (std::holds_alternative<GameError>(result)) {
                    finish(ImportStatus::Failed);
                } else {
                    advanceAfterInsert();
                }
            });
    connect(&database_, &Database::gamesInserted, this,
            [this](RequestId request, const std::vector<InsertGameResult>& results) {
                if (!active_ || request != storageRequest_ || request.isNull()) {
                    return;
                }
                storageRequest_ = {};
                const auto metas = std::exchange(pendingItems_, {});
                const auto count = std::min(results.size(), metas.size());
                for (std::size_t i = 0; i < count; ++i) {
                    handleInsertOutcome(metas[i], results[i]);
                    if (std::holds_alternative<GameError>(results[i])) {
                        finish(ImportStatus::Failed);
                        return;
                    }
                }
                if (results.size() != metas.size()) {
                    finish(ImportStatus::Failed);
                    return;
                }
                advanceAfterInsert();
            });
}
GameImportService::~GameImportService() {
    Q_ASSERT_X(!active_ && pool_.activeThreadCount() == 0, "GameImportService",
               "Await closed() before destruction");
}
void GameImportService::importText(RequestId request, std::string bytes, GameSource source,
                                   ImportBudget budget, ImportMode mode) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (closing_ || active_) {
        const auto error = GameError{closing_ ? GameErrorCode::Closing : GameErrorCode::Busy,
                                     QStringLiteral("Game import is unavailable.")};
        QTimer::singleShot(0, this, [this, request, error] { emit rejected(request, error); });
        return;
    }
    if (bytes.size() > pgn::PgnLimits{}.maxInputBytes || budget.games == 0 ||
        budget.games > pgn::PgnLimits{}.maxGames || budget.warnings > maxImportReportWarnings) {
        QTimer::singleShot(0, this, [this, request] {
            emit rejected(request, {GameErrorCode::InvalidSnapshot,
                                    QStringLiteral("PGN input exceeds the import limit.")});
        });
        return;
    }
    ordinal_ = 0;
    retainedWarnings_ = 0;
    firstGamePending_ = true;
    pendingItems_.clear();
    trailingOutcome_.reset();
    active_ = true;
    report_ = {request, ImportStatus::Completed, {}};
    source_ = std::move(source);
    budget_ = budget;
    parse_ = std::make_shared<ParseState>(std::move(bytes), budget.games, mode);
    next();
}
void GameImportService::next() {
    if (parse_->stop.stop_requested()) {
        finish(ImportStatus::Cancelled);
        return;
    }
    const std::size_t maxChunk =
        (parse_->mode == ImportMode::OneCompletedGame || firstGamePending_) ? 1 : 32;
    firstGamePending_ = false;
    pool_.start([this, state = parse_, maxChunk] {
        try {
            std::vector<pgn::PgnDocument> games;
            std::optional<pgn::PgnOutcome> trailing;
            games.reserve(maxChunk);
            std::size_t chunkBytes = 0;
            constexpr std::size_t maxChunkBytes = 4 * 1024 * 1024;
            for (std::size_t i = 0; i < maxChunk; ++i) {
                if (state->stop.stop_requested()) {
                    trailing = pgn::Cancelled{};
                    break;
                }
                auto outcome = state->read();
                if (auto* doc = std::get_if<pgn::PgnDocument>(&outcome)) {
                    chunkBytes += doc->originalText.size();
                    games.push_back(std::move(*doc));
                    if (state->mode == ImportMode::OneCompletedGame ||
                        chunkBytes >= maxChunkBytes) {
                        break;
                    }
                } else {
                    trailing = std::move(outcome);
                    break;
                }
            }
            QMetaObject::invokeMethod(
                this,
                [this, games = std::move(games), trailing = std::move(trailing)]() mutable {
                    parsedChunk(std::move(games), std::move(trailing));
                },
                Qt::QueuedConnection);
        } catch (const std::exception&) {
            QMetaObject::invokeMethod(
                this,
                [this] {
                    report_.items.push_back(
                        {ordinal_ + 1,
                         GameError{GameErrorCode::Storage,
                                   QStringLiteral("PGN import could not complete.")}});
                    finish(ImportStatus::Failed);
                },
                Qt::QueuedConnection);
        }
    });
}
void GameImportService::parsedChunk(std::vector<pgn::PgnDocument> games,
                                    std::optional<pgn::PgnOutcome> trailing) {
    if (parse_->stop.stop_requested()) {
        finish(ImportStatus::Cancelled);
        return;
    }
    if (games.empty()) {
        if (trailing) {
            handleTrailing(std::move(*trailing));
        } else {
            finish(ImportStatus::Completed);
        }
        return;
    }
    const auto playerName = [](const std::string& value) {
        auto text =
            QString::fromUtf8(value.data(),
                              static_cast<qsizetype>(std::min<std::size_t>(value.size(), 640)))
                .left(160);
        if (!text.isEmpty() && text.back().isHighSurrogate()) {
            text.chop(1);
        }
        return text.simplified();
    };
    pendingItems_.clear();
    pendingItems_.reserve(games.size());
    std::vector<std::shared_ptr<const pgn::PgnDocument>> documents;
    documents.reserve(games.size());
    for (auto& document : games) {
        QString white = tr("White");
        QString black = tr("Black");
        for (const auto& tag : document.tags) {
            if (tag.name == "White") {
                white = playerName(tag.value);
            } else if (tag.name == "Black") {
                black = playerName(tag.value);
            }
        }
        ordinal_ = document.ordinal;
        report_.attemptedGames = ordinal_;
        const auto retained =
            std::min(document.warnings.size(), budget_.warnings - retainedWarnings_);
        const auto suppressed = document.suppressedWarnings + document.warnings.size() - retained;
        std::vector<pgn::Diagnostic> warnings(document.warnings.begin(),
                                              document.warnings.begin() +
                                                  static_cast<std::ptrdiff_t>(retained));
        retainedWarnings_ += retained;
        pendingItems_.push_back({ordinal_, document.range, std::move(warnings), suppressed,
                                 tr("%1 vs %2").arg(white, black)});
        documents.push_back(std::make_shared<const pgn::PgnDocument>(std::move(document)));
    }
    trailingOutcome_ = std::move(trailing);
    storageRequest_ = QUuid::createUuid();
    const auto firstOrdinal = pendingItems_.front().ordinal;
    if (documents.size() == 1) {
        database_.insertGame(storageRequest_, std::move(documents.front()), source_);
    } else {
        database_.insertGameBatch(storageRequest_, std::move(documents), source_);
    }
    emit savingGame(report_.request, firstOrdinal);
}
void GameImportService::handleTrailing(pgn::PgnOutcome outcome) {
    if (const auto* failure = std::get_if<pgn::PgnFailure>(&outcome)) {
        ordinal_ = failure->ordinal;
        report_.attemptedGames = std::min(ordinal_, budget_.games);
        report_.gameLimitReached = ordinal_ > budget_.games;
        report_.items.push_back({ordinal_, *failure, failure->range});
        emit progress(report_.request, report_.items.back());
        if (failure->unreadableTail) {
            finish(ImportStatus::Failed);
        } else {
            next();
        }
    } else if (std::holds_alternative<pgn::Cancelled>(outcome)) {
        finish(ImportStatus::Cancelled);
    } else {
        finish(parse_->stop.stop_requested() ? ImportStatus::Cancelled : ImportStatus::Completed);
    }
}
void GameImportService::handleInsertOutcome(const PendingBatchItem& meta,
                                            const InsertGameResult& result) {
    ImportOutcome outcome =
        std::visit([](const auto& value) -> ImportOutcome { return value; }, result);
    report_.items.push_back({meta.ordinal, std::move(outcome), meta.range, meta.warnings,
                             meta.suppressedWarnings, meta.title});
    emit progress(report_.request, report_.items.back());
}
void GameImportService::advanceAfterInsert() {
    if (parse_->stop.stop_requested()) {
        finish(ImportStatus::Cancelled);
        return;
    }
    if (trailingOutcome_) {
        auto trailing = std::exchange(trailingOutcome_, std::nullopt);
        handleTrailing(std::move(*trailing));
        return;
    }
    if (parse_->mode == ImportMode::OneCompletedGame) {
        finish(ImportStatus::Completed);
    } else {
        next();
    }
}
void GameImportService::cancel() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (parse_) {
        parse_->stop.request_stop();
    }
}
void GameImportService::finish(ImportStatus status) {
    report_.status = status;
    active_ = false;
    parse_.reset();
    // A value snapshot permits slots to start another import without invalidating this report.
    const auto report = std::move(report_);
    emit finished(report);
    if (closing_) {
        finishClose();
    }
}
void GameImportService::close() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (closing_) {
        return;
    }
    closing_ = true;
    cancel();
    if (!active_) {
        finishClose();
    }
}
void GameImportService::finishClose() {
    if (closed_ || active_) {
        return;
    }
    // The posted parse result can precede worker return. Never join on the GUI thread.
    if (!pool_.waitForDone(0)) {
        QTimer::singleShot(5, this, &GameImportService::finishClose);
        return;
    }
    closed_ = true;
    QTimer::singleShot(0, this, &GameImportService::closed);
}
} // namespace chess_review::app
