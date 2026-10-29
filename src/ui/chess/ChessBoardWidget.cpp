#include "ui/chess/ChessBoardWidget.h"
#include <QAccessible>
#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QFocusEvent>
#include <QHideEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QSvgRenderer>
#include <QTimer>
#include <algorithm>
#include <cmath>

// Explicit initialization keeps resources linked when the UI lives in a static archive.
static void initializeBoardResources() { Q_INIT_RESOURCE(pieces); }
namespace chess_review::ui {
namespace {
constexpr std::array<const char*, 6> assetTypes{"p", "n", "b", "r", "q", "k"};
std::size_t pieceIndex(chess::Piece piece) {
    return static_cast<std::size_t>(piece.type) + (piece.color == chess::Color::Black ? 6U : 0U);
}
} // namespace
ChessBoardWidget::ChessBoardWidget(QWidget* parent) : QWidget(parent) {
    connect(&feedbackTimer_, &QTimer::timeout, this, [this] {
        feedbackSquare_.reset();
        update();
    });

    static const bool initialized = [] {
        initializeBoardResources();
        return true;
    }();
    static_cast<void>(initialized);
    installBoardAccessibility();
    setObjectName(QStringLiteral("chessBoard"));
    setAccessibleName(tr("Chessboard"));
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    // Let the containing layout allocate the available height. A preferred
    // vertical hint propagates through the shell's height-for-width scroll area
    // and otherwise forces the board and controls to overflow laptop windows.
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    setMinimumSize(240, 240);
    setToolTip(tr("Use the selected input mode to move pieces. "
                  "Board Options → Board Gestures and Keyboard Help lists all controls."));
    animation_.setStartValue(0.0);
    animation_.setEndValue(1.0);
    animation_.setDuration(170);
    animation_.setEasingCurve(QEasingCurve::OutCubic);
    connect(&animation_, &QVariantAnimation::valueChanged, this, [this] { update(); });
    connect(&animation_, &QVariantAnimation::finished, this, [this] {
        animatedMove_.reset();
        update();
    });
}
QSize ChessBoardWidget::sizeHint() const { return {560, 560}; }
QSize ChessBoardWidget::minimumSizeHint() const { return {240, 240}; }
QRectF ChessBoardWidget::boardRect() const {
    const int side = std::max(0, (std::min(width(), height()) - 4) / 8 * 8);
    // Center on integral logical pixels before converting to painter coordinates.
    const QPoint origin((width() - side) / 2, (height() - side) / 2);
    return {origin, QSize(side, side)};
}
QRectF ChessBoardWidget::squareRect(chess::Square square) const {
    const auto board = boardRect();
    const qreal side = board.width() / 8.0;
    const int column = state_.blackAtBottom ? 7 - square.file() : square.file();
    const int row = state_.blackAtBottom ? square.rank() : 7 - square.rank();
    return {board.left() + column * side, board.top() + row * side, side, side};
}
std::optional<chess::Square> ChessBoardWidget::squareAt(QPointF point) const {
    const auto board = boardRect();
    if (board.isEmpty() || point.x() < board.left() || point.x() >= board.right() ||
        point.y() < board.top() || point.y() >= board.bottom()) {
        return std::nullopt;
    }
    const int column = static_cast<int>((point.x() - board.left()) * 8 / board.width());
    const int row = static_cast<int>((point.y() - board.top()) * 8 / board.height());
    return chess::Square::fromCoordinates(state_.blackAtBottom ? 7 - column : column,
                                          state_.blackAtBottom ? row : 7 - row);
}
void ChessBoardWidget::setState(const app::BoardViewState& state) {
    const bool changedPosition =
        state.revision != state_.revision || state.blackAtBottom != state_.blackAtBottom;
    const auto pending = pendingDrop_;
    const bool sameOrientation = state.blackAtBottom == state_.blackAtBottom;
    if (changedPosition || !state.interactive) {
        cancelTransient();
        if (pending && sameOrientation) {
            if (state.revision == pending->revision) {
                // Keep the visual drop in place while the controller validates it.
                pendingDrop_ = pending;
            } else if (state.lastMove == pending->move) {
                settledDrop_ = pending->move;
            }
        }
    }
    const auto oldSelection = state_.selection;
    state_ = state;
    setAccessibleDescription(
        tr("%1 at bottom. %2. Enter enables keyboard square navigation.")
            .arg(state_.blackAtBottom ? tr("Black") : tr("White"), state_.positionDescription));
    if (oldSelection != state_.selection && state_.selection) {
        QAccessibleAnnouncementEvent announcement(this, squareDescription(*state_.selection));
        QAccessible::updateAccessibility(&announcement);
    }
    if (changedPosition) {
        QAccessibleEvent event(this, QAccessible::DescriptionChanged);
        QAccessible::updateAccessibility(&event);
    }
    update();
}
void ChessBoardWidget::setPreferences(const app::BoardPreferences& preferences) {
    cancelTransient();
    preferences_ = preferences;
    rebuildPieces();
    update();
}
void ChessBoardWidget::setAnnotationColor(int color) {
    if (color >= 0 && color < 4) {
        annotationColor_ = color;
    }
}
void ChessBoardWidget::rebuildPieces() {
    const auto cell = squareRect(chess::Square{}).size().toSize();
    const auto dpr = devicePixelRatioF();
    if (cell == cachedSize_ && qFuzzyCompare(dpr, cachedDpr_)) {
        return;
    }
    cachedSize_ = cell;
    cachedDpr_ = dpr;
    if (cell.isEmpty()) {
        return;
    }
    for (std::size_t i = 0; i < pieces_.size(); ++i) {
        const auto path = QStringLiteral(":/pieces/Chess_%1%2t45.svg")
                              .arg(QString::fromLatin1(assetTypes[i % 6]),
                                   i < 6 ? QStringLiteral("l") : QStringLiteral("d"));
        QSvgRenderer svg(path);
        Q_ASSERT_X(svg.isValid(), "ChessBoardWidget", "Bundled piece SVG is invalid");
        QPixmap image(QSize(qCeil(cell.width() * dpr), qCeil(cell.height() * dpr)));
        image.setDevicePixelRatio(dpr);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        // All pinned pieces have a square viewBox; retain their intrinsic aspect ratio.
        svg.render(&painter, QRectF(QPointF{}, QSizeF(cell))
                                 .adjusted(cell.width() * .06, cell.height() * .06,
                                           -cell.width() * .06, -cell.height() * .06));
        pieces_[i] = std::move(image);
    }
}
void ChessBoardWidget::flashMove(chess::Move move, bool success) {
    feedbackSquare_ = move.to;
    feedbackSuccess_ = success;
    feedbackTimer_.setSingleShot(true);
    feedbackTimer_.start(300);
    update();
}
void ChessBoardWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto board = boardRect();
    if (board.isEmpty()) {
        return;
    }
    const qreal side = board.width() / 8;
    const bool previewDrop = pendingDrop_ && pendingDrop_->revision == state_.revision;
    painter.setPen(QPen(QColor(QStringLiteral("#7d9085")), 2));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(board.adjusted(-1, -1, 1, 1));
    for (int rank = 0; rank < 8; ++rank) {
        for (int file = 0; file < 8; ++file) {
            const auto square = *chess::Square::fromCoordinates(file, rank);
            const auto rect = squareRect(square);
            const bool light = (file + rank) % 2 != 0;
            painter.fillRect(rect,
                             QColor(light ? QStringLiteral("#e9e1d0") : QStringLiteral("#97b49e")));
            if (state_.lastMove &&
                (state_.lastMove->from == square || state_.lastMove->to == square)) {
                painter.fillRect(rect,
                                 QColor(236, 194, 56, state_.lastMove->to == square ? 87 : 51));
            }
            if (state_.selection) {
                const bool legal =
                    std::any_of(state_.legalMoves.begin(), state_.legalMoves.end(),
                                [this, square](const auto& move) {
                                    return move.from == *state_.selection && move.to == square;
                                });
                if (legal) {
                    const bool capture = isCaptureDestination(square);
                    painter.fillRect(rect,
                                     capture ? QColor(180, 105, 65, 85) : QColor(39, 131, 75, 65));
                    // Shape redundancy distinguishes captures as well as color.
                    painter.setPen(QPen(QColor(39, 100, 60), 2));
                    painter.setBrush(Qt::NoBrush);
                    if (capture) {
                        painter.drawEllipse(rect.adjusted(4, 4, -4, -4));
                    } else {
                        painter.drawRect(
                            rect.adjusted(side * .1, side * .1, -side * .1, -side * .1));
                    }
                }
            }
            if (state_.selection == square) {
                painter.fillRect(rect, QColor(39, 131, 75, 72));
                painter.setPen(QPen(QColor(QStringLiteral("#185a32")), 2));
                painter.setBrush(Qt::NoBrush);
                painter.drawRect(rect.adjusted(2, 2, -2, -2));
            }
            if (state_.checkedKing == square) {
                QRadialGradient gradient(rect.center(), side * .6);
                gradient.setColorAt(0, QColor(197, 61, 73, 200));
                gradient.setColorAt(1, Qt::transparent);
                painter.fillRect(rect, gradient);
            }
            if (feedbackSquare_ == square && feedbackTimer_.isActive()) {
                painter.fillRect(rect, feedbackSuccess_ ? QColor(39, 131, 75, 100)
                                                        : QColor(197, 61, 73, 100));
            }
            if (preferences_.coordinates) {
                painter.setPen(
                    QColor(light ? QStringLiteral("#55655c") : QStringLiteral("#34493b")));
                auto font = painter.font();
                font.setPixelSize(std::clamp(static_cast<int>(side * .18), 9, 14));
                painter.setFont(font);
                const bool bottom = state_.blackAtBottom ? rank == 7 : rank == 0;
                const bool left = state_.blackAtBottom ? file == 7 : file == 0;
                if (bottom) {
                    painter.drawText(rect.adjusted(3, 1, -3, -1), Qt::AlignRight | Qt::AlignBottom,
                                     QString(QChar('a' + file)));
                }
                if (left) {
                    painter.drawText(rect.adjusted(3, 1, -3, -1), Qt::AlignLeft | Qt::AlignTop,
                                     QString::number(rank + 1));
                }
            }
            const auto piece = state_.pieces[static_cast<std::size_t>(square.index())];
            if (piece && !(dragging_ && !rightGesture_ && pressedSquare_ == square) &&
                !(animatedMove_ && animatedMove_->to == square) &&
                !(previewDrop &&
                  (pendingDrop_->move.from == square || pendingDrop_->move.to == square))) {
                painter.drawPixmap(rect.topLeft(), pieces_[pieceIndex(*piece)]);
            }
        }
    }
    if (previewDrop) {
        const auto piece = state_.pieces[static_cast<std::size_t>(pendingDrop_->move.from.index())];
        if (piece) {
            painter.drawPixmap(squareRect(pendingDrop_->move.to).topLeft(),
                               pieces_[pieceIndex(*piece)]);
        }
    }
    if (state_.status == app::BoardStatus::Checkmate && state_.checkedKing) {
        const auto king = squareRect(*state_.checkedKing);
        const qreal badgeSide = side * .34;
        const QRectF badge(king.right() - badgeSide - 2, king.top() + 2, badgeSide, badgeSide);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(QStringLiteral("#8b1e2d")));
        painter.drawEllipse(badge);
        auto font = painter.font();
        font.setBold(true);
        font.setPixelSize(std::max(10, static_cast<int>(badgeSide * .80)));
        painter.setFont(font);
        painter.setPen(Qt::white);
        painter.drawText(badge, Qt::AlignCenter, QStringLiteral("#"));
    }
    for (const auto& annotation : state_.annotations) {
        auto color =
            QColor(preferences_.annotationColors[static_cast<std::size_t>(annotation.color)]);
        color.setAlpha(175);
        const auto from = squareRect(annotation.from);
        const auto to = squareRect(annotation.to);
        if (annotation.from == annotation.to) {
            painter.setPen(QPen(color, std::max(2.0, side * .065)));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(from.adjusted(side * .10, side * .10, -side * .10, -side * .10));
        } else {
            QLineF line(from.center(), to.center());
            const qreal length = line.length();
            const QPointF unit((line.dx()) / length, (line.dy()) / length);
            const QPointF normal(-unit.y(), unit.x());
            const auto neck = line.p2() - unit * (side * .32);
            QPainterPath arrow;
            arrow.moveTo(line.p1() + normal * (side * .08));
            arrow.lineTo(neck + normal * (side * .08));
            arrow.lineTo(neck + normal * (side * .225));
            arrow.lineTo(line.p2());
            arrow.lineTo(neck - normal * (side * .225));
            arrow.lineTo(neck - normal * (side * .08));
            arrow.lineTo(line.p1() - normal * (side * .08));
            arrow.closeSubpath();
            painter.fillPath(arrow, color);
        }
    }
    if (dragging_ && rightGesture_ && pressedSquare_) {
        painter.setPen(
            QPen(QColor(preferences_.annotationColors[static_cast<std::size_t>(pressedColor_)]),
                 side * .10, Qt::DashLine, Qt::RoundCap));
        painter.drawLine(squareRect(*pressedSquare_).center(), pointer_);
    }
    if (dragging_ && !rightGesture_ && pressedSquare_) {
        const auto piece = state_.pieces[static_cast<std::size_t>(pressedSquare_->index())];
        if (piece) {
            painter.drawPixmap(pointer_ - QPointF(side / 2, side / 2), pieces_[pieceIndex(*piece)]);
        }
    }
    if (animatedMove_) {
        const auto piece = state_.pieces[static_cast<std::size_t>(animatedMove_->to.index())];
        const auto from = squareRect(animatedMove_->from).topLeft();
        const auto to = squareRect(animatedMove_->to).topLeft();
        if (piece) {
            painter.drawPixmap(from + (to - from) * animation_.currentValue().toReal(),
                               pieces_[pieceIndex(*piece)]);
        }
    }
    if (hasFocus()) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(QStringLiteral("#155080")), 3, Qt::DashLine));
        painter.drawRect((entryMode_ ? squareRect(focusedSquare_) : board).adjusted(3, 3, -3, -3));
    }
}
void ChessBoardWidget::resizeEvent(QResizeEvent* event) {
    cancelTransient();
    QWidget::resizeEvent(event);
    rebuildPieces();
}
void ChessBoardWidget::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::EnabledChange && !isEnabled()) {
        cancelTransient();
    }
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        cachedSize_ = {};
        rebuildPieces();
        update();
    }
}
bool ChessBoardWidget::event(QEvent* event) {
    if (event->type() == QEvent::DevicePixelRatioChange) {
        cancelTransient();
        rebuildPieces();
    }
    return QWidget::event(event);
}
void ChessBoardWidget::cancelTransient() {
    ++gestureRevision_;
    pressedSquare_.reset();
    dragging_ = false;
    rightGesture_ = false;
    arrowStart_.reset();
    animation_.stop();
    animatedMove_.reset();
    pendingDrop_.reset();
    settledDrop_.reset();
    if (promotion_) {
        promotion_->close();
    }
    update();
}
void ChessBoardWidget::animateMove(chess::Move move) {
    QAccessibleAnnouncementEvent announcement(
        this, tr("Move %1 to %2. %3")
                  .arg(QString::fromStdString(move.from.name()),
                       QString::fromStdString(move.to.name()), accessibleDescription()));
    QAccessible::updateAccessibility(&announcement);
    const bool dropped = settledDrop_ == move;
    settledDrop_.reset();
    if (!dropped && preferences_.animation && isVisible()) {
        animatedMove_ = move;
        animation_.start();
    }
}
int ChessBoardWidget::gestureColor(Qt::KeyboardModifiers modifiers) const {
    const bool shift = modifiers.testFlag(Qt::ShiftModifier);
    const bool control = modifiers.testFlag(Qt::ControlModifier);
    return shift && control ? 3 : control ? 2 : shift ? 1 : annotationColor_;
}
void ChessBoardWidget::mousePressEvent(QMouseEvent* event) {
    if (!state_.interactive ||
        (event->button() != Qt::LeftButton && event->button() != Qt::RightButton)) {
        return;
    }
    animation_.stop();
    animatedMove_.reset();
    setFocus(Qt::MouseFocusReason);
    const auto square = squareAt(event->position());
    if (!square) {
        return;
    }
    pressedSquare_ = square;
    pressPoint_ = event->position();
    pointer_ = pressPoint_;
    rightGesture_ = event->button() == Qt::RightButton;
    pressedColor_ = gestureColor(event->modifiers());
    if (!rightGesture_ && preferences_.inputMode == app::BoardInputMode::Drag) {
        emit squareSelected(*square);
    }
}
void ChessBoardWidget::mouseMoveEvent(QMouseEvent* event) {
    pointer_ = event->position();
    if (pressedSquare_ &&
        (pointer_ - pressPoint_).manhattanLength() >= QApplication::startDragDistance()) {
        if (rightGesture_ || (preferences_.inputMode == app::BoardInputMode::Drag &&
                              state_.selection == pressedSquare_)) {
            dragging_ = true;
        }
    }
    update();
}
void ChessBoardWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (!pressedSquare_ || !state_.interactive) {
        cancelTransient();
        return;
    }
    if ((rightGesture_ && event->button() != Qt::RightButton) ||
        (!rightGesture_ && event->button() != Qt::LeftButton)) {
        return;
    }
    const auto from = *pressedSquare_;
    const auto to = squareAt(event->position());
    const bool displaced =
        (event->position() - pressPoint_).manhattanLength() >= QApplication::startDragDistance();
    const bool dragged =
        dragging_ ||
        (displaced && (rightGesture_ || (preferences_.inputMode == app::BoardInputMode::Drag &&
                                         state_.selection == pressedSquare_)));
    const bool right = rightGesture_;
    const int color = pressedColor_;
    pressedSquare_.reset();
    dragging_ = false;
    rightGesture_ = false;
    update();
    if (!to) {
        return;
    }
    if (right) {
        if (dragged || from == *to) {
            emit annotationToggled({from, dragged ? *to : from, color});
        }
    } else if (preferences_.inputMode == app::BoardInputMode::Click) {
        if (from == *to) {
            activateSquare(*to);
        }
    } else if (dragged && from != *to) {
        proposeMove(from, *to, true);
    }
}
void ChessBoardWidget::activateSquare(chess::Square square) {
    if (!state_.interactive || !isEnabled() || !isVisible()) {
        return;
    }
    const auto piece = state_.pieces[static_cast<std::size_t>(square.index())];
    if (state_.selection && *state_.selection != square &&
        (!piece || piece->color != state_.sideToMove)) {
        proposeMove(*state_.selection, square);
    } else if (state_.selection == square) {
        emit selectionCleared();
    } else {
        emit squareSelected(square);
    }
}
void ChessBoardWidget::proposeMove(chess::Square from, chess::Square to, bool dropped) {
    const auto revision = state_.revision;
    if (dropped) {
        pendingDrop_ = PendingDrop{{from, to, std::nullopt}, revision};
        update();
    }
    const bool promotes = std::any_of(
        state_.legalMoves.begin(), state_.legalMoves.end(), [from, to](const auto& move) {
            return move.from == from && move.to == to && move.promotion;
        });
    if (!promotes) {
        emit moveAttempted({from, to, std::nullopt}, revision);
        return;
    }
    if (promotion_) {
        return;
    }
    promotion_ = new QMenu(this);
    promotion_->setObjectName(QStringLiteral("promotionMenu"));
    promotion_->setAccessibleName(tr("Choose promotion piece"));
    const std::array choices{std::pair{chess::PieceType::Queen, tr("&Queen")},
                             std::pair{chess::PieceType::Rook, tr("&Rook")},
                             std::pair{chess::PieceType::Bishop, tr("&Bishop")},
                             std::pair{chess::PieceType::Knight, tr("&Knight")}};
    const auto gestureRevision = ++gestureRevision_;
    for (const auto& [piece, label] : choices) {
        auto* action = promotion_->addAction(label);
        connect(action, &QAction::triggered, this,
                [this, from, to, piece, revision, dropped, gestureRevision] {
                    if (state_.interactive && state_.revision == revision &&
                        gestureRevision_ == gestureRevision && isEnabled() && isVisible()) {
                        ++gestureRevision_; // A chooser can submit at most once.
                        if (dropped) {
                            pendingDrop_ = PendingDrop{{from, to, piece}, revision};
                        }
                        emit moveAttempted({from, to, piece}, revision);
                    }
                });
    }
    auto* menu = promotion_;
    connect(menu, &QMenu::aboutToHide, this, [this, menu, gestureRevision] {
        promotion_ = nullptr;
        menu->deleteLater();
        // QAction::triggered follows aboutToHide. Let an accepted choice submit
        // first; a cancelled chooser restores the pawn without committing a move.
        QTimer::singleShot(0, this, [this, gestureRevision] {
            if (gestureRevision_ == gestureRevision) {
                ++gestureRevision_;
                pendingDrop_.reset();
                update();
            }
        });
    });
    menu->popup(mapToGlobal(squareRect(to).bottomLeft().toPoint()));
    menu->setActiveAction(menu->actions().front());
}
void ChessBoardWidget::focusSquare(chess::Square square) {
    if (!isEnabled() || !isVisible()) {
        return;
    }
    focusedSquare_ = square;
    entryMode_ = true;
    setFocus(Qt::OtherFocusReason);
    QAccessibleEvent event(this, QAccessible::Focus);
    event.setChild(square.index() + 1);
    QAccessible::updateAccessibility(&event);
    update();
}
void ChessBoardWidget::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        if (pressedSquare_ || arrowStart_ || animatedMove_ || promotion_) {
            cancelTransient();
        } else if (state_.selection) {
            emit selectionCleared();
        } else {
            entryMode_ = false;
            update();
        }
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
        event->key() == Qt::Key_Space) {
        if (!entryMode_) {
            focusSquare(focusedSquare_);
        } else {
            activateSquare(focusedSquare_);
        }
        return;
    }
    if (entryMode_) {
        int file = focusedSquare_.file();
        int rank = focusedSquare_.rank();
        const int direction = state_.blackAtBottom ? -1 : 1;
        switch (event->key()) {
        case Qt::Key_Left:
            file -= direction;
            break;
        case Qt::Key_Right:
            file += direction;
            break;
        case Qt::Key_Up:
            rank += direction;
            break;
        case Qt::Key_Down:
            rank -= direction;
            break;
        case Qt::Key_H:
            if (state_.interactive) {
                emit annotationToggled(
                    {focusedSquare_, focusedSquare_, gestureColor(event->modifiers())});
            }
            return;
        case Qt::Key_A:
            if (!state_.interactive) {
                return;
            }
            if (arrowStart_) {
                const auto start = *arrowStart_;
                arrowStart_.reset();
                emit annotationToggled({start, focusedSquare_, gestureColor(event->modifiers())});
            } else {
                arrowStart_ = focusedSquare_;
            }
            return;
        default:
            QWidget::keyPressEvent(event);
            return;
        }
        if (const auto square = chess::Square::fromCoordinates(file, rank)) {
            focusSquare(*square);
        }
        return;
    }
    switch (event->key()) {
    case Qt::Key_Left:
        emit previousRequested();
        return;
    case Qt::Key_Right:
        emit nextRequested();
        return;
    case Qt::Key_Home:
        emit beginningRequested();
        return;
    case Qt::Key_End:
        emit endRequested();
        return;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
}
void ChessBoardWidget::hideEvent(QHideEvent* event) {
    cancelTransient();
    QWidget::hideEvent(event);
}
void ChessBoardWidget::focusOutEvent(QFocusEvent* event) {
    // Promotion transfers focus to its popup; preserve it until choice or dismissal.
    if (!promotion_) {
        cancelTransient();
    }
    QWidget::focusOutEvent(event);
}
bool ChessBoardWidget::isCaptureDestination(chess::Square square) const {
    return state_.selection && std::any_of(state_.captureMoves.begin(), state_.captureMoves.end(),
                                           [this, square](const auto& move) {
                                               return move.from == *state_.selection &&
                                                      move.to == square;
                                           });
}
QString ChessBoardWidget::squareDescription(chess::Square square) const {
    const auto piece = state_.pieces[static_cast<std::size_t>(square.index())];
    QString description = QString::fromStdString(square.name());
    const std::array names{tr("pawn"), tr("knight"), tr("bishop"),
                           tr("rook"), tr("queen"),  tr("king")};
    description +=
        piece ? tr(", %1 %2").arg(piece->color == chess::Color::White ? tr("White") : tr("Black"),
                                  names[static_cast<std::size_t>(piece->type)])
              : tr(", empty");
    if (state_.selection == square) {
        description += tr(", selected");
    }
    if (state_.selection &&
        std::any_of(state_.legalMoves.begin(), state_.legalMoves.end(), [this, square](auto move) {
            return move.from == *state_.selection && move.to == square;
        })) {
        const bool capture = isCaptureDestination(square);
        description += capture ? tr(", legal capture") : tr(", legal move destination");
    }
    if (state_.checkedKing == square) {
        description +=
            state_.status == app::BoardStatus::Checkmate ? tr(", checkmated") : tr(", in check");
    }
    for (const auto& annotation : state_.annotations) {
        if (annotation.from == square) {
            description += tr(", annotation color %1").arg(annotation.color + 1);
            description +=
                annotation.from == annotation.to
                    ? tr(", highlighted")
                    : tr(", arrow to %1").arg(QString::fromStdString(annotation.to.name()));
        }
    }
    return description;
}
} // namespace chess_review::ui
