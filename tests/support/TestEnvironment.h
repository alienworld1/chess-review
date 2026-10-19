#pragma once

#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QtLogging>

// Construct before the Qt application can cache paths. Destroy after it and all
// workers; restore the original directory before deleting the temporary root.
class TestEnvironment final {
  public:
    TestEnvironment() : previousDirectory_(QDir::currentPath()) {
        if (!directory_.isValid() || !QDir::setCurrent(directory_.path())) {
            qCritical("Cannot create isolated test working directory");
            return;
        }
#ifdef Q_OS_LINUX
        if (!qputenv("HOME", QFile::encodeName(directory_.path())) ||
            !qputenv("XDG_DATA_HOME",
                     QFile::encodeName(directory_.filePath(QStringLiteral("data")))) ||
            !qputenv("XDG_CONFIG_HOME",
                     QFile::encodeName(directory_.filePath(QStringLiteral("config")))) ||
            !qputenv("XDG_CACHE_HOME",
                     QFile::encodeName(directory_.filePath(QStringLiteral("cache"))))) {
            qCritical("Cannot isolate test storage environment");
            return;
        }
#endif
        valid_ = true;
    }

    ~TestEnvironment() {
        if (!QDir::setCurrent(previousDirectory_)) {
            qFatal("Cannot restore test working directory");
        }
    }

    bool isValid() const { return valid_; }

    TestEnvironment(const TestEnvironment&) = delete;
    TestEnvironment& operator=(const TestEnvironment&) = delete;

  private:
    QTemporaryDir directory_;
    QString previousDirectory_;
    bool valid_ = false;
};
