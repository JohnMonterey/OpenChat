#pragma once

#include "domain/Attachment.h"

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

namespace OpenChat {

// A chat attachment ready to send: its blob in the kind's format
// (domain/Attachment.h), the preview JPEG to send after it (Image and Video;
// may be empty) and the descriptor filled in except for the attachment id and
// key, which the sender mints when it sends. `notice` is a sentence for the
// card when something about the file changed on the way ("Only the first
// minute will be sent.", "Couldn't prepare this as a photo — it will be sent as
// a file.").
struct PreparedAttachment final {
    AttachmentDescriptor descriptor;
    QByteArray blob;
    QByteArray preview;
    QString notice;
};

// The photo file types this computer can read, as lower-case suffixes in the
// order they are offered: jpg, jpeg, png, then webp, bmp, tif and tiff where
// Qt has an image plugin for them (WebP and TIFF need qtimageformats).
[[nodiscard]] QStringList photoSuffixes();
// The same for a person: "JPG, PNG" and ", WebP" where it can be read.
[[nodiscard]] QString photoFormatsHint();

// What an attachment is taken for, from its name alone: photoSuffixes() →
// Image; mp4, m4v, mov, webm, mkv, avi, wmv → Video; wav, mp3, m4a, aac, ogg,
// oga, opus, flac → Audio; anything else (GIF and HEIC included, and a WebP
// or TIFF this computer cannot read) → File.
[[nodiscard]] AttachmentKind guessAttachmentKind(const QString &path);

// Turns one local file (or a pasted picture) into a PreparedAttachment, off
// the GUI thread except for what Qt Multimedia needs on it. Photos are
// re-encoded as baseline JPEG (long side at most maxImageLongSide, within
// maxImageBytes), videos as a 480 px VP9/Opus sequence of up to a minute,
// audio as one Opus song of up to five minutes (milder loudness handling than
// a profile song, and short or quiet clips allowed), files are read as they
// are. A photo, video or audio file that cannot be converted but fits
// maxFileBytes is prepared as a File instead, with a notice. One job per
// importer; the chat controller queues several and runs them in turn.
class ChatAttachmentImporter final : public QObject
{
    Q_OBJECT

public:
    explicit ChatAttachmentImporter(QObject *parent = nullptr);
    ~ChatAttachmentImporter() override;

    ChatAttachmentImporter(const ChatAttachmentImporter &) = delete;
    ChatAttachmentImporter &operator=(const ChatAttachmentImporter &) = delete;

    // `path` is a local file. The kind is guessAttachmentKind(path).
    void start(const QString &path);
    // A picture from the clipboard; `name` becomes its file name (".jpg" added).
    void startImage(const QImage &image, const QString &name);
    void cancel();
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] AttachmentKind kind() const noexcept;

    // Whether this build can prepare videos (it has libvpx).
    [[nodiscard]] static bool videoSupported();

signals:
    // 0…1, at most about ten times a second.
    void progressChanged(qreal progress);
    void finished(const OpenChat::PreparedAttachment &attachment);
    // A sentence for the card ("Files up to 16 MB can be sent.").
    void failed(const QString &message);

private:
    class Private;
    std::unique_ptr<Private> d;
};

} // namespace OpenChat

Q_DECLARE_METATYPE(OpenChat::PreparedAttachment)
