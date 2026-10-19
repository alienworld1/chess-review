#pragma once
#include <QObject>
#include <QString>
#include <optional>

namespace chess_review::app {
enum class Destination { Home, Games, Puzzles, Analyze, Settings };
QString destinationId(Destination destination);
std::optional<Destination> destinationFromId(const QString& id);

class NavigationController final : public QObject {
    Q_OBJECT
  public:
    explicit NavigationController(QObject* parent = nullptr);
    Destination current() const;
    void navigate(Destination destination);
  signals:
    void destinationChanged(chess_review::app::Destination destination);

  private:
    Destination current_ = Destination::Home;
};
} // namespace chess_review::app
Q_DECLARE_METATYPE(chess_review::app::Destination)
