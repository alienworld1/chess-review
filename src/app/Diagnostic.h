#pragma once

#include <QString>

namespace chess_review::diagnostics {
enum class LogFailure;
}

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
Diagnostic loggingDiagnostic(diagnostics::LogFailure failure);

} // namespace chess_review::app
