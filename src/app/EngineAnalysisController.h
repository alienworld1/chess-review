#pragma once

#include "app/AnalysisController.h"
#include "app/EngineSettingsController.h"
#include <QThreadPool>

namespace chess_review::app {
struct EngineLinePresentation {
    QString evaluation;
    QString pv;
    std::optional<int> depth;
    std::optional<double> whiteShare;
};
// Replay and format only on a worker. Scores remain raw in the engine layer.
EngineLinePresentation presentEngineLine(const chess::Position& root,
                                         const engine::AnalysisLine& line);

struct EngineAnalysisState {
    QString engineName;
    QString status;
    QString error;
    EngineLinePresentation line;
    bool canStart = false;
    bool canStop = false;
};

// GUI-thread presentation facade; the supervisor remains the sole scheduler owner.
// close() drains formatting and cancellation asynchronously; await closed().
class EngineAnalysisController final : public QObject {
    Q_OBJECT
  public:
    EngineAnalysisController(AnalysisController& board, EngineSettingsController& settings,
                             engine::EngineSupervisor& supervisor, QObject* parent = nullptr);
    ~EngineAnalysisController() override;
    const EngineAnalysisState& state() const { return state_; }
    void start();
    void stop();
    void setVisible(bool visible);
    void close();
  signals:
    void changed();
    void closed();

  private:
    void boardChanged();
    void runtimeChanged();
    void reconcile();
    void cancel();
    void updateControls();
    void acceptEvidence(const engine::ScheduledEvidence& evidence);
    void formatNext();
    void finishClose();
    AnalysisController& board_;
    EngineSettingsController& settings_;
    engine::EngineSupervisor& supervisor_;
    EngineAnalysisState state_;
    // SAN replay must not delay board navigation; at most one running and one latest frame.
    QThreadPool formatting_;
    std::optional<engine::AnalysisSnapshot> pending_;
    std::shared_ptr<const chess::Position> root_;
    QUuid session_;
    QUuid request_;
    std::optional<QUuid> attempt_;
    std::uint64_t revision_ = 0;
    std::uint64_t epoch_ = 0;
    bool visible_ = false;
    bool following_ = false;
    bool cancelling_ = false;
    bool formattingBusy_ = false;
    bool closing_ = false;
    bool closed_ = false;
};
} // namespace chess_review::app
