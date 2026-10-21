#pragma once
#include "persistence/Database.h"
#include <QObject>
#include <QThreadPool>
#include <memory>
#include <stop_token>

namespace chess_review::app {
inline constexpr std::size_t maxImportReportWarnings = 1000;
enum class ImportStatus { Completed, Cancelled, Failed };
using ImportOutcome = std::variant<persistence::Inserted, persistence::Duplicate, pgn::PgnFailure,
                                   persistence::GameError>;
struct ImportItem {
    std::size_t ordinal;
    ImportOutcome outcome;
    pgn::SourceRange range{};
    std::vector<pgn::Diagnostic> warnings{};
    std::size_t suppressedWarnings{};
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
                    persistence::GameSource source = {}, ImportBudget budget = {});
    void cancel();
    void close();
  signals:
    // Emitted after submission, before the asynchronous durable outcome.
    void savingGame(std::size_t ordinal);
    void progress(const chess_review::app::ImportItem& item);
    void finished(const chess_review::app::ImportReport& report);
    void rejected(chess_review::persistence::RequestId request,
                  const chess_review::persistence::GameError& error);
    void closed();

  private:
    struct ParseState;
    void next();
    void parsed(pgn::PgnOutcome outcome);
    void finish(ImportStatus status);
    void finishClose();
    persistence::Database& database_;
    QThreadPool pool_;
    std::shared_ptr<ParseState> parse_;
    persistence::GameSource source_;
    ImportBudget budget_;
    ImportReport report_;
    persistence::RequestId storageRequest_;
    std::size_t ordinal_ = 0;
    pgn::SourceRange range_{};
    std::vector<pgn::Diagnostic> warnings_;
    std::size_t suppressedWarnings_ = 0;
    std::size_t retainedWarnings_ = 0;
    bool active_ = false;
    bool closing_ = false;
    bool closed_ = false;
};
} // namespace chess_review::app
Q_DECLARE_METATYPE(chess_review::app::ImportItem)
Q_DECLARE_METATYPE(chess_review::app::ImportReport)
