#include "app/GamePgn.h"
#include "persistence/ReviewRepository.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringDecoder>
#include <QStringList>
#include <QUrl>
#include <algorithm>
#include <array>
#include <utility>

namespace chess_review::app {
namespace {
constexpr std::string_view colors = "GRYB";
QString message(const char* text) {
    return QCoreApplication::translate("AnalysisController", text);
}
} // namespace
std::string analysisCommentWithDrawings(QString comment,
                                        const std::vector<BoardAnnotation>& drawings) {
    QStringList squares;
    QStringList arrows;
    for (const auto& drawing : drawings) {
        auto text = QLatin1Char(colors[static_cast<std::size_t>(drawing.color)]) +
                    QString::fromStdString(drawing.from.name());
        if (drawing.from == drawing.to) {
            squares.append(text);
        } else {
            arrows.append(text + QString::fromStdString(drawing.to.name()));
        }
    }
    const auto append = [&comment](const QString& command, const QStringList& values) {
        if (!values.empty()) {
            if (!comment.isEmpty()) {
                comment += QLatin1Char('\n');
            }
            comment += QStringLiteral("[%1 %2]").arg(command, values.join(QLatin1Char(',')));
        }
    };
    append(QStringLiteral("%csl"), squares);
    append(QStringLiteral("%cal"), arrows);
    return comment.toStdString();
}

namespace {
struct PgnNote {
    QByteArray comment;
    std::optional<int> nag;
};
using PgnNotes = std::map<chess::NodeId, PgnNote>;

class Movetext {
  public:
    Movetext(const chess::GameTree& tree, const AnalysisDrawings& drawings, const PgnNotes& notes,
             QByteArray& bytes)
        : tree_(tree), drawings_(drawings), notes_(notes), bytes_(bytes) {}

    void token(const QByteArray& value) {
        const qsizetype separator = column_ == 0 ? 0 : 1;
        if (column_ + value.size() + separator > 79) {
            newline();
        }
        if (column_ != 0) {
            bytes_ += ' ';
            ++column_;
        }
        bytes_ += value;
        const auto lastNewline = value.lastIndexOf('\n');
        column_ = lastNewline >= 0 ? value.size() - lastNewline - 1 : column_ + value.size();
    }
    void newline() {
        if (column_ != 0) {
            bytes_ += '\n';
            column_ = 0;
        }
    }
    bool comment(std::string_view value, QByteArrayView prefix = {}) {
        if (value.empty()) {
            return true;
        }
        if (value.size() + static_cast<std::size_t>(prefix.size()) >
            pgn::PgnLimits{}.tree.maxCommentBytes) {
            return false;
        }
        const auto bytes = QByteArray(value.data(), static_cast<qsizetype>(value.size()));
        // Braces cannot escape a closing brace. PGN line comments can carry the same text.
        if (bytes.contains('}')) {
            for (const auto& line : bytes.split('\n')) {
                newline();
                token(QByteArray(";") + prefix + line);
                newline();
            }
        } else {
            token(QByteArray("{") + prefix + bytes + '}');
        }
        return true;
    }
    chess::NodeAnnotations annotations(chess::NodeId id, chess::NodeAnnotations notes) const {
        if (const auto found = drawings_.find(id); found != drawings_.end()) {
            notes.commentBefore = analysisComment(notes.commentBefore).toStdString();
            notes.commentAfter =
                analysisCommentWithDrawings(analysisComment(notes.commentAfter), found->second);
        }
        return notes;
    }
    bool line(std::span<const chess::NodeId> siblings, std::uint32_t number, chess::Color side,
              std::size_t depth, std::optional<std::size_t> recordedPlies = std::nullopt) {
        std::vector<chess::NodeId> continuation;
        bool numberNeeded = true;
        while (!siblings.empty()) {
            if (depth > pgn::PgnLimits{}.maxVariationDepth ||
                static_cast<std::size_t>(bytes_.size()) > pgn::PgnLimits{}.maxGameBytes) {
                return false;
            }
            auto node = *tree_.node(siblings.front());
            const auto notes = annotations(node.id, std::move(node.annotations));
            const auto extra = notes_.find(node.id);
            const auto moveNumber =
                QByteArray::number(number) + (side == chess::Color::White ? "." : "...");
            if (side == chess::Color::White || numberNeeded || !notes.commentBefore.empty()) {
                token(moveNumber);
            }
            if (!comment(notes.commentBefore)) {
                return false;
            }
            token(QByteArray::fromStdString(*node.san));
            for (const auto nag : notes.nags) {
                token("$" + QByteArray::number(nag));
            }
            if (extra != notes_.end() && extra->second.nag &&
                std::ranges::none_of(notes.nags, [](auto nag) { return nag >= 1 && nag <= 6; })) {
                token("$" + QByteArray::number(*extra->second.nag));
            }
            if (!comment(notes.commentAfter) ||
                (extra != notes_.end() && !comment(std::string_view(extra->second.comment)))) {
                return false;
            }
            numberNeeded = siblings.size() > 1 || !notes.nags.empty() ||
                           !notes.commentAfter.empty() || extra != notes_.end();
            for (const auto alternate : siblings.subspan(1)) {
                token("(");
                if (!line(std::span(&alternate, 1), number, side, depth + 1)) {
                    return false;
                }
                token(")");
            }
            if (recordedPlies && --*recordedPlies == 0) {
                if (!node.children.empty()) {
                    // Import merges repeated moves. Keep an analysis continuation after the last
                    // played move in a variation so the game still ends on the recorded move.
                    token("(");
                    token(moveNumber);
                    token(QByteArray::fromStdString(*node.san));
                    const auto nextSide =
                        side == chess::Color::White ? chess::Color::Black : chess::Color::White;
                    if (!line(node.children, number + (side == chess::Color::Black), nextSide,
                              depth + 1)) {
                        return false;
                    }
                    token(")");
                }
                break;
            }
            continuation = std::move(node.children);
            siblings = continuation;
            if (side == chess::Color::White) {
                side = chess::Color::Black;
            } else {
                side = chess::Color::White;
                ++number;
            }
        }
        return true;
    }

