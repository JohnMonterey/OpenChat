#pragma once

#include "core/Result.h"
#include "domain/ProfilePageCodec.h"
#include "render/ProfileImage.h"

#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QString>

#include <functional>
#include <memory>

namespace OpenChat {

// A profile background picture on its way from the owner's file to the
// PageMedia blob contacts receive (ARCH §5.1). Unlike a profile picture it is
// never cropped: the aspect is kept, it is never enlarged, transparency is
// composited onto the page's base colour, and it is re-encoded as a baseline
// JPEG without metadata small enough for one message.
struct ProfileBackgroundLimits final {
    qint64 maxFileBytes = 25LL * 1024 * 1024;
    int minSide = 32;
    int maxSide = 12'000;
    int maxLongSide = Profile::maxBackgroundDimension; // 1920
    qsizetype maxOutputBytes = maxBackgroundImageBytes; // 224 KiB
};

struct ProcessedBackground final {
    QByteArray jpeg;
    QSize size;
};

using ProgressFn = std::function<void(qreal)>; // 0…1, called from the worker
using CancelFn = std::function<bool()>;

// Tries the long side {min(1920, source), 1600, 1280, 1024, 800, 640} (never
// larger than the source) at quality {82, 74, 66, 58, 50, 42} each, and keeps
// the first encoding within maxOutputBytes. A cancelled run stops between
// steps and reports EncodeFailed; the importer never shows that result.
[[nodiscard]] Result<ProcessedBackground, ProfileImageError>
processProfileBackgroundFile(const QString &path, const QColor &matte, const ProfileBackgroundLimits &limits = {},
                             ProgressFn progress = {}, CancelFn cancelled = {});
[[nodiscard]] Result<ProcessedBackground, ProfileImageError>
processProfileBackground(const QImage &image, const QColor &matte, const ProfileBackgroundLimits &limits = {},
                         ProgressFn progress = {}, CancelFn cancelled = {});
// The editor's sentence for an error (profileImageErrorText speaks of profile
// pictures and their 16 px minimum).
[[nodiscard]] QString profileBackgroundErrorText(ProfileImageError error);

// Runs processProfileBackgroundFile on QThreadPool::globalInstance() and
// reports on the thread that owns it. A new start() cancels the previous run,
// whose result is discarded; so is the result of a cancel()led one.
class ProfileBackgroundImporter final : public QObject
{
    Q_OBJECT

public:
    explicit ProfileBackgroundImporter(QObject *parent = nullptr);
    ~ProfileBackgroundImporter() override;

    void start(const QString &path, const QColor &matte);
    void cancel();
    [[nodiscard]] bool busy() const noexcept { return m_job != nullptr; }
    [[nodiscard]] qreal progress() const noexcept { return m_progress; }

signals:
    void progressChanged(qreal progress);
    void finished(const QByteArray &jpeg, QSize size);
    void failed(OpenChat::ProfileImageError error, const QString &message);

private:
    struct Job;

    void setProgress(qreal progress);
    void detach();

    std::shared_ptr<Job> m_job;
    qreal m_progress = 0;
};

} // namespace OpenChat
