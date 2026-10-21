#include "import/LocalPgnInput.h"
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <array>

namespace chess_review::input {
InputResult readLocalPgn(const QUrl& url, std::size_t byteBudget, std::stop_token stop) {
    InputResult result;
    if (stop.stop_requested()) {
        result.cancelled = true;
        return result;
    }
    if (!url.isLocalFile() || url.hasQuery() || url.hasFragment() ||
        url.toLocalFile().contains(QChar::Null)) {
        result.error = QStringLiteral("Choose a local PGN file.");
        return result;
    }
    const QFileInfo info(url.toLocalFile());
    if (info.suffix().compare(QStringLiteral("pgn"), Qt::CaseInsensitive) != 0 || !info.isFile()) {
        result.error = QStringLiteral("Choose a regular .pgn file.");
        return result;
    }
    const auto uri = QUrl::fromLocalFile(info.absoluteFilePath()).toString(QUrl::FullyEncoded);
    if (uri.size() > 65'536 || info.size() < 0 || static_cast<quint64>(info.size()) > byteBudget) {
        result.error = QStringLiteral("File exceeds the remaining import limit.");
        return result;
    }
    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("Could not read the PGN file. Check file access.");
        return result;
    }
    std::array<char, 64 * 1024> buffer{};
    while (!file.atEnd()) {
        if (stop.stop_requested()) {
            result.cancelled = true;
            break;
        }
        const auto capacity = std::min(buffer.size(), byteBudget - result.bytesRead);
        if (capacity == 0) {
            result.error = QStringLiteral("File exceeds the remaining import limit.");
            break;
        }
        const auto count = file.read(buffer.data(), static_cast<qint64>(capacity));
        if (count <= 0) {
            result.error = QStringLiteral("Could not finish reading the PGN file.");
            break;
        }
        result.bytesRead += static_cast<std::size_t>(count);
        result.bytes.append(buffer.data(), static_cast<std::size_t>(count));
    }
    result.source = {QStringLiteral("file"), uri};
    if (!result.error.isEmpty() || result.cancelled) {
        result.bytes.clear();
    }
    return result;
}

InputResult encodePgnText(const QString& text, std::size_t byteBudget, std::stop_token stop) {
    InputResult result;
    result.source.type = QStringLiteral("paste");
    if (stop.stop_requested()) {
        result.cancelled = true;
        return result;
    }
    if (static_cast<quint64>(text.size()) > byteBudget || !text.isValidUtf16()) {
        result.error = QStringLiteral("Pasted text is too large or contains invalid Unicode.");
        return result;
    }
    // Encode in bounded chunks, without splitting a surrogate pair.
    for (qsizetype offset = 0; offset < text.size();) {
        if (stop.stop_requested()) {
            result.cancelled = true;
            result.bytes.clear();
            return result;
        }
        auto count = std::min<qsizetype>(16 * 1024, text.size() - offset);
        if (offset + count < text.size() && text.at(offset + count - 1).isHighSurrogate()) {
            --count;
        }
        const auto chunk = QStringView(text).mid(offset, count).toUtf8();
        if (static_cast<std::size_t>(chunk.size()) > byteBudget - result.bytesRead) {
            result.error = QStringLiteral("Pasted text exceeds the import limit.");
            result.bytes.clear();
            return result;
        }
        result.bytesRead += static_cast<std::size_t>(chunk.size());
        result.bytes.append(chunk.constData(), static_cast<std::size_t>(chunk.size()));
        offset += count;
    }
    return result;
}
} // namespace chess_review::input
