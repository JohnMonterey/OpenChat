#include "controllers/CrashReportController.h"

#include "diagnostics/CrashReporter.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QProcess>
#include <QStringList>
#include <QUrl>

namespace OpenChat {

namespace {

// Reports are text the handlers wrote; anything far larger than one is not.
constexpr qint64 maxReportBytes = 4 * 1024 * 1024;

// `summary` starts at the report's first "Key: value" line; the block ends at
// the first blank line.
[[nodiscard]] QString valueAfter(const QStringList &summary, const QString &key)
{
    for (const QString &line : summary) {
        if (line.trimmed().isEmpty())
            break;
        if (line.startsWith(key))
            return line.mid(key.size()).trimmed();
    }
    return {};
}

} // namespace

CrashReportController::CrashReportController(const QString &reportPath, bool canRestart,
                                             QObject *parent)
    : QObject(parent)
    , m_reportPath(reportPath)
    , m_canRestart(canRestart)
{
    QFile file(reportPath);
    if (file.open(QIODevice::ReadOnly))
        m_reportText = QString::fromUtf8(file.read(maxReportBytes));
    if (m_reportText.isEmpty()) {
        m_headline = QStringLiteral("OpenChat closed unexpectedly");
        m_whatHappened = QStringLiteral("A crash report should have been saved at %1, but it "
                                        "could not be read.")
                             .arg(reportPath);
        return;
    }
    const QStringList lines = m_reportText.split(QLatin1Char('\n'));
    const QString title = lines.value(0).trimmed();
    if (title == QStringLiteral("OpenChat stopped responding"))
        m_headline = QStringLiteral("OpenChat stopped responding");
    else if (title == QStringLiteral("OpenChat closed unexpectedly"))
        m_headline = QStringLiteral("OpenChat closed unexpectedly");
    else
        m_headline = QStringLiteral("OpenChat crashed");
    // Skip the title and its underline; the summary block follows.
    const QStringList summary = lines.mid(3);
    m_whatHappened = valueAfter(summary, QStringLiteral("What happened:"));
    m_doing = valueAfter(summary, QStringLiteral("Doing:"));
    m_where = valueAfter(summary, QStringLiteral("Where:"));
    m_hint = valueAfter(summary, QStringLiteral("Hint:"));
}

void CrashReportController::copyReport()
{
    if (QClipboard *clipboard = QGuiApplication::clipboard())
        clipboard->setText(m_reportText);
    if (!m_copied) {
        m_copied = true;
        emit copiedChanged();
    }
}

void CrashReportController::openReportFolder()
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_reportPath).absolutePath()));
}

void CrashReportController::restartOpenChat()
{
    CrashReporter::markSeen(m_reportPath);
    QProcess::startDetached(QCoreApplication::applicationFilePath(), {});
    dismiss();
}

void CrashReportController::dismiss()
{
    CrashReporter::markSeen(m_reportPath);
    if (m_finished)
        return;
    m_finished = true;
    emit finished();
}

} // namespace OpenChat
