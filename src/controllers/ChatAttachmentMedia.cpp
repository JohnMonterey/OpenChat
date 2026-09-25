#include "controllers/ChatAttachmentMedia.h"

#include "controllers/ChatController.h"
#include "domain/Attachment.h"
#include "profile/ProfilePanelMedia.h"
#include "profile/SongPlayer.h"

namespace OpenChat {

namespace {

// The stored transfer state of an attachment whose bytes are all here.
constexpr int readyState = 1;

} // namespace

ChatAttachmentMedia::ChatAttachmentMedia(QObject *parent)
    : QObject(parent)
{
}

ChatAttachmentMedia::~ChatAttachmentMedia()
{
    releaseAll();
}

QObject *ChatAttachmentMedia::controller() const
{
    return m_controller.data();
}

void ChatAttachmentMedia::setController(QObject *controller)
{
    auto *chat = qobject_cast<ChatController *>(controller);
    if (m_controller == chat)
        return;
    if (m_controller)
        disconnect(m_controller, nullptr, this, nullptr);
    releaseAll();
    m_controller = chat;
    if (chat != nullptr) {
        // Withholding plaintext drops everything; showing it again brings the
        // preview (and whatever is wanted) back.
        connect(chat, &ChatController::sessionStateChanged, this, &ChatAttachmentMedia::refresh);
        connect(chat, &QObject::destroyed, this, [this] {
            releaseAll();
            emit changed();
        });
    }
    emit controllerChanged();
    refresh();
    emit changed();
}

void ChatAttachmentMedia::setStableId(const QString &stableId)
{
    if (m_stableId == stableId)
        return;
    // Every key belongs to the attachment it was put for.
    releaseAll();
    m_stableId = stableId;
    emit stableIdChanged();
    refresh();
    emit changed();
}

void ChatAttachmentMedia::setTransferState(int state)
{
    if (m_transferState == state)
        return;
    m_transferState = state;
    // A new state may make the bytes readable (or not any more).
    m_loadFailed = false;
    if (m_transferState != readyState)
        releaseFull();
    emit transferStateChanged();
    refresh();
    emit changed();
}

void ChatAttachmentMedia::setPreviewRevision(int revision)
{
    if (m_previewRevision == revision)
        return;
    m_previewRevision = revision;
    emit previewRevisionChanged();
    refresh();
}

void ChatAttachmentMedia::setWantFull(bool want)
{
    if (m_wantFull == want)
        return;
    m_wantFull = want;
    emit wantFullChanged();
    refresh();
    emit changed();
}

void ChatAttachmentMedia::setWantSegments(bool want)
{
    if (m_wantSegments == want)
        return;
    m_wantSegments = want;
    emit wantSegmentsChanged();
    refresh();
    emit changed();
}

void ChatAttachmentMedia::setWantSong(bool want)
{
    if (m_wantSong == want)
        return;
    m_wantSong = want;
    emit wantSongChanged();
    refresh();
    emit changed();
}

bool ChatAttachmentMedia::ready() const
{
    if (m_transferState != readyState || m_blob.isEmpty())
        return false;
    if (m_wantFull && m_imageKey.isEmpty())
        return false;
    if (m_wantSegments && m_segmentKeys.isEmpty())
        return false;
    if (m_wantSong && m_songKey.isEmpty())
        return false;
    return m_wantFull || m_wantSegments || m_wantSong;
}

void ChatAttachmentMedia::classBegin()
{
    m_complete = false;
}

void ChatAttachmentMedia::componentComplete()
{
    m_complete = true;
    refresh();
    emit changed();
}

bool ChatAttachmentMedia::wantsBlob() const
{
    return m_transferState == readyState && (m_wantFull || m_wantSegments || m_wantSong);
}

QString ChatAttachmentMedia::keyFor(const QString &part) const
{
    return QStringLiteral("chat:") + m_stableId + u':' + part;
}

void ChatAttachmentMedia::refresh()
{
    if (!m_complete)
        return;
    ChatController *controller = m_controller.data();
    if (controller == nullptr || m_stableId.isEmpty() || !controller->plaintextVisible()) {
        const bool held = !m_previewKey.isEmpty() || !m_blob.isEmpty() || m_loading;
        releaseAll();
        if (held)
            emit changed();
        return;
    }
    bool moved = false;
    // The preview from the start, and fetched again when one arrives later.
    if (m_previewKey.isEmpty() && m_previewFetchedRevision != m_previewRevision) {
        m_previewFetchedRevision = m_previewRevision;
        const QByteArray preview = controller->attachmentPreview(m_stableId);
        if (!preview.isEmpty()) {
            m_previewKey = keyFor(QStringLiteral("preview"));
            PanelMediaLibrary::instance().put(m_previewKey, preview);
            moved = true;
        }
    }
    if (!wantsBlob()) {
        if (!m_blob.isEmpty() || m_loading) {
            releaseFull();
            moved = true;
        }
    } else if (!m_blob.isEmpty()) {
        applyBlob();
        moved = true;
    } else if (!m_loading && !m_loadFailed) {
        m_loading = true;
        moved = true;
        const quint64 generation = ++m_generation;
        const QPointer<ChatAttachmentMedia> self(this);
        controller->loadAttachmentBlob(m_stableId, this, [self, generation](const QByteArray &blob) {
            if (!self || self->m_generation != generation)
                return;
            self->m_loading = false;
            if (blob.isEmpty())
                self->m_loadFailed = true;
            else
                self->m_blob = blob;
            self->refresh();
            emit self->changed();
        });
    }
    if (moved)
        emit changed();
}

void ChatAttachmentMedia::applyBlob()
{
    PanelMediaLibrary &library = PanelMediaLibrary::instance();
    if (m_wantFull && m_imageKey.isEmpty()) {
        m_imageKey = keyFor(QStringLiteral("image"));
        library.put(m_imageKey, m_blob);
    } else if (!m_wantFull && !m_imageKey.isEmpty()) {
        library.release(std::exchange(m_imageKey, QString()));
    }
    if (m_wantSegments && m_segmentKeys.isEmpty()) {
        // Each segment was checked when the attachment arrived; this only
        // splits them out. Not a video: nothing to play.
        if (const auto segments = decodeVideoSequence(m_blob)) {
            for (qsizetype index = 0; index < segments->size(); ++index) {
                const QString key = keyFor(QStringLiteral("segment:%1").arg(index));
                library.put(key, segments->at(index));
                m_segmentKeys.append(key);
            }
        }
    } else if (!m_wantSegments && !m_segmentKeys.isEmpty()) {
        for (const QString &key : std::as_const(m_segmentKeys))
            library.release(key);
        m_segmentKeys.clear();
    }
    if (m_wantSong && m_songKey.isEmpty()) {
        m_songKey = keyFor(QStringLiteral("song"));
        SongLibrary::instance().put(m_songKey, m_blob);
    } else if (!m_wantSong && !m_songKey.isEmpty()) {
        SongLibrary::instance().release(std::exchange(m_songKey, QString()));
    }
}

void ChatAttachmentMedia::releasePreview()
{
    if (!m_previewKey.isEmpty())
        PanelMediaLibrary::instance().release(std::exchange(m_previewKey, QString()));
    m_previewFetchedRevision = -1;
}

void ChatAttachmentMedia::releaseFull()
{
    PanelMediaLibrary &library = PanelMediaLibrary::instance();
    if (!m_imageKey.isEmpty())
        library.release(std::exchange(m_imageKey, QString()));
    for (const QString &key : std::as_const(m_segmentKeys))
        library.release(key);
    m_segmentKeys.clear();
    if (!m_songKey.isEmpty())
        SongLibrary::instance().release(std::exchange(m_songKey, QString()));
    m_blob.clear();
    m_loading = false;
    // A read still under way is for what was just dropped.
    ++m_generation;
}

void ChatAttachmentMedia::releaseAll()
{
    releasePreview();
    releaseFull();
    m_loadFailed = false;
}

} // namespace OpenChat
