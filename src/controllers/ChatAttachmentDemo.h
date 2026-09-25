#pragma once

#include "domain/Attachment.h"

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>

// Sample media for --attachment-demo (ChatController::injectDemoAttachmentsForCapture),
// drawn and encoded in-process so a capture shows a real photo, clip and song
// without a file on disk. Never used by a live profile.
namespace OpenChat::ChatAttachmentDemo {

// A landscape at dusk (or at night): sky, sun, three ranges of hills and a
// lake. `phase` (0…1) moves the sun, so frames of a clip differ.
[[nodiscard]] QImage landscape(QSize size, qreal phase, bool night = false);
// A preview the way the importer makes one: long side 320, a baseline JPEG
// within maxPreviewBytes; empty when none fits.
[[nodiscard]] QByteArray preview(const QImage &image);
// The descriptor of a sample blob, as the importer fills it (a zero key: it
// never travels).
[[nodiscard]] AttachmentDescriptor descriptor(AttachmentKind kind, const QByteArray &blob,
                                              const QString &fileName, const QString &mimeType);

// A few seconds of a soft arpeggio as a chat song, and its waveform.
struct Song final {
    QByteArray container;
    qint64 durationMs = 0;
    QByteArray peaks;
};
[[nodiscard]] Song song();

// Four seconds of the landscape, the sun crossing it, as a one-segment video
// sequence; empty without libvpx. Slow: call it on a worker.
[[nodiscard]] QByteArray video(QSize size);

} // namespace OpenChat::ChatAttachmentDemo
