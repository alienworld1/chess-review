#include "app/ImportController.h"
#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <exception>
#include <utility>

namespace chess_review::app {
ImportController::ImportController(GameImportService& service, QObject* parent)
    : QObject(parent), service_(service) {
    pool_.setMaxThreadCount(1);
    connect(&service_, &GameImportService::savingGame, this,
            [this](persistence::RequestId request, std::size_t ordinal) {
                if (request_.isNull() || request != request_) {
                    return;
                }
                currentGame_ = ordinal;
                emit changed();
            });
    connect(&service_, &GameImportService::progress, this,
            [this](persistence::RequestId request, const ImportItem& item) {
                if (request_.isNull() || request != request_) {
                    return;
                }
                currentGame_ = item.ordinal;
                results_[index_].items.push_back(item);
                retainedWarnings_ += item.warnings.size();
                warnings_ += item.warnings.size() + item.suppressedWarnings;
                const bool inserted = std::holds_alternative<persistence::Inserted>(item.outcome);
                if (inserted) {
                    ++inserted_;
                } else if (std::holds_alternative<persistence::Duplicate>(item.outcome)) {
                    ++duplicates_;
                } else {
                    ++failures_;
                }
                emit changed();
                if (inserted) {
                    emit committed();
                }
            });
    connect(&service_, &GameImportService::finished, this, &ImportController::completeFile);
    connect(&service_, &GameImportService::rejected, this,
            [this](persistence::RequestId request, const persistence::GameError& error) {
                if (request_.isNull() || request != request_) {
                    return;
                }
                request_ = {};
                results_[index_].state = ImportFileState::Failed;
                results_[index_].message = error.message;
                ++failures_;
                finish(ImportBatchStatus::Failed);
            });
    connect(&service_, &GameImportService::closed, this, &ImportController::closed);
}
ImportController::~ImportController() { Q_ASSERT(!busy_ && pool_.activeThreadCount() == 0); }
void ImportController::setReady(bool ready) {
    ready_ = ready;
    emit changed();
}
bool ImportController::begin() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!available()) {
        emit rejected(tr("Import is unavailable while storage is loading, closing, or another "
                         "import is running."));
        return false;
    }
    stop_ = std::stop_source{};
    results_.clear();
    index_ = currentGame_ = bytesRead_ = attempted_ = retainedWarnings_ = 0;
    inserted_ = duplicates_ = failures_ = warnings_ = 0;
    busy_ = true;
    status_ = ImportBatchStatus::Running;
    return true;
}
bool ImportController::acceptsFiles(const QList<QUrl>& files) {
    return !files.isEmpty() && static_cast<std::size_t>(files.size()) <= maxBatchFiles &&
           std::all_of(files.begin(), files.end(), input::isLocalPgnUrl);
}
bool ImportController::importFiles(const QList<QUrl>& files) {
    if (files.isEmpty() || static_cast<std::size_t>(files.size()) > maxBatchFiles) {
        emit rejected(tr("Choose between 1 and 32 PGN files."));
        return false;
    }
    if (!acceptsFiles(files)) {
        emit rejected(tr("Only absolute local .pgn files without remote hosts can be imported."));
        return false;
    }
    if (!begin()) {
        return false;
    }
    urls_ = files;
    text_.clear();
    for (const auto& url : urls_) {
        results_.push_back({url.fileName(), ImportFileState::Pending, {}, {}});
    }
    emit submitted();
    next();
    return true;
}
bool ImportController::importText(QString text) {
    if (static_cast<quint64>(text.size()) > maxBatchBytes) {
        emit rejected(tr("Pasted text exceeds the import limit."));
        return false;
    }
    if (!begin()) {
        return false;
    }
    urls_.clear();
    text_ = std::move(text);
    results_.push_back({tr("Pasted PGN"), ImportFileState::Pending, {}, {}});
    emit submitted();
    next();
    return true;
}
void ImportController::next() {
    if (stop_.stop_requested()) {
        finish(ImportBatchStatus::Cancelled);
        return;
    }
    if (index_ == results_.size()) {
        finish(ImportBatchStatus::Completed);
        return;
    }
    if (attempted_ >= maxBatchGames) {
        finish(ImportBatchStatus::GameLimitReached);
        return;
    }
    results_[index_].state = ImportFileState::Reading;
    currentGame_ = 0;
    reading_ = true;
    const auto token = stop_.get_token();
    const auto remaining = maxBatchBytes - bytesRead_;
    const auto url = urls_.isEmpty() ? QUrl{} : urls_.at(static_cast<qsizetype>(index_));
    auto text = std::exchange(text_, {});
    const bool paste = urls_.isEmpty();
    pool_.start([this, url, text = std::move(text), remaining, token, paste] {
        input::InputResult result;
        try {
            result = paste ? input::encodePgnText(text, remaining, token)
                           : input::readLocalPgn(url, remaining, token);
        } catch (const std::exception&) {
            result.error = QStringLiteral("PGN input could not be acquired.");
            // The failed acquisition cannot report its partial read count. Spend its entire
            // allowance conservatively so later files cannot exceed the batch byte budget.
            result.bytesRead = remaining;
        }
        QMetaObject::invokeMethod(
            this, [this, result = std::move(result)]() mutable { acquired(std::move(result)); },
            Qt::QueuedConnection);
    });
    emit changed();
}
void ImportController::acquired(input::InputResult result) {
    reading_ = false;
    bytesRead_ += result.bytesRead;
    if (stop_.stop_requested() || result.cancelled) {
        results_[index_].state = ImportFileState::Cancelled;
        finish(ImportBatchStatus::Cancelled);
        return;
    }
    if (!result.error.isEmpty()) {
        results_[index_].state = ImportFileState::Failed;
        results_[index_].message = result.error;
        ++failures_;
        ++index_;
        next();
        return;
    }
    results_[index_].state = ImportFileState::Parsing;
    request_ = QUuid::createUuid();
    service_.importText(request_, std::move(result.bytes), std::move(result.source),
                        {maxBatchGames - attempted_, maxImportReportWarnings - retainedWarnings_});
    emit changed();
}
void ImportController::completeFile(const ImportReport& report) {
    if (request_.isNull() || report.request != request_) {
        return;
    }
    request_ = {};
    auto& file = results_[index_];
    attempted_ += report.attemptedGames;
    // Worker exceptions are terminal and may have no progress notification.
    for (std::size_t i = file.items.size(); i < report.items.size(); ++i) {
        file.items.push_back(report.items[i]);
        ++failures_;
    }
    file.state = report.status == ImportStatus::Cancelled ? ImportFileState::Cancelled
                 : report.status == ImportStatus::Failed  ? ImportFileState::Failed
                                                          : ImportFileState::Completed;
    if (report.items.empty() && report.status == ImportStatus::Completed) {
        file.message = tr("No games found.");
    }
    bool storageFailure = false;
    bool parseFailure = false;
    for (const auto& item : file.items) {
        storageFailure |= std::holds_alternative<persistence::GameError>(item.outcome);
        parseFailure |= std::holds_alternative<pgn::PgnFailure>(item.outcome);
    }
    if (parseFailure && file.state == ImportFileState::Completed) {
        file.state = ImportFileState::CompletedWithErrors;
    }
    ++index_;
    if (storageFailure) {
        finish(ImportBatchStatus::Failed);
    } else if (report.gameLimitReached) {
        finish(ImportBatchStatus::GameLimitReached);
    } else if (report.status == ImportStatus::Cancelled) {
        finish(ImportBatchStatus::Cancelled);
    } else {
        next();
    }
}
void ImportController::finish(ImportBatchStatus status) {
    status_ = status;
    finishing_ = true;
    for (auto& file : results_) {
        if (file.state == ImportFileState::Pending) {
            file.state = ImportFileState::Unprocessed;
            file.message = status == ImportBatchStatus::Cancelled
                               ? tr("Not processed: import cancelled.")
                           : status == ImportBatchStatus::GameLimitReached
                               ? tr("Not processed: batch game limit reached.")
                               : tr("Not processed: import stopped after a failure.");
        }
    }
    busy_ = false;
    urls_.clear();
    text_.clear();
    // Do not admit a replacement batch halfway through the terminal state notification.
    emit changed();
    emit finished();
    finishing_ = false;
    // Terminal observers have finished reading the report; admission can resume now.
    emit changed();
    if (closing_) {
        finishClose();
    }
}
void ImportController::cancel() {
    if (!busy_) {
        return;
    }
    stop_.request_stop();
    service_.cancel();
    emit changed();
}
void ImportController::close() {
    if (closing_) {
        return;
    }
    closing_ = true;
    cancel();
    emit changed();
    if (!busy_) {
        finishClose();
    }
}
void ImportController::finishClose() {
    if (reading_ || busy_ || finishing_ || closingService_) {
        return;
    }
    if (!pool_.waitForDone(0)) {
        QTimer::singleShot(5, this, &ImportController::finishClose);
        return;
    }
    closingService_ = true;
    service_.close();
}
} // namespace chess_review::app
