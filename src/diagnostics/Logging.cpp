#include "diagnostics/Logging.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <cstdio>

namespace OpenChat {
Q_LOGGING_CATEGORY(relayLog, "openchat.relay", QtWarningMsg)
Q_LOGGING_CATEGORY(contactsLog, "openchat.contacts", QtWarningMsg)
Q_LOGGING_CATEGORY(mlsLog, "openchat.mls", QtWarningMsg)

namespace {
QMutex logMutex;
QString logPath;
QtMessageHandler previousHandler = nullptr;
constexpr qint64 maxLogBytes = 1024 * 1024;

void fileMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    // Debug/info only arrive when Qt's category rules enable them. Bound each
    // entry, normalize newlines, and do not record source paths or private data.
    QString safe = message.left(8192);
    safe.replace(u'\r', QStringLiteral("\\r"));
    safe.replace(u'\n', QStringLiteral("\\n"));
    const char *level = type == QtDebugMsg ? "DEBUG" : type == QtInfoMsg ? "INFO"
        : type == QtWarningMsg ? "WARN" : type == QtCriticalMsg ? "ERROR" : "FATAL";
    const QByteArray line = (QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)
        + u' ' + QString::fromLatin1(level) + u' '
        + QString::fromUtf8(context.category ? context.category : "default")
        + QStringLiteral(": ") + safe + u'\n').toUtf8();
    {
        QMutexLocker lock(&logMutex);
        QFile file(logPath);
        if (file.size() + line.size() > maxLogBytes) {
            QFile::remove(logPath + QStringLiteral(".2"));
            QFile::rename(logPath + QStringLiteral(".1"), logPath + QStringLiteral(".2"));
            QFile::rename(logPath, logPath + QStringLiteral(".1"));
        }
        if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
            file.write(line);
            file.flush();
        }
    }
    if (previousHandler)
        previousHandler(type, context, message);
    else
        std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stderr);
}
} // namespace

void installFileLogging()
{
    if (!logPath.isEmpty())
        return;
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (directory.isEmpty() || !QDir().mkpath(directory))
        return;
    logPath = QDir(directory).filePath(QStringLiteral("openchat.log"));
    previousHandler = qInstallMessageHandler(fileMessageHandler);
}
} // namespace OpenChat
