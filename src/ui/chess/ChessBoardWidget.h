#pragma once

#include "app/BoardState.h"
#include <QPixmap>
#include <QTimer>
#include <QVariantAnimation>
#include <QWidget>
#include <array>
#include <cstdint>

class QMenu;
namespace chess_review::ui {
class ChessBoardWidget final : public QWidget {
    Q_OBJECT
  public:
    explicit ChessBoardWidget(QWidget* parent = nullptr);
    void setState(const app::BoardViewState& state);
    void setPreferences(const app::BoardPreferences& preferences);
    void setAnnotationColor(int color);
    void animateMove(chess::Move move);
    void flashMove(chess::Move move, bool success);
    const app::BoardViewState& state() const { return state_; }
    QRectF boardRect() const;
    QRectF squareRect(chess::Square square) const;
    std::optional<chess::Square> squareAt(QPointF point) const;
    chess::Square focusedSquare() const { return focusedSquare_; }
    bool entryMode() const { return entryMode_; }
    void focusSquare(chess::Square square);
    void activateSquare(chess::Square square);
    QString squareDescription(chess::Square square) const;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
  signals:
    void squareSelected(chess_review::chess::Square square);
    void selectionCleared();
    void moveAttempted(chess_review::chess::Move move, std::uint64_t revision);
    void annotationToggled(chess_review::app::BoardAnnotation annotation);
    void previousRequested();
    void nextRequested();
    void beginningRequested();
    void endRequested();

  protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;
    bool event(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

  private:
    void rebuildPieces();
    bool isCaptureDestination(chess::Square square) const;
    void cancelTransient();
    void proposeMove(chess::Square from, chess::Square to, bool dropped = false);
    int gestureColor(Qt::KeyboardModifiers modifiers) const;
    app::BoardViewState state_;
    app::BoardPreferences preferences_;
    std::array<QPixmap, 12> pieces_;
    QSize cachedSize_;
    qreal cachedDpr_ = 0;
    std::optional<chess::Square> pressedSquare_;
    QPointF pressPoint_;
    QPointF pointer_;
    bool dragging_ = false;
    bool rightGesture_ = false;
    bool entryMode_ = false;
    chess::Square focusedSquare_;
    std::optional<chess::Square> arrowStart_;
    int annotationColor_ = 0;
    int pressedColor_ = 0;
    QTimer feedbackTimer_;
    std::optional<chess::Square> feedbackSquare_;
    bool feedbackSuccess_ = false;
    QMenu* promotion_ = nullptr;
    std::uint64_t gestureRevision_ = 0;
    QVariantAnimation animation_;
    std::optional<chess::Move> animatedMove_;
    struct PendingDrop {
        chess::Move move;
        std::uint64_t revision;
    };
    std::optional<PendingDrop> pendingDrop_;
    std::optional<chess::Move> settledDrop_;
};
void installBoardAccessibility();
} // namespace chess_review::ui