  private:
    const chess::GameTree& tree_;
    const AnalysisDrawings& drawings_;
    const PgnNotes& notes_;
    QByteArray& bytes_;
    qsizetype column_ = 0;
};
} // namespace

QString analysisComment(std::string_view comment, std::vector<BoardAnnotation>* drawings) {
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::ConvertInitialBom);
    const QString clean =
        decoder(QByteArrayView(comment.data(), static_cast<qsizetype>(comment.size())));
    if (!clean.contains(QStringLiteral("[%c"))) {
        return clean;
    }
    static const QRegularExpression directive(QStringLiteral(
        R"(\[%(csl|cal)[ \t]+([GRYB][a-h][1-8](?:[a-h][1-8])?(?:,[ \t]*[GRYB][a-h][1-8](?:[a-h][1-8])?)*)\])"));
    auto matches = directive.globalMatch(clean);
    QString text;
    qsizetype keptFrom = 0;
    while (matches.hasNext()) {
        const auto match = matches.next();
        const bool arrows = match.capturedView(1) == QStringLiteral("cal");
        std::vector<BoardAnnotation> decoded;
        bool valid = true;
        for (const auto& entry : match.captured(2).split(QLatin1Char(','))) {
            const auto shape = entry.trimmed().toLatin1();
            if (shape.size() != (arrows ? 5 : 3)) {
                valid = false;
                break;
            }
            const auto from = *chess::Square::fromName(std::string_view(shape.constData() + 1, 2));
            const auto to =
                arrows ? *chess::Square::fromName(std::string_view(shape.constData() + 3, 2))
                       : from;
            if (arrows && from == to) {
                valid = false;
                break;
            }
            const auto color = static_cast<int>(colors.find(shape.front()));
            decoded.push_back({from, to, color});
        }
        if (!valid) {
            continue;
        }
        text += QStringView(clean).sliced(keptFrom, match.capturedStart() - keptFrom);
        keptFrom = match.capturedEnd();
        if (drawings) {
            for (const auto& shape : decoded) {
                if (drawings->size() < 256 &&
                    std::find(drawings->begin(), drawings->end(), shape) == drawings->end()) {
                    drawings->push_back(shape);
                }
            }
        }
    }
    if (keptFrom == 0) {
        return clean;
    }
    text += QStringView(clean).sliced(keptFrom);
    return text.trimmed();
}

