#include "app/AnalysisPgn.h"
#include <QCoreApplication>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringDecoder>
#include <QStringList>
#include <algorithm>
#include <array>

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
class Movetext {
  public:
    Movetext(const chess::GameTree& tree, const AnalysisDrawings& drawings, QByteArray& bytes)
        : tree_(tree), drawings_(drawings), bytes_(bytes) {}

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
    chess::NodeAnnotations annotations(const chess::GameTreeNode& node) const {
        auto notes = node.annotations;
        if (const auto found = drawings_.find(node.id); found != drawings_.end()) {
            notes.commentBefore = analysisComment(notes.commentBefore).toStdString();
            notes.commentAfter =
                analysisCommentWithDrawings(analysisComment(notes.commentAfter), found->second);
        }
        return notes;
    }
    bool line(std::span<const chess::NodeId> siblings, std::uint32_t number, chess::Color side,
              std::size_t depth) {
        std::vector<chess::NodeId> continuation;
        while (!siblings.empty()) {
            if (depth > pgn::PgnLimits{}.maxVariationDepth ||
                static_cast<std::size_t>(bytes_.size()) > pgn::PgnLimits{}.maxGameBytes) {
                return false;
            }
            auto node = *tree_.node(siblings.front());
            const auto notes = annotations(node);
            token(QByteArray::number(number) + (side == chess::Color::White ? "." : "..."));
            if (!comment(notes.commentBefore)) {
                return false;
            }
            token(QByteArray::fromStdString(*node.san));
            for (const auto nag : notes.nags) {
                token("$" + QByteArray::number(nag));
            }
            if (!comment(notes.commentAfter)) {
                return false;
            }
            for (const auto alternate : siblings.subspan(1)) {
                token("(");
                if (!line(std::span(&alternate, 1), number, side, depth + 1)) {
                    return false;
                }
                token(")");
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

QString writeAnalysisPgn(const QString& filename, const chess::GameTree& tree,
                         const pgn::PgnDocument* source, const AnalysisDrawings& drawings) {
    const auto writeError = [] { return message("The analysis PGN could not be saved."); };
    const auto limitError = [] {
        return message("The analysis is too large to save as a reopenable PGN.");
    };
    std::vector<chess::Move> mainline;
    const auto ids = std::get<std::vector<chess::NodeId>>(tree.mainLine(tree.rootId()));
    for (const auto id : ids) {
        if (const auto move = tree.node(id)->move) {
            mainline.push_back(*move);
        }
    }
    const bool original = source && std::ranges::equal(mainline, source->originalMainline());
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
    tag(pgn::analysisCopyTag, "1");
    if (context.provenance == chess::RootProvenance::FenAnalysisRoot) {
        tag("SetUp", "1");
        tag("FEN", QByteArray::fromStdString(context.rootFen));
    }
    if (source) {
        for (const auto& extra : source->tags) {
            if (std::ranges::find(roster, extra.name) != roster.end() || extra.name == "FEN" ||
                extra.name == "SetUp" || extra.name == "PlyCount" ||
                extra.name == pgn::analysisCopyTag ||
                (!original && (extra.name == "Termination" || extra.name == "ECO" ||
                               extra.name == "Opening" || extra.name == "Variation"))) {
                continue;
            }
            tag(QByteArray::fromStdString(extra.name), QByteArray::fromStdString(extra.value));
        }
    }
    bytes += '\n';
    Movetext text(tree, drawings, bytes);
    const auto rootNode = *tree.node(tree.rootId());
    const auto rootNotes = text.annotations(rootNode);
    if (!text.comment(rootNotes.commentBefore, pgn::rootCommentPrefix) ||
        !text.comment(rootNotes.commentAfter, pgn::rootCommentPrefix) ||
        !text.line(rootNode.children, root.fullmoveNumber(), root.sideToMove(), 0)) {
        return limitError();
    }
    text.token(termination);
    text.newline();
    if (static_cast<std::size_t>(bytes.size()) > pgn::PgnLimits{}.maxGameBytes) {
        return limitError();
    }
    // Check the same reader used by Games before replacing the destination.
    pgn::PgnReader reader(
        std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
    if (!std::holds_alternative<pgn::PgnDocument>(reader.next())) {
        return message("The analysis could not be represented as a reopenable PGN.");
    }
    QSaveFile file(filename);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        return writeError();
    }
    return {};
}
} // namespace chess_review::app
