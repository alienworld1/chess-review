#pragma once

#include "import/PgnInput.h"
#include <QString>
#include <QUrl>
#include <cstddef>
#include <stop_token>

namespace chess_review::input {
// Pure URL/path validation; performs no filesystem or network access.
bool isLocalPgnUrl(const QUrl& url);
// Blocking acquisition: call only on a worker. Errors contain no native/source text.
InputResult readLocalPgn(const QUrl& url, std::size_t byteBudget, std::stop_token stop);
InputResult encodePgnText(const QString& text, std::size_t byteBudget, std::stop_token stop);
} // namespace chess_review::input
