#include "PipeIo.h"
#include "Scenario.h"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fake_uci {
PipeIo::PipeIo(QObject* parent) : QObject(parent) {
    outputs_[0].fd = STDOUT_FILENO;
    outputs_[1].fd = STDERR_FILENO;
}
PipeIo::~PipeIo() {
    stopInput();
    for (int fd = 0; fd < 3; ++fd) {
        const auto flags = originalFlags_[static_cast<std::size_t>(fd)];
        if (flags != -1) {
            // Best effort restoration; the process is already shutting down.
            (void)::fcntl(fd, F_SETFL, flags);
        }
    }
}
bool PipeIo::ignoreTermination() {
    struct sigaction ignore {};
    ignore.sa_handler = SIG_IGN;
    return ::sigemptyset(&ignore.sa_mask) == 0 && ::sigaction(SIGTERM, &ignore, nullptr) == 0;
}
bool PipeIo::start() {
    // Broken parent pipes are I/O errors, not accidental crashes. Intentional crash fixtures
    // must not leave core files in the caller's working directory.
    struct sigaction ignore {};
    ignore.sa_handler = SIG_IGN;
    if (::sigemptyset(&ignore.sa_mask) != 0 || ::sigaction(SIGPIPE, &ignore, nullptr) != 0) {
        return false;
    }
    const rlimit noCore{0, 0};
    if (::setrlimit(RLIMIT_CORE, &noCore) != 0) {
        return false;
    }
    // Status flags belong to open file descriptions, shared with the parent after fork.
    // Only exclusively owned pipe ends are supported. Reject terminals/files before mutation
    // and snapshot every flag first so merged stdout/stderr restore the same original state.
    std::array<int, 3> flags{};
    for (int fd = 0; fd < 3; ++fd) {
        struct stat status {};
        auto& saved = flags[static_cast<std::size_t>(fd)];
        saved = ::fcntl(fd, F_GETFL);
        if (::fstat(fd, &status) != 0 || !S_ISFIFO(status.st_mode) || saved == -1) {
            return false;
        }
    }
    originalFlags_ = flags;
    for (int fd = 0; fd < 3; ++fd) {
        if (::fcntl(fd, F_SETFL, flags[static_cast<std::size_t>(fd)] | O_NONBLOCK) == -1) {
            return false;
        }
    }
    input_ = std::make_unique<QSocketNotifier>(STDIN_FILENO, QSocketNotifier::Read);
    connect(input_.get(), &QSocketNotifier::activated, this, [this] { readInput(); });
    for (auto& output : outputs_) {
        output.notifier = std::make_unique<QSocketNotifier>(output.fd, QSocketNotifier::Write);
        output.notifier->setEnabled(false);
        connect(output.notifier.get(), &QSocketNotifier::activated, this, [this, &output] {
            if (!flush(output)) {
                failed();
                return;
            }
            if (pendingBytes() == 0) {
                drained();
            }
        });
    }
    return true;
}
void PipeIo::readInput() {
    std::array<char, 4096> bytes{};
    // A bounded read per activation gives timers/output a turn under sustained input.
    const auto count = ::read(STDIN_FILENO, bytes.data(), bytes.size());
    if (count > 0) {
        received(QByteArray(bytes.data(), static_cast<qsizetype>(count)));
    } else if (count == 0) {
        stopInput();
        eof();
    } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        stopInput();
        failed();
    }
}
void PipeIo::stopInput() {
    if (input_) {
        input_->setEnabled(false);
    }
}
qsizetype PipeIo::pendingBytes() const {
    return outputs_[0].bytes.size() - outputs_[0].offset + outputs_[1].bytes.size() -
           outputs_[1].offset;
}
void PipeIo::clearOutput() {
    for (auto& output : outputs_) {
        output.bytes.clear();
        output.offset = 0;
        if (output.notifier) {
            output.notifier->setEnabled(false);
        }
    }
}
bool PipeIo::write(bool stderrStream, const QByteArray& data) {
    if (data.size() > kMaxBytes - pendingBytes()) {
        return false;
    }
    auto& output = outputs_[stderrStream ? 1U : 0U];
    // Drop the consumed prefix before appending to keep physical storage bounded too.
    output.bytes.remove(0, output.offset);
    output.offset = 0;
    output.bytes.append(data);
    return flush(output);
}
bool PipeIo::flush(Output& output) {
    while (output.offset < output.bytes.size()) {
        const auto count = ::write(output.fd, output.bytes.constData() + output.offset,
                                   static_cast<std::size_t>(output.bytes.size() - output.offset));
        if (count > 0) {
            output.offset += static_cast<qsizetype>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            output.notifier->setEnabled(true);
            return true;
        } else {
            output.notifier->setEnabled(false);
            return false;
        }
    }
    output.bytes.clear();
    output.offset = 0;
    output.notifier->setEnabled(false);
    return true;
}
} // namespace fake_uci
