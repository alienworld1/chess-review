#pragma once

#include "persistence/GameRepository.h"
#include <QString>
#include <QUrl>
#include <cstddef>
#include <stop_token>
#include <string>

namespace chess_review::input {
struct InputResult {
    std::string bytes;
    persistence::GameSource source;
    QString error;
    std::size_t bytesRead = 0;
    bool cancelled = false;
};
// Blocking acquisition: call only on a worker. Errors contain no native/source text.
InputResult readLocalPgn(const QUrl& url, std::size_t byteBudget, std::stop_token stop);
InputResult encodePgnText(const QString& text, std::size_t byteBudget, std::stop_token stop);
} // namespace chess_review::input
