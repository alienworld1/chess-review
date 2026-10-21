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
    ParseState(std::string input, std::size_t games) : bytes(std::move(input)) {
        limits.maxGames = games;
    }
    pgn::PgnLimits limits;
    std::string bytes;
    std::stop_source stop;
    std::unique_ptr<pgn::PgnReader> reader; // Used only by the single parser worker.
};
GameImportService::GameImportService(Database& database, QObject* parent)
    : QObject(parent), database_(database) {
    pool_.setMaxThreadCount(1);
    connect(&database_, &Database::gameInserted, this,
            [this](RequestId request, const InsertGameResult& result) {
                if (!active_ || request != storageRequest_ || request.isNull()) {
                    return;
                }
                storageRequest_ = {};
                ImportOutcome outcome =
                    std::visit([](const auto& value) -> ImportOutcome { return value; }, result);
                report_.items.push_back({ordinal_, std::move(outcome), range_, std::move(warnings_),
                                         suppressedWarnings_});
                emit progress(report_.request, report_.items.back());
                if (std::holds_alternative<GameError>(result)) {
                    finish(ImportStatus::Failed);
                } else {
                    next();
                }
            });
}
GameImportService::~GameImportService() {
    Q_ASSERT_X(!active_ && pool_.activeThreadCount() == 0, "GameImportService",
               "Await closed() before destruction");
}
void GameImportService::importText(RequestId request, std::string bytes, GameSource source,
                                   ImportBudget budget) {
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
    active_ = true;
    report_ = {request, ImportStatus::Completed, {}};
    source_ = std::move(source);
    budget_ = budget;
    parse_ = std::make_shared<ParseState>(std::move(bytes), budget.games);
    next();
}
void GameImportService::next() {
    if (parse_->stop.stop_requested()) {
        finish(ImportStatus::Cancelled);
        return;
    }
    pool_.start([this, state = parse_] {
        try {
            if (!state->reader) {
                state->reader = std::make_unique<pgn::PgnReader>(state->bytes, state->limits,
                                                                 state->stop.get_token());
            }
            auto outcome = state->reader->next();
            QMetaObject::invokeMethod(
                this,
                [this, outcome = std::move(outcome)]() mutable { parsed(std::move(outcome)); },
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
void GameImportService::parsed(pgn::PgnOutcome outcome) {
    if (auto* document = std::get_if<pgn::PgnDocument>(&outcome)) {
        ordinal_ = document->ordinal;
        report_.attemptedGames = ordinal_;
        range_ = document->range;
        const auto retained =
            std::min(document->warnings.size(), budget_.warnings - retainedWarnings_);
        suppressedWarnings_ = document->suppressedWarnings + document->warnings.size() - retained;
        // Fresh storage bounds capacity too, including the zero-retained case.
        warnings_ = std::vector<pgn::Diagnostic>(document->warnings.begin(),
                                                 document->warnings.begin() +
                                                     static_cast<std::ptrdiff_t>(retained));
        retainedWarnings_ += retained;
        if (parse_->stop.stop_requested()) {
            finish(ImportStatus::Cancelled);
            return;
        }
        storageRequest_ = QUuid::createUuid();
        database_.insertGame(storageRequest_,
                             std::make_shared<const pgn::PgnDocument>(std::move(*document)),
                             source_);
        emit savingGame(report_.request, ordinal_);
    } else if (const auto* failure = std::get_if<pgn::PgnFailure>(&outcome)) {
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
