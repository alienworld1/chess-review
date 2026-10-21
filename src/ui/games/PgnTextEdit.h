#pragma once

#include <QPlainTextEdit>
#include <QString>

class QMimeData;

namespace chess_review::ui {
// Source editor: preserve unchanged clipboard content and avoid Qt's ASCII text conversion.
class PgnTextEdit final : public QPlainTextEdit {
    Q_OBJECT
  public:
    explicit PgnTextEdit(QWidget* parent = nullptr);
    void setPgnText(QString text);
    QString pgnText() const;
    bool previewOnly() const noexcept { return previewOnly_; }
  signals:
    void previewChanged(bool previewOnly);
    void inputRejected(const QString& message);

  protected:
    void insertFromMimeData(const QMimeData* source) override;

  private:
    QString original_;
    bool edited_ = true;
    bool previewOnly_ = false;
};
} // namespace chess_review::ui