namespace {
QString writePgn(const QString& filename, const chess::GameTree& tree,
                 const pgn::PgnDocument* source, const AnalysisDrawings& drawings,
                 const PgnNotes& notes = {}, const openings::Opening* opening = nullptr) {
    const auto writeError = [] {
        return message("Could not save the PGN. Choose a writable folder and try again.");
    };
    const auto limitError = [] {
        return message("The game and its annotations are too large to save as a reopenable PGN.");
    };
    bool original = source && &source->tree == &tree;
    if (source && !original) {
        std::vector<chess::Move> mainline;
        const auto ids = std::get<std::vector<chess::NodeId>>(tree.mainLine(tree.rootId()));
        for (const auto id : ids) {
            if (const auto move = tree.node(id)->move) {
                mainline.push_back(*move);
            }
        }
        original = std::ranges::equal(mainline, source->originalMainline());
    }
    const auto result = original ? source->result : pgn::GameResult::Unknown;
    const std::array<QByteArray, 4> results{"*", "1-0", "0-1", "1/2-1/2"};
    const auto& termination = results[static_cast<std::size_t>(result)];
    const auto root = std::get<chess::Position>(tree.position(tree.rootId()));
    const auto context = root.context();
    QByteArray bytes;
    const auto tag = [&bytes](QByteArray name, QByteArray value) {
        value.replace("\\", "\\\\").replace("\"", "\\\"");
        bytes += '[' + name + " \"" + value + "\"]\n";
    };
    const std::array<std::string_view, 7> roster{"Event", "Site",  "Date",  "Round",
                                                 "White", "Black", "Result"};
    for (const auto name : roster) {
        QByteArray value = name == "Date" ? "????.??.??" : "?";
        if (name == "Result") {
            value = termination;
        } else if (source) {
            const auto found = std::ranges::find(source->tags, name, &pgn::Tag::name);
            if (found != source->tags.end()) {
                value = QByteArray::fromStdString(found->value);
            }
        }
        tag(QByteArray(name.data(), static_cast<qsizetype>(name.size())), value);
    }
    std::map<QByteArray, QByteArray> extras{{pgn::analysisCopyTag, "1"}};
    if (context.provenance == chess::RootProvenance::FenAnalysisRoot) {
        extras.emplace("SetUp", "1");
        extras.emplace("FEN", QByteArray::fromStdString(context.rootFen));
    }
    if (source) {
        for (const auto& extra : source->tags) {
            if (std::ranges::find(roster, extra.name) != roster.end() || extra.name == "FEN" ||
                extra.name == "SetUp" || (extra.name == "PlyCount" && !original) ||
                extra.name == pgn::analysisCopyTag ||
                (!original && (extra.name == "Termination" || extra.name == "ECO" ||
                               extra.name == "Opening" || extra.name == "Variation"))) {
                continue;
            }
            extras.emplace(QByteArray::fromStdString(extra.name),
                           QByteArray::fromStdString(extra.value));
        }
    }
    if (opening) {
        extras.try_emplace("ECO", QByteArray::fromStdString(opening->eco));
        extras.try_emplace("Opening", QByteArray::fromStdString(opening->name));
    }
    for (const auto& [name, value] : extras) {
        tag(name, value);
    }
    bytes += '\n';
    Movetext text(tree, drawings, notes, bytes);
    auto rootNode = *tree.node(tree.rootId());
    const auto rootNotes = text.annotations(rootNode.id, std::move(rootNode.annotations));
    const auto rootExtra = notes.find(rootNode.id);
    const auto recordedPlies = source && &source->tree == &tree
                                   ? std::optional(source->originalMainline().size())
                                   : std::nullopt;
    if (!text.comment(rootNotes.commentBefore, pgn::rootCommentPrefix) ||
        !text.comment(rootNotes.commentAfter, pgn::rootCommentPrefix) ||
        (rootExtra != notes.end() &&
         !text.comment(std::string_view(rootExtra->second.comment), pgn::rootCommentPrefix)) ||
        !text.line(rootNode.children, root.fullmoveNumber(), root.sideToMove(), 0, recordedPlies)) {
        return limitError();
    }
    text.token(termination);
    text.newline();
    bytes += '\n';
    if (static_cast<std::size_t>(bytes.size()) > pgn::PgnLimits{}.maxGameBytes) {
        return limitError();
    }
    // Check the same reader used by Games before replacing the destination.
    pgn::PgnReader reader(
        std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
    const auto parsed = reader.next();
    const auto* reopened = std::get_if<pgn::PgnDocument>(&parsed);
    if (!reopened || reopened->result != result || reopened->tree.size() != tree.size() ||
        (recordedPlies &&
         !std::ranges::equal(reopened->originalMainline(), source->originalMainline()))) {
        return message(
            "The game and its annotations could not be represented as a reopenable PGN.");
    }
    QSaveFile file(filename);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        return writeError();
    }
    return {};
}
} // namespace

