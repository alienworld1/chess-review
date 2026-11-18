#include "persistence/private/PuzzleCodec.h"
#include "analysis/ReviewEvidenceAdapter.h"
#include "engine/AnalysisCache.h"
#include "engine/EffectiveSearch.h"
#include "persistence/private/AnalysisCodec.h"
#include "persistence/private/ReviewCodec.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <set>
#include <utility>

namespace chess_review::persistence {
std::variant<puzzles::Verification, ReviewError> verifyPuzzle(const PuzzleDocument& document) {
    const auto invalid = [] {
        return ReviewError{
            ReviewErrorCode::InvalidRecord,
            QStringLiteral("Puzzle verification evidence is invalid or incompatible.")};
    };
    auto restored = engine::restorePosition(document.definition.position);
    const auto* root = std::get_if<chess::Position>(&restored);
    if (!root || !engine::validFingerprint(document.fingerprint) || document.engineProfileId <= 0 ||
        document.passes.size() > 192 || (document.gameId && *document.gameId <= 0) ||
        (document.reviewId && (!document.gameId || !document.sourcePosition ||
                               *document.reviewId <= 0 || *document.sourcePosition < 1)) ||
        (document.definition.origin == puzzles::Origin::Generated && !document.reviewId)) {
        return invalid();
    }
    std::vector<review::PositionEvidence> passes;
    std::set<QUuid> attempts;
    auto result = puzzles::verify(*root, document.definition, passes);
    for (const auto& raw : document.passes) {
        if (result.outcome != puzzles::Outcome::Pending || !result.next ||
            !raw.evidence.provenance || !attempts.insert(raw.evidence.request).second) {
            return invalid();
        }
        const auto& need = *result.next;
        const auto& snapshot = raw.evidence;
        const auto* nodes = std::get_if<engine::NodeLimit>(&snapshot.limit);
        if (!nodes || nodes->nodes != need.nodes || !snapshot.wdlRequested ||
            snapshot.requestedMultiPv != need.width ||
            snapshot.provenance->position != need.position.context() ||
            !engine::reviewWidthCompatible(document.fingerprint, snapshot.provenance->fingerprint,
                                           snapshot.effectiveMultiPv)) {
            return invalid();
        }
        auto converted = analysis::reviewEvidence(raw, &need.position);
        auto* record = std::get_if<analysis::ReviewEvidenceRecord>(&converted);
        if (!record) {
            return invalid();
        }
        if (!record->evidence.complete) {
            return puzzles::Verification{puzzles::Outcome::Insufficient, {}, {}};
        }
        // Qualification requires both requested and measured strength. Nominal search
        // budgets account for effort; a completed iteration may overshoot a node limit.
        record->evidence.strength.nodes = nodes->nodes;
        passes.push_back(std::move(record->evidence));
        result = puzzles::verify(*root, document.definition, passes);
    }
    return result;
}
} // namespace chess_review::persistence

