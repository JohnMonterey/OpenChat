#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QQmlParserStatus>
#include <QString>
#include <QStringList>

namespace OpenChat {

class ChatController;

// What one attachment bubble (or the media viewer) shows, as keys into the
// stores the painted items read (docs/chat-attachments.md): the preview
// (PanelMediaLibrary) from the start, and the whole photo, the video's
// segments or the song (SongLibrary) only once the bytes are all here and
// the bubble asks for them (wantFull, wantSegments, wantSong). The keys
// follow stableId, transferState and those wishes: a photo that finishes
// arriving while it is on screen gets its full picture without anything
// being rebuilt. Blobs are read off the GUI thread by the controller, which
// keeps the recently used ones.
//
// Everything it put is released when it goes, when the wishes change, and
// whenever the controller withholds plaintext (every key is empty then).
// Safe with no controller: every key stays empty.
class ChatAttachmentMedia : public QObject, public QQmlParserStatus
{
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)
    Q_PROPERTY(QObject *controller READ controller WRITE setController NOTIFY controllerChanged)
    Q_PROPERTY(QString stableId READ stableId WRITE setStableId NOTIFY stableIdChanged)
    Q_PROPERTY(int transferState READ transferState WRITE setTransferState NOTIFY transferStateChanged)
    Q_PROPERTY(int previewRevision READ previewRevision WRITE setPreviewRevision NOTIFY previewRevisionChanged)
    Q_PROPERTY(bool wantFull READ wantFull WRITE setWantFull NOTIFY wantFullChanged)
    Q_PROPERTY(bool wantSegments READ wantSegments WRITE setWantSegments NOTIFY wantSegmentsChanged)
    Q_PROPERTY(bool wantSong READ wantSong WRITE setWantSong NOTIFY wantSongChanged)
    Q_PROPERTY(QString previewKey READ previewKey NOTIFY changed)
    Q_PROPERTY(QString imageKey READ imageKey NOTIFY changed)
    Q_PROPERTY(QStringList segmentKeys READ segmentKeys NOTIFY changed)
    Q_PROPERTY(QString songKey READ songKey NOTIFY changed)
    // The media asked for is loaded (its keys are set).
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    Q_PROPERTY(bool loading READ loading NOTIFY changed)

public:
    explicit ChatAttachmentMedia(QObject *parent = nullptr);
    ~ChatAttachmentMedia() override;

    [[nodiscard]] QObject *controller() const;
    void setController(QObject *controller);
    [[nodiscard]] QString stableId() const { return m_stableId; }
    void setStableId(const QString &stableId);
    [[nodiscard]] int transferState() const noexcept { return m_transferState; }
    void setTransferState(int state);
    [[nodiscard]] int previewRevision() const noexcept { return m_previewRevision; }
    void setPreviewRevision(int revision);
    [[nodiscard]] bool wantFull() const noexcept { return m_wantFull; }
    void setWantFull(bool want);
    [[nodiscard]] bool wantSegments() const noexcept { return m_wantSegments; }
    void setWantSegments(bool want);
    [[nodiscard]] bool wantSong() const noexcept { return m_wantSong; }
    void setWantSong(bool want);

    [[nodiscard]] QString previewKey() const { return m_previewKey; }
    [[nodiscard]] QString imageKey() const { return m_imageKey; }
    [[nodiscard]] QStringList segmentKeys() const { return m_segmentKeys; }
    [[nodiscard]] QString songKey() const { return m_songKey; }
    [[nodiscard]] bool ready() const;
    [[nodiscard]] bool loading() const noexcept { return m_loading; }

    void classBegin() override;
    void componentComplete() override;

signals:
    void controllerChanged();
    void stableIdChanged();
    void transferStateChanged();
    void previewRevisionChanged();
    void wantFullChanged();
    void wantSegmentsChanged();
    void wantSongChanged();
    void changed();

private:
    // Brings the keys in line with the inputs.
    void refresh();
    void releasePreview();
    void releaseFull();
    void releaseAll();
    // Puts what the wishes ask for out of the loaded blob.
    void applyBlob();
    [[nodiscard]] bool wantsBlob() const;
    [[nodiscard]] QString keyFor(const QString &part) const;

    QPointer<ChatController> m_controller;
    QString m_stableId;
    int m_transferState = 0;
    int m_previewRevision = 0;
    bool m_wantFull = false;
    bool m_wantSegments = false;
    bool m_wantSong = false;
    // Not before QML has set every property it was given.
    bool m_complete = true;

    QString m_previewKey;
    int m_previewFetchedRevision = -1;
    QString m_imageKey;
    QStringList m_segmentKeys;
    QString m_songKey;
    QByteArray m_blob;       // the whole attachment, while something of it is wanted
    bool m_loading = false;
    bool m_loadFailed = false;
    // Bumped whenever what was asked for is dropped: an older read lands nowhere.
    quint64 m_generation = 0;
};

} // namespace OpenChat
