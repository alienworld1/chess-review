#pragma once

#include <QByteArray>
#include <QObject>
#include <QSocketNotifier>
#include <array>
#include <functional>
#include <memory>

namespace fake_uci {
// Native nonblocking pipe operations are confined here. Owned only by the event-loop thread.
class PipeIo final : public QObject {
  public:
    explicit PipeIo(QObject* parent = nullptr);
    ~PipeIo() override;
    bool start();
    bool ignoreTermination();
    bool write(bool stderrStream, const QByteArray& data);
    qsizetype pendingBytes() const;
    void stopInput();
    void clearOutput();
    std::function<void(const QByteArray&)> received;
    std::function<void()> eof;
    std::function<void()> failed;
    std::function<void()> drained;

  private:
    struct Output {
        int fd{};
        QByteArray bytes;
        qsizetype offset{};
        std::unique_ptr<QSocketNotifier> notifier;
    };
    void readInput();
    bool flush(Output& output);
    std::unique_ptr<QSocketNotifier> input_;
    std::array<Output, 2> outputs_;
    std::array<int, 3> originalFlags_{{-1, -1, -1}};
};
} // namespace fake_uci
