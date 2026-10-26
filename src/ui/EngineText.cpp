#include "ui/EngineText.h"
#include <QStringView>

namespace chess_review::ui {
QString presentedEngineText(const QString& text) {
    QString result;
    for (qsizetype index = 0; index < text.size();) {
        const auto character = text[index];
        const bool pair = character.isHighSurrogate() && index + 1 < text.size() &&
                          text[index + 1].isLowSurrogate();
        const auto code = pair ? QChar::surrogateToUcs4(character, text[index + 1])
                               : static_cast<char32_t>(character.unicode());
        const auto category = QChar::category(code);
        if (category == QChar::Other_Control || category == QChar::Other_Format ||
            category == QChar::Other_Surrogate || category == QChar::Separator_Line ||
            category == QChar::Separator_Paragraph) {
            result += (pair ? QStringLiteral("\\U%1") : QStringLiteral("\\u%1"))
                          .arg(static_cast<uint>(code), pair ? 8 : 4, 16, QLatin1Char('0'));
        } else {
            result += QStringView(text).sliced(index, pair ? 2 : 1);
        }
        index += pair ? 2 : 1;
    }
    return result;
}
QString clippedEngineText(const QString& text) {
    if (text.size() <= 256) {
        return text;
    }
    const qsizetype length = text.at(255).isHighSurrogate() ? 255 : 256;
    return text.left(length) + QChar(0x2026);
}
} // namespace chess_review::ui
