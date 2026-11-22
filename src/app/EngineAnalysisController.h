#pragma once

#include "app/AnalysisController.h"
#include "app/EngineSettingsController.h"
#include <QThreadPool>

namespace chess_review::review {
struct PositionEvidence;
}
namespace chess_review::app {
struct EngineLinePresentation {
    QString evaluation;
    QString pv;
    std::optional<int> depth;
    std::optional<double> whiteShare;
    std::vector<chess::Move> moves;
    int rank = 1;
    bool operator==(const EngineLinePresentation&) const = default;
};
// Replay and format only on a worker. Scores remain raw in the engine layer.
EngineLinePresentation presentEngineLine(const chess::Position& root,
                                         const engine::AnalysisLine& line);

struct EngineAnalysisState {
    QString engineName;
    QString status;
    QString error;
    EngineLinePresentation line;
    std::vector<EngineLinePresentation> candidates;
    engine::SearchStatistics statistics;
    std::optional<int> targetDepth;
    std::uint64_t candidatesRevision = 0;
    int requestedLines = BoardPreferences::defaultAnalysisMultiPv;
    int availableLines = 1;
    bool canStart = false;
    bool canStop = false;
    bool canPause = false;
    bool canDeepen = false;
    bool paused = false;
    bool savedEvidence = false;
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
    void pause();
    void stop();
    void deepen();
    void setMultiPv(int count);
    void previewLine(int rank);
    void stepPreview(int delta);
    void dismissPreview();
    void addPreview();
    void showSavedReviewEvidence(const review::PositionEvidence& evidence);
    const std::optional<BoardViewState>& previewBoard() const { return previewBoard_; }
    const std::optional<EngineLinePresentation>& preview() const { return preview_; }
    int previewPly() const { return previewPly_; }
    enum class HiddenSearch { Stop, Suspend };
    void setVisible(bool visible, HiddenSearch hiddenSearch = HiddenSearch::Stop);
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
    void preparePreview();
    void clearPreview();
    void clearEvidence();
    bool boardReady() const;
    void finishClose();
    AnalysisController& board_;
    EngineSettingsController& settings_;
    engine::EngineSupervisor& supervisor_;
    EngineAnalysisState state_;
    // SAN replay must not delay board navigation; at most one running and one latest frame.
    QThreadPool formatting_;
    std::optional<engine::AnalysisSnapshot> pending_;
    std::shared_ptr<const chess::Position> root_;
    std::optional<EngineLinePresentation> preview_;
    std::optional<BoardViewState> previewBoard_;
    int previewPly_ = 0;
    std::uint64_t previewEpoch_ = 0;
    bool previewPending_ = false;
    QUuid session_;
    QUuid request_;
    std::optional<QUuid> attempt_;
    std::uint64_t revision_ = 0;
    std::uint64_t epoch_ = 0;
    bool visible_ = false;
    enum class Mode { Stopped, Following, Paused, Deepening };
    Mode mode_ = Mode::Stopped;
    bool admitted_ = false;
    bool cancelling_ = false;
    bool formattingBusy_ = false;
    bool receivedLiveEvidence_ = false;
    bool closing_ = false;
    bool closed_ = false;
};
} // namespace chess_review::app
