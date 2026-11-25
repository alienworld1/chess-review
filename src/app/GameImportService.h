#pragma once
#include "persistence/Database.h"
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

namespace chess_review::app {
inline constexpr std::size_t maxImportReportWarnings = 1000;
enum class ImportStatus { Completed, Cancelled, Failed };
enum class ImportMode { Batch, OneCompletedGame };
using ImportOutcome = std::variant<persistence::Inserted, persistence::Duplicate, pgn::PgnFailure,
                                   persistence::GameError>;
struct ImportItem {
    std::size_t ordinal;
    ImportOutcome outcome;
    pgn::SourceRange range{};
    std::vector<pgn::Diagnostic> warnings{};
    std::size_t suppressedWarnings{};
    QString title{}; // Bounded player names for the post-import chooser; always plain text.
};
struct ImportBudget {
    std::size_t games = 10'000;
    std::size_t warnings = maxImportReportWarnings;
};
struct ImportReport {
    persistence::RequestId request;
    ImportStatus status = ImportStatus::Completed;
    std::vector<ImportItem> items;
    std::size_t attemptedGames = 0;
    bool gameLimitReached = false;
};
// Call on the owning application thread. Database must outlive closed().
// close() cancels production, drains the current game and reports closed asynchronously.
// Await closed() before destruction; never close Database before this service.
class GameImportService final : public QObject {
    Q_OBJECT
  public:
    explicit GameImportService(persistence::Database& database, QObject* parent = nullptr);
    ~GameImportService() override;
    void importText(persistence::RequestId request, std::string bytes,
                    persistence::GameSource source = {}, ImportBudget budget = {},
                    ImportMode mode = ImportMode::Batch);
    void cancel();
    void close();
  signals:
    // Emitted after submission, before the asynchronous durable outcome.
    void savingGame(chess_review::persistence::RequestId request, std::size_t ordinal);
    void progress(chess_review::persistence::RequestId request,
                  const chess_review::app::ImportItem& item);
    void finished(const chess_review::app::ImportReport& report);
    void rejected(chess_review::persistence::RequestId request,
                  const chess_review::persistence::GameError& error);
    void closed();

  private:
    struct ParseState;
    struct PendingBatchItem {
        std::size_t ordinal = 0;
        pgn::SourceRange range{};
        std::vector<pgn::Diagnostic> warnings{};
        std::size_t suppressedWarnings = 0;
        QString title{};
    };
    void next();
    void parsedChunk(std::vector<pgn::PgnDocument> games, std::optional<pgn::PgnOutcome> trailing);
    void handleTrailing(pgn::PgnOutcome outcome);
    void handleInsertOutcome(const PendingBatchItem& meta,
                             const persistence::InsertGameResult& result);
    void advanceAfterInsert();
    void finish(ImportStatus status);
    void finishClose();
    persistence::Database& database_;
    QThreadPool pool_;
    std::shared_ptr<ParseState> parse_;
    persistence::GameSource source_;
    ImportBudget budget_;
    ImportReport report_;
    persistence::RequestId storageRequest_;
    std::vector<PendingBatchItem> pendingItems_;
    std::optional<pgn::PgnOutcome> trailingOutcome_;
    std::size_t ordinal_ = 0;
    std::size_t retainedWarnings_ = 0;
    bool firstGamePending_ = true;
    bool active_ = false;
    bool closing_ = false;
    bool closed_ = false;
};
} // namespace chess_review::app
Q_DECLARE_METATYPE(chess_review::app::ImportItem)
Q_DECLARE_METATYPE(chess_review::app::ImportReport)
