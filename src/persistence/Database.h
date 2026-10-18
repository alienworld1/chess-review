#pragma once

#include <QMetaType>
#include <QObject>
#include <QString>

#include <memory>
#include <optional>

namespace chess_review::persistence {

enum class DatabaseErrorCode { Path, Locked, Open, Configuration, Schema, Backup, Migration };

struct DatabaseError {
    DatabaseErrorCode code{};
    QString operation;
    QString detail;
    QString nativeCode;
    QString databasePath;
    QString backupPath;
    std::optional<int> migrationVersion;
};

// Call on the owning (application) thread. One initialization per instance.
// The owner must call close() and await closed() before destroying this service.
// SQL connections and all filesystem/database work belong to the storage thread.
class Database final : public QObject {
    Q_OBJECT

  public:
    explicit Database(QObject* parent = nullptr);
    ~Database() override;

    void initialize(const QString& databasePath);
    void close();

  signals:
    void progress(const QString& message);
    void ready(int schemaVersion);
    void failed(const chess_review::persistence::DatabaseError& error);
    void closed();

  private:
    void finishClose();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace chess_review::persistence

Q_DECLARE_METATYPE(chess_review::persistence::DatabaseError)