namespace chess_review::persistence::detail {
namespace {
QJsonValue number(std::uint64_t n) { return QString::number(n); }
std::optional<std::uint64_t> natural(const QJsonValue& value, std::uint64_t maximum) {
    if (!value.isString()) {
        return {};
    }
    bool ok = false;
    const auto text = value.toString();
    const auto n = text.toULongLong(&ok);
    if (!ok || n > maximum || QString::number(n) != text) {
        return {};
    }
    return n;
}
QJsonObject policy(const puzzles::Policy& p) {
    const auto& c = p.review;
    QJsonArray budgets;
    for (const auto& b : c.budgets) {
        budgets.append(QJsonArray{number(b.quickScanNodes), number(b.deepeningNodes),
                                  number(b.verificationNodes)});
    }
    const auto& s = c.special;
    return {
        {"baseline", number(p.baselineNodes)},
        {"rootMinimum", number(p.rootMinimumNodes)},
        {"playerMoves", p.maximumPlayerMoves},
        {"positions", p.maximumPositions},
        {"budgetUnits", p.maximumBudgetUnits},
        {"materialGain", p.minimumMaterialGain},
        {"immediateGain", p.immediateMaterialGain},
        {"quality", static_cast<int>(c.quality)},
        {"budgets", budgets},
        {"classification",
         QJsonArray{c.classification.inaccuracyLoss, c.classification.mistakeLoss,
                    c.classification.blunderLoss, c.classification.excellentLoss}},
        {"conversion", QJsonArray{c.conversion.fallbackScaleCp, c.conversion.preferWdl}},
        {"refinement",
         QJsonArray{c.refinement.boundaryMargin, c.refinement.maximumDrift, c.refinement.multiPv}},
        {"special", QJsonArray{s.maximumLoss, s.maximumLossDrift, s.alternativeGap, s.missLoss,
                               s.viableScore, s.alreadyWinningScore, s.margin, s.sacrificeValue}}};
}
std::optional<puzzles::Policy> readPolicy(const QJsonObject& o) {
    const auto baseline = natural(o["baseline"], 1000000000000ULL);
    const auto root = natural(o["rootMinimum"], 1000000000000ULL);
    if (!baseline || !root) {
        return {};
    }
    puzzles::Policy p;
    p.baselineNodes = *baseline;
    p.rootMinimumNodes = *root;
    p.maximumPlayerMoves = o["playerMoves"].toInt(-1);
    p.maximumPositions = o["positions"].toInt(-1);
    p.maximumBudgetUnits = o["budgetUnits"].toInt(-1);
    p.minimumMaterialGain = o["materialGain"].toInt(-1);
    p.immediateMaterialGain = o["immediateGain"].toInt(-1);
    auto& c = p.review;
    const auto quality = o["quality"].toInt(-1);
    if (quality < 0 || quality > 2) {
        return {};
    }
    c.quality = static_cast<review::Quality>(quality);
    const auto budgets = o["budgets"].toArray();
    if (budgets.size() != 3) {
        return {};
    }
    for (int i = 0; i < 3; ++i) {
        const auto b = budgets[i].toArray();
        if (b.size() != 3) {
            return {};
        }
        const auto quick = natural(b[0], 1000000000000ULL);
        const auto deep = natural(b[1], 1000000000000ULL);
        const auto verified = natural(b[2], 1000000000000ULL);
        if (!quick || !deep || !verified) {
            return {};
        }
        c.budgets[static_cast<std::size_t>(i)] = {*quick, *deep, *verified};
    }
    const auto a = o["classification"].toArray();
    const auto e = o["conversion"].toArray();
    const auto r = o["refinement"].toArray();
    const auto s = o["special"].toArray();
    if (a.size() != 4 || e.size() != 2 || r.size() != 3 || s.size() != 8 || !e[1].isBool()) {
        return {};
    }
    c.classification = {a[0].toDouble(-1), a[1].toDouble(-1), a[2].toDouble(-1), a[3].toDouble(-1)};
    c.conversion = {e[0].toDouble(-1), e[1].toBool()};
    c.refinement = {r[0].toDouble(-1), r[1].toDouble(-1), r[2].toInt(-1)};
    c.special = {s[0].toDouble(-1), s[1].toDouble(-1), s[2].toDouble(-1), s[3].toDouble(-1),
                 s[4].toDouble(-1), s[5].toDouble(-1), s[6].toDouble(-1), s[7].toInt(-1)};
    if (!puzzles::validPolicy(p) ||
        std::holds_alternative<config::Diagnostics>(config::validate(c)) || policy(p) != o) {
        return {};
    }
    return p;
}
QJsonArray moves(std::span<const chess::Move> path) {
    QJsonArray out;
    for (const auto& m : path) {
        out.append(QJsonArray{m.from.index(), m.to.index(),
                              m.promotion ? static_cast<int>(*m.promotion) : -1});
    }
    return out;
}
QJsonObject solution(const puzzles::Solution& s) {
    QJsonArray nodes;
    for (const auto& n : s.nodes) {
        QJsonArray edges;
        for (const auto& edge : n.moves) {
            edges.append(QJsonObject{{"move", moves(std::span(&edge.move, 1))},
                                     {"next", static_cast<int>(edge.next)}});
        }
        nodes.append(QJsonObject{{"path", moves(n.path)},
                                 {"moves", edges},
                                 {"complete", n.complete},
                                 {"idea", static_cast<int>(n.idea)}});
    }
    return {{"goal", static_cast<int>(s.goal)}, {"material", s.materialTarget}, {"nodes", nodes}};
}
QJsonObject documentObject(const PuzzleDocument& d) {
    QJsonArray passes;
    for (const auto& pass : d.passes) {
        const auto encoded = encodeAnalysis(pass);
        const auto* bytes = std::get_if<QByteArray>(&encoded);
        if (!bytes) {
            return {};
        }
        passes.append(QString::fromLatin1(bytes->toBase64()));
    }
    const auto& context = d.definition.position;
    auto root = context.provenance == chess::RootProvenance::InitialPosition
                    ? chess::Result<chess::Position>(chess::Position::initial())
                    : chess::Position::fromFen(context.rootFen);
    auto* position = std::get_if<chess::Position>(&root);
    if (!position) {
        return {};
    }
    QJsonArray history;
    for (const auto& move : context.moves) {
        const auto uci = position->uci(move);
        if (!std::holds_alternative<std::string>(uci) ||
            std::holds_alternative<chess::Error>(position->play(move))) {
            return {};
        }
        history.append(QString::fromStdString(std::get<std::string>(uci)));
    }
    return {{"formatVersion", 1},
            {"modelVersion", static_cast<int>(puzzles::modelVersion)},
            {"origin", static_cast<int>(d.definition.origin)},
            {"initial", context.provenance == chess::RootProvenance::InitialPosition},
            {"rootFen", QString::fromStdString(context.rootFen)},
            {"history", history},
            {"policy", policy(d.definition.policy)},
            {"engine", QString::fromLatin1(encodeFingerprint(d.fingerprint).toBase64())},
            {"profile", number(static_cast<std::uint64_t>(d.engineProfileId))},
            {"game", d.gameId ? number(static_cast<std::uint64_t>(*d.gameId)) : QJsonValue()},
            {"review", d.reviewId ? number(static_cast<std::uint64_t>(*d.reviewId)) : QJsonValue()},
            {"sourcePosition", d.sourcePosition ? QJsonValue(*d.sourcePosition) : QJsonValue()},
            {"passes", passes},
            {"solution", solution(d.solution)}};
}
std::optional<PuzzleDocument> readDocument(const QJsonObject& o) {
    if (o["formatVersion"].toInt(-1) != 1 || o["modelVersion"].toInt(-1) != 1 ||
        !o["initial"].isBool() || !o["history"].isArray() || !o["passes"].isArray() ||
        o["rootFen"].toString().size() > 256 || o["history"].toArray().size() > 20000 ||
        o["passes"].toArray().size() > 192) {
        return {};
    }
    auto root = o["initial"].toBool()
                    ? chess::Result<chess::Position>(chess::Position::initial())
                    : chess::Position::fromFen(o["rootFen"].toString().toStdString());
    auto* position = std::get_if<chess::Position>(&root);
    if (!position) {
        return {};
    }
    for (const auto& item : o["history"].toArray()) {
        if (!item.isString() || item.toString().size() > 5) {
            return {};
        }
        const auto move = position->parseUci(item.toString().toStdString());
        if (!std::holds_alternative<chess::Move>(move) ||
            std::holds_alternative<chess::Error>(position->play(std::get<chess::Move>(move)))) {
            return {};
        }
    }
    const auto p = readPolicy(o["policy"].toObject());
    const auto engine =
        decodeFingerprint(QByteArray::fromBase64(o["engine"].toString().toLatin1()));
    const auto profile = natural(o["profile"], 9223372036854775807ULL);
    const int origin = o["origin"].toInt(-1);
    if (!p || !engine || !profile || *profile == 0 || origin < 0 || origin > 1) {
        return {};
    }
    PuzzleDocument d;
    d.definition = {position->context(), static_cast<puzzles::Origin>(origin), *p};
    d.fingerprint = *engine;
    d.engineProfileId = static_cast<std::int64_t>(*profile);
    for (const auto& [key, target] :
         {std::pair{"game", &d.gameId}, std::pair{"review", &d.reviewId}}) {
        if (!o[key].isNull()) {
            const auto n = natural(o[key], 9223372036854775807ULL);
            if (!n || *n == 0) {
                return {};
            }
            *target = static_cast<std::int64_t>(*n);
        }
    }
    if (!o["sourcePosition"].isNull()) {
        const int index = o["sourcePosition"].toInt(-1);
        if (index < 1 || index > 20000) {
            return {};
        }
        d.sourcePosition = index;
    }
    for (const auto& item : o["passes"].toArray()) {
        if (!item.isString()) {
            return {};
        }
        const auto raw = decodeAnalysis(QByteArray::fromBase64(item.toString().toLatin1()));
        if (!raw) {
            return {};
        }
        d.passes.push_back(*raw);
    }
    const auto verified = verifyPuzzle(d);
    const auto* result = std::get_if<puzzles::Verification>(&verified);
    if (!result) {
        return {};
    }
    // Incomplete checkpoints carry no publicly usable solution.
    if (result->outcome == puzzles::Outcome::Accepted) {
        d.solution = result->solution;
    }
    if (documentObject(d) != o) {
        return {};
    }
    return d;
}
} // namespace
QByteArray puzzleSolution(const puzzles::Solution& s) {
    return QJsonDocument(solution(s)).toJson(QJsonDocument::Compact);
}
QByteArray encodePuzzle(const PuzzleDocument& d) {
    if (std::holds_alternative<config::Diagnostics>(config::validate(d.definition.policy.review))) {
        return {};
    }
    const auto verified = verifyPuzzle(d);
    const auto* result = std::get_if<puzzles::Verification>(&verified);
    if (!result ||
        (result->outcome == puzzles::Outcome::Accepted ? d.solution != result->solution
                                                       : d.solution != puzzles::Solution{})) {
        return {};
    }
    const auto object = documentObject(d);
    const auto bytes = reviewDocument(object);
    return !object.isEmpty() && bytes.size() <= maxPuzzleBytes ? bytes : QByteArray{};
}
std::optional<PuzzleDocument> decodePuzzle(const QByteArray& bytes) {
    const auto o = readReviewDocument(bytes, maxPuzzleBytes);
    return o ? readDocument(*o) : std::nullopt;
}
QByteArray encodePuzzleJob(const PuzzleJob& job) {
    if (job.candidates.size() > 6 || job.accepted.size() > 2 || job.current < 0 ||
        job.current > static_cast<int>(job.candidates.size()) || job.revision < 0 ||
        static_cast<int>(job.status) < 0 || static_cast<int>(job.status) > 4) {
        return {};
    }
    QJsonArray candidates, accepted;
    for (const auto& c : job.candidates) {
        const auto bytes = encodePuzzle(c);
        if (bytes.isEmpty() || c.reviewId != job.reviewId) {
            return {};
        }
        candidates.append(QString::fromLatin1(bytes.toBase64()));
    }
    for (const auto id : job.accepted) {
        if (id <= 0) {
            return {};
        }
        accepted.append(number(static_cast<std::uint64_t>(id)));
    }
    const auto bytes = reviewDocument({{"formatVersion", 1},
                                       {"reusedExisting", job.reusedExisting},
                                       {"candidates", candidates},
                                       {"accepted", accepted},
                                       {"current", job.current}});
    return bytes.size() <= maxPuzzleJobBytes ? bytes : QByteArray{};
}
std::optional<PuzzleJob> decodePuzzleJob(const QByteArray& bytes) {
    const auto o = readReviewDocument(bytes, maxPuzzleJobBytes);
    if (!o || (*o)["formatVersion"].toInt(-1) != 1 || !(*o)["reusedExisting"].isBool() ||
        !(*o)["candidates"].isArray() || !(*o)["accepted"].isArray() ||
        (*o)["candidates"].toArray().size() > 6 || (*o)["accepted"].toArray().size() > 2) {
        return {};
    }
    PuzzleJob job;
    job.reusedExisting = (*o)["reusedExisting"].toBool();
    job.current = (*o)["current"].toInt(-1);
    for (const auto& item : (*o)["candidates"].toArray()) {
        const auto d = decodePuzzle(QByteArray::fromBase64(item.toString().toLatin1()));
        if (!d || (!job.candidates.empty() && d->reviewId != job.reviewId)) {
            return {};
        }
        job.reviewId = d->reviewId;
        job.candidates.push_back(*d);
    }
    for (const auto& item : (*o)["accepted"].toArray()) {
        const auto n = natural(item, 9223372036854775807ULL);
        if (!n || *n == 0) {
            return {};
        }
        job.accepted.push_back(static_cast<PuzzleId>(*n));
    }
    if (job.current < 0 || job.current > static_cast<int>(job.candidates.size())) {
        return {};
    }
    return job;
}
} // namespace chess_review::persistence::detail
