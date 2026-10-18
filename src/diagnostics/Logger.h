#pragma once

#include <QObject>
#include <QString>
#include <QtLogging>

#include <functional>
#include <memory>
#include <optional>

namespace chess_review::diagnostics {

enum class Event {
    SessionStarted,
    SessionStopped,
    StorageStarted,
    StorageReady,
    StorageFailed,
    StorageClosed,
    SchemaChecked,
    BackupStarted,
    BackupVerified,
    MigrationStarted,
    MigrationCompleted,
    QtMessage,
    EventsDropped
};

enum class Severity { Debug, Info, Warning, Error, Fatal };
enum class QtCategory { Default, Core, Gui, Widgets, Sql, Other };
enum class Health { Starting, Active, MemoryOnly, Closed };

// Only bounded numeric metadata crosses the logging boundary. Never add free-form
// text, paths, or native error messages here.
struct Context {
    std::optional<QtCategory> qtCategory;
    std::optional<int> schemaVersion;
    std::optional<int> migrationVersion;
    std::optional<int> errorCode;
    std::optional<qint64> elapsedMs;
    std::optional<quint64> count;
};

class Logger final : public QObject {
    Q_OBJECT

  public:
    explicit Logger(const QString& logDirectory, bool debugEnabled = false,
                    QObject* parent = nullptr);
    ~Logger() override;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // Thread-safe; disk I/O belongs exclusively to the writer thread.
    void record(Event event, const Context& context = {}, Severity severity = Severity::Info);
    QString report() const;
    Health health() const;
    QString logDirectory() const;
    // Owning thread only. Await closed() before destruction; does not block the GUI.
    void close();
    void installQtHandler();

  signals:
    void healthChanged();
    void closed();

  private:
    // Internal deterministic writer seam used by failure/backpressure tests.
    Logger(const QString& directory, bool debug, std::function<bool()> beforeWrite,
           QObject* parent);
    void finishClose();
    void restoreQtHandler();
    static void qtMessage(QtMsgType type, const QMessageLogContext& context,
                          const QString& message);
    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend class LoggerTestAccess;
};

} // namespace chess_review::diagnostics
