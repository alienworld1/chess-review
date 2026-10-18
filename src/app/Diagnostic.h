#pragma once

#include <QString>

namespace chess_review::persistence {
struct DatabaseError;
}

namespace chess_review::app {

struct Diagnostic {
    QString code;
    QString summary;
    QString nextSteps;
    QString sensitiveDetails;
    QString safeContext;

    QString safeReport() const;
};

Diagnostic storageDiagnostic(const persistence::DatabaseError& error);

} // namespace chess_review::app
