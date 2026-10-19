#pragma once
#include "diagnostics/Logger.h"
#include <QDialog>
#include <QPointer>
#include <QString>
#include <functional>

namespace chess_review::ui {
class DiagnosticsDialog final : public QDialog {
    Q_OBJECT
  public:
    DiagnosticsDialog(diagnostics::Logger* logger, std::function<QString()> reportProvider,
                      QWidget* parent);

  private:
    QPointer<diagnostics::Logger> logger_;
    std::function<QString()> reportProvider_;
};
} // namespace chess_review::ui