QString writeAnalysisPgn(const QString& filename, const chess::GameTree& tree,
                         const pgn::PgnDocument* source, const AnalysisDrawings& drawings) {
    return writePgn(filename, tree, source, drawings);
}

QString writeReviewPgn(const QString& filename, const persistence::ReviewExportSnapshot& snapshot) {
    const auto& [game, run, opening, moves] = snapshot;
    if (game.source.type == QStringLiteral("file") &&
        QFileInfo(filename) == QFileInfo(QUrl(game.source.uri).toLocalFile())) {
        return message("Choose a different file to keep the source PGN intact.");
    }
    const auto& tree = game.document->tree;
    PgnNotes notes;
    if (run.status != review::JobStatus::Complete) {
        notes.emplace(
            tree.rootId(),
            PgnNote{QStringLiteral("Chess Review: Partial review (%1 of %2 positions analyzed).")
                        .arg(run.completedPositions)
                        .arg(run.totalPositions)
                        .toUtf8(),
                    std::nullopt});
    }
    const auto ids = std::get<std::vector<chess::NodeId>>(tree.mainLine(tree.rootId()));
    for (const auto& move : moves) {
        const auto id = ids.at(static_cast<std::size_t>(move.positionIndex));
        PgnNote note;
        QStringList text;
        if (move.label) {
            // File content is independent of the UI locale.
            constexpr std::array labels{"Best",    "Excellent", "Good",  "Inaccuracy", "Mistake",
                                        "Blunder", "Brilliant", "Great", "Miss",       "Book"};
            static_assert(labels.size() == review::classificationCount);
            const auto label =
                QString::fromLatin1(labels.at(static_cast<std::size_t>(*move.label)));
            text << (move.stable ? label : QStringLiteral("%1 (preliminary)").arg(label));
            if (move.stable) {
                using enum review::Classification;
                switch (*move.label) {
                case Brilliant:
                    note.nag = 3;
                    break;
                case Great:
                    note.nag = 1;
                    break;
                case Inaccuracy:
                    note.nag = 6;
                    break;
                case Mistake:
                    note.nag = 2;
                    break;
                case Blunder:
                    note.nag = 4;
                    break;
                default:
                    break;
                }
            }
        }
        if (!move.suggestedLine.isEmpty()) {
            text << QStringLiteral("Suggested line %1").arg(move.suggestedLine);
        }
        if (move.whiteScore) {
            const auto& score = *move.whiteScore;
            const auto value =
                score.kind == review::ScoreKind::Mate
                    ? QStringLiteral("#%1").arg(score.value)
                    : QString::number(static_cast<double>(score.value) / 100, 'f', 2);
            // Preserve the imported directive and keep the new evaluation readable alongside it.
            if (tree.node(id)->annotations.commentAfter.find("[%eval ") != std::string::npos) {
                text << QStringLiteral("White evaluation %1").arg(value);
            } else {
                text << QStringLiteral("[%eval %1]").arg(value);
            }
        }
        note.comment =
            QStringLiteral("Chess Review: %1").arg(text.join(QStringLiteral(".\n"))).toUtf8();
        notes.emplace(id, std::move(note));
    }
    const auto* match = opening && opening->match ? &*opening->match : nullptr;
    return writePgn(filename, tree, game.document.get(), {}, notes, match);
}
} // namespace chess_review::app
