#pragma once

#include "domain/Attachment.h"
#include "domain/ChatTypes.h"
#include "domain/Identifiers.h"
#include "models/Message.h"
#include "models/StagedAttachmentModel.h"
#include "profile/ChatAttachmentImport.h"

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <vector>

namespace OpenChat {

class AttachmentTransfer;
class ChatController;
class ProfileSession;
class SyncEngine;

// The attachment half of ChatController (docs/chat-attachments.md), kept
// apart so the controller stays readable. It owns:
//
// - the tray: what the user picked, prepared by ChatAttachmentImporter in
//   one queue (one video or audio file and two others at a time, on the
//   importer's own low-priority pool), shown by a StagedAttachmentModel;
// - the sends: a send takes every ready card in order, the caption and any
//   reply on the first. A live profile seals each blob into the file store
//   (AttachmentTransfer) before the engine encrypts its message, so a store
//   that cannot take the bytes never moves the ratchet;
// - the live transfers (AttachmentTransfer, made with the engine and gone
//   with it) and the rows they move;
// - the media the bubbles show: previews, and whole blobs read off the GUI
//   thread into a small cache of recently used ones, so scrolling back does
//   not read them again. Nothing is handed out, and everything decrypted is
//   dropped, while the controller withholds plaintext.
//
// In mock mode (no session) the same tray prepares real files, and a send
// keeps the prepared bytes here, by row, so previews and captures show real
// media.
//
// A friend of ChatController: it reads the open chat and its routing there,
// and appends and updates rows as the controller itself would.
class ChatAttachments final : public QObject
{
public:
    // The cache of recently used blobs; Low memory mode lowers it
    // (MemorySettings).
    static constexpr qint64 defaultMediaCacheBudget = 32LL * 1024 * 1024;
    static constexpr qint64 lowMemoryMediaCacheBudget = 8LL * 1024 * 1024;
    static void setMediaCacheBudget(qint64 bytes);
    [[nodiscard]] static qint64 mediaCacheBudget();

    explicit ChatAttachments(ChatController &controller);
    ~ChatAttachments() override;

    ChatAttachments(const ChatAttachments &) = delete;
    ChatAttachments &operator=(const ChatAttachments &) = delete;

    // --- The tray

    [[nodiscard]] StagedAttachmentModel *model() noexcept { return &m_model; }
    [[nodiscard]] bool hasStaged() const noexcept { return !m_staged.empty(); }
    // A card is still being prepared.
    [[nodiscard]] bool busy() const;
    // A card is ready, or will be: a send has something to take.
    [[nodiscard]] bool sendable() const;
    [[nodiscard]] bool sendWhenReady() const noexcept { return m_sendWhenReady; }
    [[nodiscard]] QString notice() const { return m_notice; }
    void setNotice(const QString &notice);

    void attachFiles(const QList<QUrl> &files);
    [[nodiscard]] bool attachClipboard();
    void remove(const QString &id);
    // Every card goes (a live profile replacing the mock, say).
    void clear();
    // The open chat is now `chatId`: the tray belongs to the chat it was
    // filled in, so another one starts empty.
    void chatShown(const QString &chatId);
    void cancelSendWhenReady();
    // While the controller withholds plaintext the tray shows nothing and
    // holds no picture keys, and every decrypted blob is dropped; the cards
    // come back when it shows plaintext again.
    void setWithheld(bool withheld);

    enum class SendOutcome {
        Nothing, // nothing ready could go (the composer keeps its text)
        Waiting, // a card is still being prepared: the send follows (sendWhenReady)
        Sent,    // every ready card went, the caption and reply with the first
    };
    // Sends every ready card, or remembers to once none is being prepared.
    [[nodiscard]] SendOutcome send(const QString &caption, const std::optional<Message> &answered);

    // --- The bubbles

    [[nodiscard]] bool cancel(const QString &stableId);
    [[nodiscard]] bool retry(const QString &stableId);
    [[nodiscard]] bool save(const QString &stableId, const QUrl &target);
    [[nodiscard]] QString suggestedSaveName(const QString &stableId) const;
    [[nodiscard]] bool copyImage(const QString &stableId);
    [[nodiscard]] QByteArray preview(const QString &stableId);
    void loadBlob(const QString &stableId, const QObject *context,
                  std::function<void(const QByteArray &)> done);

    // --- A live profile

    void setLiveServices(ProfileSession &session, SyncEngine &engine);
    // The roster changed: transfers to members who left stop, and anything
    // waiting for a member's device goes.
    void rosterChanged();
    void setCallActive(bool active);

    // --- Rows

    // Fills a row's attachment fields from its descriptor (the name
    // sanitised again, whoever wrote it).
    static void describe(Message &message, const AttachmentDescriptor &descriptor);
    // What stands in for a message where only text fits (a quote, the
    // compose bar, a notification): an attachment's summary ("Photo: …"),
    // anything else's body.
    [[nodiscard]] static QString summaryOf(const Message &message);

