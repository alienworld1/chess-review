#pragma once

#include "app/GameImportService.h"
#include "import/LocalPgnInput.h"
#include <QList>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QUrl>
#include <cstddef>
#include <stop_token>
#include <vector>

namespace chess_review::app {
inline constexpr std::size_t maxBatchFiles = 32;
inline constexpr std::size_t maxBatchBytes = 64 * 1024 * 1024;
inline constexpr std::size_t maxBatchGames = 10'000;
enum class ImportFileState {
    Pending,
    Reading,
    Parsing,
    Completed,
    CompletedWithErrors,
    Failed,
    Cancelled,
    Unprocessed
};
enum class ImportBatchStatus { Idle, Running, Completed, Cancelled, Failed, GameLimitReached };
struct ImportFileResult {
    QString name;
    ImportFileState state = ImportFileState::Pending;
    QString message;
    std::vector<ImportItem> items;
};
// GUI-thread facade. Injected service must outlive closed(). close() owns service closure.
// Terminal notifications retain stable results; queue submissions requested by finished() slots.
class ImportController final : public QObject {
    Q_OBJECT
  public:
    explicit ImportController(GameImportService& service, QObject* parent = nullptr);
    ~ImportController() override;
    static bool acceptsFiles(const QList<QUrl>& files);
    bool importFiles(const QList<QUrl>& files);
    bool importText(QString text);
    void setReady(bool ready);
    bool available() const { return ready_ && !busy_ && !finishing_ && !closing_; }
    bool busy() const { return busy_; }
    ImportBatchStatus status() const { return status_; }
    bool cancelling() const { return stop_.stop_requested(); }
    const std::vector<ImportFileResult>& files() const { return results_; }
    std::size_t currentFile() const { return index_; }
    std::size_t currentGame() const { return currentGame_; }
    std::size_t inserted() const { return inserted_; }
    std::size_t duplicates() const { return duplicates_; }
    std::size_t failures() const { return failures_; }
    std::size_t warnings() const { return warnings_; }
    void cancel();
    void close();
  signals:
    void changed();
    void submitted();
    void rejected(const QString& message);
    void committed();
    void finished();
    void closed();

  private:
    bool begin();
    void next();
    void acquired(input::InputResult result);
    void completeFile(const ImportReport& report);
    void finish(ImportBatchStatus status);
    void finishClose();
    GameImportService& service_;
    QThreadPool pool_;
    QList<QUrl> urls_;
    QString text_;
    std::vector<ImportFileResult> results_;
    std::stop_source stop_;
    persistence::RequestId request_;
    std::size_t index_ = 0;
    std::size_t currentGame_ = 0;
    std::size_t bytesRead_ = 0;
    std::size_t attempted_ = 0;
    std::size_t retainedWarnings_ = 0;
    std::size_t inserted_ = 0;
    std::size_t duplicates_ = 0;
    std::size_t failures_ = 0;
    std::size_t warnings_ = 0;
    ImportBatchStatus status_ = ImportBatchStatus::Idle;
    bool finishing_ = false;
    bool ready_ = false;
    bool busy_ = false;
    bool reading_ = false;
    bool closing_ = false;
    bool closingService_ = false;
};
} // namespace chess_review::app
