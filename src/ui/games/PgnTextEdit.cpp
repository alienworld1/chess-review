#include "ui/games/PgnTextEdit.h"
#include "app/ImportController.h"
#include <QMimeData>
#include <QTextCursor>
#include <QTextDocument>
#include <utility>

namespace chess_review::ui {
namespace {
constexpr qsizetype editableBytes = 1024 * 1024;
constexpr qsizetype previewCharacters = 4096;
bool needsPreview(const QString& text) {
    // Bound the conversion too: a UTF-16 unit always requires at least one UTF-8 byte.
    return text.size() > editableBytes || text.toUtf8().size() > editableBytes;
}
} // namespace
PgnTextEdit::PgnTextEdit(QWidget* parent) : QPlainTextEdit(parent) {
    setLineWrapMode(QPlainTextEdit::NoWrap);
    connect(this, &QPlainTextEdit::textChanged, this, [this] {
        edited_ = true;
        original_.clear();
    });
}
void PgnTextEdit::setPgnText(QString text) {
    if (static_cast<quint64>(text.size()) > app::maxBatchBytes || !text.isValidUtf16()) {
        emit inputRejected(
            tr("Clipboard text is too large or contains invalid Unicode. Use a UTF-8 PGN file."));
        return;
    }
    previewOnly_ = needsPreview(text);
    setReadOnly(previewOnly_);
    if (previewOnly_) {
        auto length = previewCharacters;
        if (text.at(length - 1).isHighSurrogate()) {
            --length;
        }
        setPlainText(text.first(length));
    } else {
        setPlainText(text);
    }
    original_ = std::move(text);
    edited_ = false;
    emit previewChanged(previewOnly_);
}
QString PgnTextEdit::pgnText() const {
    if (!edited_) {
        return original_;
    }
    // Qt represents document paragraphs with U+2029. Preserve NBSP and U+2028 within blocks.
    auto text = document()->toRawText();
    text.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    return text;
}
void PgnTextEdit::insertFromMimeData(const QMimeData* source) {
    if (isReadOnly() || !source || !source->hasText()) {
        return;
    }
    const auto text = source->text();
    const auto cursor = textCursor();
    const auto retained =
        document()->characterCount() - 1 - (cursor.selectionEnd() - cursor.selectionStart());
    if (static_cast<quint64>(retained) + static_cast<quint64>(text.size()) > app::maxBatchBytes ||
        !text.isValidUtf16()) {
        emit inputRejected(tr(
            "Paste exceeds the editor limit or contains invalid Unicode. Use a UTF-8 PGN file."));
        return;
    }
    auto combined = document()->toRawText();
    combined.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    combined.replace(cursor.selectionStart(), cursor.selectionEnd() - cursor.selectionStart(),
                     text);
    if (needsPreview(combined)) {
        setPgnText(std::move(combined));
        return;
    }
    insertPlainText(text);
}
} // namespace chess_review::ui