    void injectDemo();

private:
    // A card: a file (or pasted picture) queued, being prepared, prepared,
    // or refused.
    struct Staged final {
        QString id;
        QString path;           // a local file; empty for a pasted picture
        QImage picture;         // a pasted picture, until its import starts
        QString name;           // what the card calls it until it is prepared
        AttachmentKind kind = AttachmentKind::File; // what it is prepared as
        qint64 sourceBytes = 0;
        bool started = false;
        bool failed = false;
        QString error;
        qreal progress = 0.0;
        std::unique_ptr<ChatAttachmentImporter> importer;
        std::optional<PreparedAttachment> prepared;
        QString previewKey;     // a PanelMediaLibrary key while the card shows a picture
    };

    // One send's attachments on their way to the engine (live), each sealed
    // into the file store before its message is encrypted.
    struct Batch final {
        ConversationId conversation = ConversationId::generate();
        QList<DeviceId> recipients;
        bool group = false;
        QString caption;
        std::optional<MessageQuote> quote;
        std::deque<PreparedAttachment> items;
        bool captionSent = false;
    };

    // A mock row's bytes, and a demo video still being encoded.
    struct MockMedia final {
        AttachmentDescriptor descriptor;
        QByteArray blob;
        QByteArray preview;
        bool blobPending = false;
    };

    struct Waiter final {
        QPointer<QObject> context;
        std::function<void(const QByteArray &)> done;
    };

    struct CachedBlob final {
        QString stableId;
        QByteArray bytes;
    };

    [[nodiscard]] Staged *find(const QString &id);
    [[nodiscard]] StagedAttachment cardFor(const Staged &staged) const;
    void publish(const Staged &staged);
    void stage(std::unique_ptr<Staged> staged);
    void startNext();
    void start(Staged &staged);
    void onPrepared(const QString &id, const PreparedAttachment &attachment);
    void onFailed(const QString &id, const QString &message);
    void releaseCard(Staged &staged);
    void cardSettled();
    void changed(bool wasSendable);
    [[nodiscard]] bool canSendNow() const;

    // How many devices a send to the open chat reaches.
    [[nodiscard]] int currentRecipientCount() const;
    [[nodiscard]] static bool withinFanOut(qint64 bytes, int recipients);
    // AttachmentTransfer's roster: the devices of `conversation` other than
    // this one, as the controller's roster has them now.
    [[nodiscard]] QList<DeviceId> rosterFor(const ConversationId &conversation) const;

    void sendMock(const QVector<Staged *> &ready, const QString &caption,
                  const std::optional<Message> &answered);
    [[nodiscard]] bool sendLive(const QVector<Staged *> &ready, const QString &caption,
                                const std::optional<Message> &answered);
    void runBatches();
    void batchItemDone(const std::optional<AttachmentDescriptor> &sealed, const QString &error);

    // An attachment row of the open chat, or nothing.
    [[nodiscard]] std::optional<Message> findRow(const QString &stableId) const;
    // Moves a row's transfer on, wherever the controller holds it.
    void applyTransfer(const QString &stableId, AttachmentTransferState state, int reason, int done,
                       int total);
    void onTransferChanged(const MessageId &messageId, int state, int reason, int done, int total);
    void onPreviewArrived(const MessageId &messageId);
    void dropTransfer();

    // Media.
    void deliverBlob(const QString &stableId, const QByteArray &blob);
    void cacheBlob(const QString &stableId, const QByteArray &blob);
    [[nodiscard]] QByteArray cachedBlob(const QString &stableId);
    void removeCached(const QString &stableId);
    void trimCache();
    void dropMedia();
    // `done` gets the complete blob of a photo or file (or nothing) on this
    // thread; used by save and copy.
    void withCompleteBlob(const QString &stableId, std::function<void(const QByteArray &)> done);

    ChatController &m_controller;
    StagedAttachmentModel m_model;
    std::vector<std::unique_ptr<Staged>> m_staged;
    bool m_sendWhenReady = false;
    bool m_sendQueued = false;
    bool m_withheld = false;
    bool m_callActive = false;
    QString m_notice;
    QString m_chatId; // the chat the tray was filled in

    QHash<QString, MockMedia> m_mockMedia;
    QHash<QString, QList<Waiter>> m_waiters;
    std::list<CachedBlob> m_cache; // most recently used first
    qint64 m_cacheBytes = 0;
    QHash<QString, QByteArray> m_previews;
    // Bumped whenever decrypted media is dropped: a read still under way
    // when it was delivers nothing.
    quint64 m_mediaGeneration = 0;

    ProfileSession *m_session = nullptr;
    std::unique_ptr<AttachmentTransfer> m_transfer;
    std::deque<Batch> m_batches;
    bool m_batchRunning = false;
};

} // namespace OpenChat
