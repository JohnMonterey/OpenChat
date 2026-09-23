#pragma once

#include <QObject>
#include <QString>

namespace OpenChat {

// Backs the window that shows a crash report: the plain-language summary a
// tester can read at a glance, the full text they can copy and send, and the
// way back into the application.
//
// The summary is read out of the report's own leading lines ("What happened:",
// "Doing:", "Where:", "Hint:"), which every report the crash handlers write
// starts with, so what the window says is exactly what the file says.
class CrashReportController final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString headline READ headline CONSTANT)
    Q_PROPERTY(QString whatHappened READ whatHappened CONSTANT)
    Q_PROPERTY(QString doing READ doing CONSTANT)
    Q_PROPERTY(QString where READ where CONSTANT)
    Q_PROPERTY(QString hint READ hint CONSTANT)
    Q_PROPERTY(QString reportText READ reportText CONSTANT)
    Q_PROPERTY(QString reportPath READ reportPath CONSTANT)
    // True in the viewer launched straight after a crash, where OpenChat is
    // not running and can be started again from here.
    Q_PROPERTY(bool canRestart READ canRestart CONSTANT)
    Q_PROPERTY(bool copied READ copied NOTIFY copiedChanged)

public:
    CrashReportController(const QString &reportPath, bool canRestart, QObject *parent = nullptr);

    [[nodiscard]] QString headline() const { return m_headline; }
    [[nodiscard]] QString whatHappened() const { return m_whatHappened; }
    [[nodiscard]] QString doing() const { return m_doing; }
    [[nodiscard]] QString where() const { return m_where; }
    [[nodiscard]] QString hint() const { return m_hint; }
    [[nodiscard]] QString reportText() const { return m_reportText; }
    [[nodiscard]] QString reportPath() const { return m_reportPath; }
    [[nodiscard]] bool canRestart() const noexcept { return m_canRestart; }
    [[nodiscard]] bool copied() const noexcept { return m_copied; }

    Q_INVOKABLE void copyReport();
    Q_INVOKABLE void openReportFolder();
    Q_INVOKABLE void restartOpenChat();
    Q_INVOKABLE void dismiss();

signals:
    void copiedChanged();
    // The window should close.
    void finished();

private:
    QString m_reportPath;
    QString m_reportText;
    QString m_headline;
    QString m_whatHappened;
    QString m_doing;
    QString m_where;
    QString m_hint;
    bool m_canRestart = false;
    bool m_copied = false;
    // finished() is raised once, however the window ends up closing.
    bool m_finished = false;
};

} // namespace OpenChat
