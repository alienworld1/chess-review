#include "PipeIo.h"
#include "Scenario.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <queue>
#include <tuple>
#include <utility>
#include <vector>

namespace fake_uci {
namespace {
// Public process contract; deliberately separate from product diagnostics/logging.
enum class ExitCode { Success = 0, Configuration = 2, Mismatch = 3, Incomplete = 4, IoFailure = 5 };
struct Pending {
    qint64 dueMs{};
    Action action;
    qsizetype stepNumber{};
    qsizetype order{};
};
struct Later {
    bool operator()(const Pending& a, const Pending& b) const {
        return std::tie(a.dueMs, a.order) > std::tie(b.dueMs, b.order);
    }
};
class Runner final : public QObject {
  public:
    explicit Runner(Scenario scenario) : scenario_(std::move(scenario)) {
        timer_.setSingleShot(true);
        timer_.setTimerType(Qt::PreciseTimer);
        connect(&timer_, &QTimer::timeout, this, [this] { dispatch(); });
        io_.received = [this](const QByteArray& bytes) { consume(bytes); };
        io_.eof = [this] {
            if (hanging_ || (allowEof_ && line_.isEmpty())) {
                finish(ExitCode::Success, false);
            } else {
                fail(ExitCode::Incomplete, "stdin EOF before explicit termination");
            }
        };
        io_.failed = [this] {
            clearPending();
            io_.clearOutput();
            io_.stopInput();
            QCoreApplication::exit(static_cast<int>(ExitCode::IoFailure));
        };
        io_.drained = [this] {
            if (finishing_) {
                complete();
            }
        };
    }
    bool start() {
        clock_.start();
        return io_.start();
    }

  private:
    void clearPending() {
        timer_.stop();
        pending_ = {};
        scheduledBytes_ = 0;
    }
    void complete() const {
        if (crashing_) {
            std::abort();
        }
        QCoreApplication::exit(static_cast<int>(exitCode_));
    }
    void finish(ExitCode code, bool crash) {
        clearPending();
        io_.stopInput();
        finishing_ = true;
        exitCode_ = code;
        crashing_ = crash;
        if (io_.pendingBytes() == 0) {
            complete();
        }
    }
    void fail(ExitCode code, const QByteArray& reason,
              std::optional<qsizetype> stepNumber = std::nullopt) {
        clearPending();
        io_.clearOutput();
        const auto message = "fake-uci: step " +
                             QByteArray::number(stepNumber.value_or(step_ + 1)) + ": " + reason +
                             '\n';
        if (!io_.write(true, message)) {
            finish(ExitCode::IoFailure, false);
            return;
        }
        finish(code, false);
    }
    void consume(const QByteArray& bytes) {
        if (finishing_ || hanging_) {
            // Hang ignores commands, but still observes pipe EOF so abandoned fixtures can exit.
            return;
        }
        for (const char byte : bytes) {
            if (byte == '\n' || byte == '\r') {
                if (!line_.isEmpty()) {
                    auto command = std::exchange(line_, {});
                    accept(command);
                    if (finishing_ || hanging_) {
                        return;
                    }
                }
            } else {
                if (line_.size() == kMaxLineBytes) {
                    fail(ExitCode::Mismatch, "input line exceeds 64 KiB");
                    return;
                }
                line_ += byte;
            }
        }
    }
    void accept(const QByteArray& line) {
        const auto command = normalizedCommand(line);
        if (step_ == scenario_.steps.size() || command != scenario_.steps[step_].expect) {
            // Limit echoed content and escape controls; input cannot forge diagnostic lines.
            const auto describe = [](const QByteArray& text) { return text.left(128).toHex(); };
            const auto expected = step_ < scenario_.steps.size() ? scenario_.steps[step_].expect
                                                                 : QByteArray("<end>");
            fail(ExitCode::Mismatch, "command mismatch; expected hex=" + describe(expected) +
                                         " received hex=" + describe(command));
            return;
        }
        const auto& actions = scenario_.steps[step_++].actions;
        allowEof_ = scenario_.steps[step_ - 1].allowEof;
        const auto acceptedMs = clock_.elapsed();
        for (const auto& action : actions) {
            if (action.type == ActionType::CancelPending) {
                clearPending();
                continue;
            }
            if (pending_.size() == static_cast<std::size_t>(kMaxActions) ||
                action.data.size() > kMaxBytes - scheduledBytes_ - io_.pendingBytes()) {
                fail(ExitCode::IoFailure, "queued output exceeds fixture limits", step_);
                return;
            }
            scheduledBytes_ += action.data.size();
            pending_.push({acceptedMs + action.afterMs, action, step_, nextOrder_++});
        }
        dispatch();
    }
    void dispatch() {
        timer_.stop();
        while (!pending_.empty()) {
            const auto remainingMs = pending_.top().dueMs - clock_.elapsed();
            if (remainingMs > 0) {
                timer_.start(static_cast<int>(remainingMs));
                return;
            }
            auto pending = pending_.top();
            auto& action = pending.action;
            pending_.pop();
            scheduledBytes_ -= action.data.size();
            switch (action.type) {
            case ActionType::Write:
                if (!io_.write(action.stderrStream, action.data)) {
                    fail(ExitCode::IoFailure, "output write failed", pending.stepNumber);
                    return;
                }
                break;
            case ActionType::Exit:
                finish(ExitCode::Success, false);
                return;
            case ActionType::Crash:
                if (!io_.write(true, "fake-uci: intentional crash\n")) {
                    finish(ExitCode::IoFailure, false);
                } else {
                    finish(ExitCode::Success, true);
                }
                return;
            case ActionType::Hang:
                clearPending();
                hanging_ = true;
                return;
            case ActionType::CancelPending:
                Q_UNREACHABLE();
            }
        }
    }
    Scenario scenario_;
    PipeIo io_;
    QTimer timer_;
    QElapsedTimer clock_;
    // Sequence breaks equal-deadline ties without re-sorting the backlog for each command.
    std::priority_queue<Pending, std::vector<Pending>, Later> pending_;
    QByteArray line_;
    qsizetype step_{};
    qsizetype scheduledBytes_{};
    qsizetype nextOrder_{}; // At most 4,096 actions can be accepted in a scenario.
    ExitCode exitCode_{};
    bool allowEof_{};
    bool hanging_{};
    bool finishing_{};
    bool crashing_{};
};
} // namespace
} // namespace fake_uci

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    const auto arguments = application.arguments();
    constexpr auto* usage = "Usage: fake-uci-engine --scenario <path>\n";
    if (arguments.size() == 2 && arguments[1] == u"--help") {
        return std::fputs(usage, stdout) < 0 || std::fflush(stdout) != 0
                   ? static_cast<int>(fake_uci::ExitCode::IoFailure)
                   : static_cast<int>(fake_uci::ExitCode::Success);
    }
    if (arguments.size() != 3 || arguments[1] != u"--scenario") {
        (void)std::fputs(usage, stderr);
        return static_cast<int>(fake_uci::ExitCode::Configuration);
    }
    QString error;
    auto scenario = fake_uci::readScenario(arguments[2], error);
    if (!scenario) {
        (void)std::fprintf(stderr, "fake-uci: configuration: %s\n", qPrintable(error));
        return static_cast<int>(fake_uci::ExitCode::Configuration);
    }
    fake_uci::Runner runner(std::move(*scenario));
    if (!runner.start()) {
        (void)std::fputs("fake-uci: transport requires exclusive stdin/stdout/stderr pipes\n",
                         stderr);
        return static_cast<int>(fake_uci::ExitCode::IoFailure);
    }
    return QCoreApplication::exec();
}
