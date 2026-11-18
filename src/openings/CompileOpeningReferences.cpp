#include "openings/OpeningIndex.h"
#include <QFile>
#include <QSaveFile>
#include <array>

int main(int argc, char** argv) {
    if (argc != 7) {
        qCritical("Expected the output path and five Lichess ECO volumes.");
        return 1;
    }
    constexpr qint64 maxVolumeBytes = 256 * 1024;
    std::array<QByteArray, 5> volumes;
    std::array<std::string_view, 5> views;
    for (std::size_t i = 0; i < volumes.size(); ++i) {
        QFile input(QFile::decodeName(argv[i + 2]));
        if (!input.open(QIODevice::ReadOnly)) {
            qCritical("Could not open a Lichess ECO volume.");
            return 1;
        }
        auto& bytes = volumes[i];
        bytes = input.read(maxVolumeBytes + 1);
        if (input.error() != QFileDevice::NoError || bytes.size() > maxVolumeBytes ||
            !bytes.isValidUtf8()) {
            qCritical("Invalid Lichess ECO volume.");
            return 1;
        }
        views[i] = {bytes.constData(), static_cast<std::size_t>(bytes.size())};
    }
    const auto references = chess_review::openings::compileReferences(views);
    if (!references) {
        qCritical("Could not replay the Lichess ECO references.");
        return 1;
    }
    QSaveFile output(QFile::decodeName(argv[1]));
    const auto size = static_cast<qint64>(references->size());
    if (!output.open(QIODevice::WriteOnly) || output.write(references->data(), size) != size ||
        !output.commit()) {
        qCritical("Could not write the opening lookup resource.");
        return 1;
    }
}
