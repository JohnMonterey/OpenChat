#include "controllers/ChatAttachments.h"
#include "diagnostics/Logging.h"

#include "app/AttachmentTransfer.h"
#include "app/ProfileSession.h"
#include "controllers/ChatAttachmentDemo.h"
#include "controllers/ChatController.h"
#include "domain/MessageContent.h"
#include "network/SyncEngine.h"
#include "profile/ClipCodec.h"
#include "profile/ProfileBackgroundImage.h"
#include "profile/ProfilePanelMedia.h"
#include "storage/SqlCipherAttachmentRepository.h"

#include <QBuffer>
#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QMetaObject>
#include <QMimeData>
#include <QSaveFile>
#include <QThreadPool>
#include <QUuid>

#include <algorithm>

#if defined(Q_OS_MACOS)
#include <sys/xattr.h>
#endif

namespace OpenChat {

namespace {

// The import queue: one video or audio file at a time (each wants the media
// decoders and hundreds of megabytes at its peak), and two photos or plain
// files beside it.
constexpr int maxHeavyImports = 1;
constexpr int maxLightImports = 2;
// Previews read from the store are kept while the chat shows them; past
// this many, the oldest go (they are small, and read again on demand).
constexpr int maxCachedPreviews = 256;

qint64 s_mediaCacheBudget = ChatAttachments::defaultMediaCacheBudget;

QList<ChatAttachments *> &liveInstances()
{
    static QList<ChatAttachments *> instances; // GUI thread only
    return instances;
}

[[nodiscard]] bool isHeavy(AttachmentKind kind)
{
    return kind == AttachmentKind::Video || kind == AttachmentKind::Audio;
}

[[nodiscard]] AttachmentKind kindOf(int attachmentKind)
{
    return attachmentKind >= 1 && attachmentKind <= 3 ? AttachmentKind(attachmentKind) : AttachmentKind::File;
}

[[nodiscard]] QString newId()
{
    return QUuid::createUuid().toString(QUuid::Id128);
}

[[nodiscard]] std::optional<MessageId> messageIdOf(const QString &stableId)
{
    return MessageId::fromBytes(QByteArray::fromHex(stableId.toLatin1()));
}

[[nodiscard]] AttachmentTransferState transferStateOf(int state)
{
    switch (state) {
    case int(AttachmentState::Complete):
        return AttachmentTransferState::Ready;
    case int(AttachmentState::Failed):
        return AttachmentTransferState::Failed;
    case int(AttachmentState::Cancelled):
        return AttachmentTransferState::Cancelled;
    default:
        return AttachmentTransferState::Transferring;
    }
}

[[nodiscard]] QString refusalText(AttachmentSendRefusal refusal)
{
    switch (refusal) {
    case AttachmentSendRefusal::None:
        break;
    case AttachmentSendRefusal::NoRecipients:
        return QStringLiteral("There is nobody to send this to.");
    case AttachmentSendRefusal::TooLarge:
        return QStringLiteral("That caption is too long to send with an attachment.");
    case AttachmentSendRefusal::FailedClosed:
    case AttachmentSendRefusal::Invalid:
    case AttachmentSendRefusal::Duplicate:
    case AttachmentSendRefusal::StoreError:
        return QStringLiteral("The attachment couldn't be sent.");
    }
    return {};
}

const QString tooManyText = QStringLiteral("Up to 10 attachments can be sent at once.");
const QString fanOutText = QStringLiteral("Too large to send to this many people at once.");
const QString saveFailedText = QStringLiteral("The file couldn't be saved.");

// Marks a saved file as downloaded, as a browser would, so the system asks
// before it is opened: the Zone.Identifier stream on Windows, the quarantine
// attribute on macOS. Linux has no such mark.
void markAsDownloaded(const QString &path)
{
#if defined(Q_OS_WIN)
    QFile zone(path + QStringLiteral(":Zone.Identifier"));
    if (zone.open(QIODevice::WriteOnly | QIODevice::Truncate))
        zone.write("[ZoneTransfer]\r\nZoneId=3\r\n");
#elif defined(Q_OS_MACOS)
    const QByteArray value = QByteArrayLiteral("0081;")
                             + QByteArray::number(QDateTime::currentSecsSinceEpoch(), 16)
                             + QByteArrayLiteral(";OpenChat;");
    (void)setxattr(QFile::encodeName(path).constData(), "com.apple.quarantine", value.constData(),
                   size_t(value.size()), 0, 0);
#else
    Q_UNUSED(path)
#endif
}

} // namespace

// ---------------------------------------------------------------------------

void ChatAttachments::setMediaCacheBudget(qint64 bytes)
{
    s_mediaCacheBudget = std::max<qint64>(0, bytes);
    for (ChatAttachments *attachments : liveInstances())
        attachments->trimCache();
}

qint64 ChatAttachments::mediaCacheBudget()
{
    return s_mediaCacheBudget;
}

ChatAttachments::ChatAttachments(ChatController &controller)
    : m_controller(controller)
{
    liveInstances().append(this);
}

ChatAttachments::~ChatAttachments()
{
    liveInstances().removeAll(this);
    // The importers first: nothing they still report may land here. Reads
    // under way answer nobody: the handles waiting on them hear their
    // controller go.
    for (auto &staged : m_staged)
        releaseCard(*staged);
    m_staged.clear();
    m_waiters.clear();
    dropTransfer();
}

// ---------------------------------------------------------------------------
// The tray
// ---------------------------------------------------------------------------

bool ChatAttachments::busy() const
{
    return std::any_of(m_staged.cbegin(), m_staged.cend(), [](const auto &staged) {
        return !staged->failed && !staged->prepared;
    });
}

bool ChatAttachments::sendable() const
{
    return std::any_of(m_staged.cbegin(), m_staged.cend(), [](const auto &staged) { return !staged->failed; });
}

void ChatAttachments::setNotice(const QString &notice)
{
    if (m_notice == notice)
        return;
    m_notice = notice;
    emit m_controller.attachmentNoticeChanged();
}

ChatAttachments::Staged *ChatAttachments::find(const QString &id)
{
    for (auto &staged : m_staged) {
        if (staged->id == id)
            return staged.get();
    }
    return nullptr;
}

StagedAttachment ChatAttachments::cardFor(const Staged &staged) const
{
    StagedAttachment card;
    card.id = staged.id;
    card.kind = int(staged.prepared ? staged.prepared->descriptor.kind : staged.kind);
    card.name = staged.prepared && !staged.prepared->descriptor.fileName.isEmpty()
                    ? staged.prepared->descriptor.fileName
                    : staged.name;
    // A file goes as it is, so its size is known at once; anything else is
    // converted, and its size is what comes out.
    card.byteCount = staged.prepared ? staged.prepared->descriptor.byteCount
                     : staged.kind == AttachmentKind::File ? staged.sourceBytes
                                                            : 0;
    card.progress = staged.progress;
    card.ready = staged.prepared.has_value() && !staged.failed;
    card.failed = staged.failed;
    card.error = staged.error;
    card.notice = staged.prepared ? staged.prepared->notice : QString();
    card.previewKey = m_withheld ? QString() : staged.previewKey;
    card.durationMs = staged.prepared ? staged.prepared->descriptor.durationMs : 0;
    return card;
}

void ChatAttachments::publish(const Staged &staged)
{
    if (!m_withheld)
        m_model.update(cardFor(staged));
}

void ChatAttachments::changed(bool wasSendable)
{
    emit m_controller.stagedAttachmentsChanged();
    m_controller.updateCanSend(wasSendable);
}

void ChatAttachments::stage(std::unique_ptr<Staged> staged)
{
    if (!m_withheld)
        m_model.append(cardFor(*staged));
    m_staged.push_back(std::move(staged));
}

void ChatAttachments::attachFiles(const QList<QUrl> &files)
{
    if (files.isEmpty() || m_withheld || m_controller.m_currentContactId.isEmpty()
        || !m_controller.m_editingMessageId.isEmpty())
        return;
    const bool wasSendable = m_controller.canSend();
    int remote = 0;
    int folders = 0;
    int tooMany = 0;
    int tooLarge = 0;
    for (const QUrl &url : files) {
        if (!url.isLocalFile()) {
            ++remote;
            continue;
        }
        const QString path = url.toLocalFile();
        const QFileInfo info(path);
        if (info.isDir()) {
            ++folders;
            continue;
        }
        if (int(m_staged.size()) >= AttachmentLimits::maxStaged) {
            ++tooMany;
            continue;
        }
        const AttachmentKind kind = guessAttachmentKind(path);
        // A file goes as it is: one too large is refused here, before any
        // of it is read. Anything else is converted, and may come out small.
        if (kind == AttachmentKind::File && info.size() > AttachmentLimits::maxFileBytes) {
            ++tooLarge;
            continue;
        }
        auto staged = std::make_unique<Staged>();
        staged->id = newId();
        staged->path = path;
        staged->name = info.fileName();
        staged->kind = kind;
        staged->sourceBytes = info.size();
        stage(std::move(staged));
    }
    if (tooMany > 0)
        setNotice(tooManyText);
    else if (tooLarge > 0)
        setNotice(QStringLiteral("Files up to 16 MB can be sent."));
    else if (remote > 0)
        setNotice(QStringLiteral("Only files on this computer can be attached."));
    else if (folders > 0)
        setNotice(QStringLiteral("Folders can't be attached."));
    else
        setNotice({});
    startNext();
    changed(wasSendable);
}

bool ChatAttachments::attachClipboard()
{
    if (m_withheld || m_controller.m_currentContactId.isEmpty()
        || !m_controller.m_editingMessageId.isEmpty())
        return false;
    // Only a GUI application has a clipboard.
    if (qobject_cast<QGuiApplication *>(QCoreApplication::instance()) == nullptr)
        return false;
    const QClipboard *clipboard = QGuiApplication::clipboard();
    const QMimeData *mime = clipboard != nullptr ? clipboard->mimeData() : nullptr;
    if (mime == nullptr)
        return false;
    if (mime->hasUrls()) {
        QList<QUrl> local;
        for (const QUrl &url : mime->urls()) {
            if (url.isLocalFile())
                local.append(url);
        }
        if (!local.isEmpty()) {
            attachFiles(local);
            return true;
        }
    }
    // Text wins: copied cells, slides and documents often carry a picture of
    // themselves beside their text, and the text is what was meant. A
    // picture alone (a screenshot, "Copy image") is attached.
    if (!mime->hasImage() || (mime->hasText() && !mime->text().trimmed().isEmpty()))
        return false;
    const QImage picture = qvariant_cast<QImage>(mime->imageData());
    if (picture.isNull())
        return false;
    const bool wasSendable = m_controller.canSend();
    if (int(m_staged.size()) >= AttachmentLimits::maxStaged) {
        setNotice(tooManyText);
        return true;
    }
    auto staged = std::make_unique<Staged>();
    staged->id = newId();
    staged->picture = picture;
    staged->name = QStringLiteral("Pasted picture.jpg");
    staged->kind = AttachmentKind::Image;
    stage(std::move(staged));
    setNotice({});
    startNext();
    changed(wasSendable);
    return true;
}

void ChatAttachments::startNext()
{
    int heavy = 0;
    int light = 0;
    for (const auto &staged : m_staged) {
        if (staged->importer)
            ++(isHeavy(staged->kind) ? heavy : light);
    }
    for (auto &staged : m_staged) {
        if (staged->started)
            continue;
        int &running = isHeavy(staged->kind) ? heavy : light;
        if (running >= (isHeavy(staged->kind) ? maxHeavyImports : maxLightImports))
            continue;
        ++running;
        start(*staged);
    }
}

void ChatAttachments::start(Staged &staged)
{
    staged.started = true;
    staged.importer = std::make_unique<ChatAttachmentImporter>();
    ChatAttachmentImporter *importer = staged.importer.get();
    const QString id = staged.id;
    connect(importer, &ChatAttachmentImporter::progressChanged, this, [this, id](qreal progress) {
        if (Staged *current = find(id)) {
            current->progress = progress;
            if (!m_withheld)
                m_model.setProgress(id, progress);
        }
    });
    connect(importer, &ChatAttachmentImporter::finished, this,
            [this, id](const PreparedAttachment &attachment) { onPrepared(id, attachment); });
    connect(importer, &ChatAttachmentImporter::failed, this,
            [this, id](const QString &message) { onFailed(id, message); });
    if (staged.path.isEmpty()) {
        const QImage picture = std::exchange(staged.picture, QImage());
        importer->startImage(picture, QFileInfo(staged.name).completeBaseName());
    } else {
        importer->start(staged.path);
    }
}

void ChatAttachments::onPrepared(const QString &id, const PreparedAttachment &attachment)
{
    Staged *staged = find(id);
    if (staged == nullptr)
        return;
    const bool wasSendable = m_controller.canSend();
    if (staged->importer) {
        staged->importer->disconnect(this);
        staged->importer.release()->deleteLater();
    }
    staged->prepared = attachment;
    staged->kind = attachment.descriptor.kind;
    staged->progress = 1.0;
    if (!attachment.preview.isEmpty()) {
        staged->previewKey = QStringLiteral("staged:") + id;
        if (!m_withheld)
            PanelMediaLibrary::instance().put(staged->previewKey, attachment.preview);
    }
    if (!withinFanOut(attachment.descriptor.byteCount, currentRecipientCount())) {
        staged->failed = true;
        staged->error = fanOutText;
    }
    publish(*staged);
    startNext();
    changed(wasSendable);
    cardSettled();
}

void ChatAttachments::onFailed(const QString &id, const QString &message)
{
    Staged *staged = find(id);
    if (staged == nullptr)
        return;
    const bool wasSendable = m_controller.canSend();
    if (staged->importer) {
        staged->importer->disconnect(this);
        staged->importer.release()->deleteLater();
    }
    staged->failed = true;
    staged->error = message;
    publish(*staged);
    startNext();
    changed(wasSendable);
    cardSettled();
}

// A card finished preparing either way: a send waiting for the tray goes now
// if nothing else is still being prepared. Never on the importer's stack.
void ChatAttachments::cardSettled()
{
    if (!m_sendWhenReady || busy() || m_sendQueued)
        return;
    m_sendQueued = true;
    QMetaObject::invokeMethod(
        this,
        [this] {
            m_sendQueued = false;
            if (!m_sendWhenReady || busy())
                return;
            if (!m_controller.sendMessage())
                cancelSendWhenReady();
        },
        Qt::QueuedConnection);
}

void ChatAttachments::releaseCard(Staged &staged)
{
    if (staged.importer) {
        staged.importer->disconnect(this);
        staged.importer->cancel();
        staged.importer.release()->deleteLater();
    }
    if (!staged.previewKey.isEmpty() && !m_withheld)
        PanelMediaLibrary::instance().release(staged.previewKey);
    staged.previewKey.clear();
}

void ChatAttachments::remove(const QString &id)
{
    const auto it = std::find_if(m_staged.begin(), m_staged.end(),
                                 [&](const auto &staged) { return staged->id == id; });
    if (it == m_staged.end())
        return;
    const bool wasSendable = m_controller.canSend();
    releaseCard(**it);
    m_staged.erase(it);
    m_model.remove(id);
    // Removing a card changes what a waiting send would send.
    m_sendWhenReady = false;
    startNext();
    changed(wasSendable);
}

void ChatAttachments::clear()
{
    const bool wasSendable = m_controller.canSend();
    const bool had = !m_staged.empty() || m_sendWhenReady;
    for (auto &staged : m_staged)
        releaseCard(*staged);
    m_staged.clear();
    m_model.clear();
    m_sendWhenReady = false;
    setNotice({});
    if (had)
        changed(wasSendable);
}

void ChatAttachments::chatShown(const QString &chatId)
{
    if (m_chatId == chatId)
        return;
    m_chatId = chatId;
    clear();
}

void ChatAttachments::cancelSendWhenReady()
{
    if (!m_sendWhenReady)
        return;
    const bool wasSendable = m_controller.canSend();
    m_sendWhenReady = false;
    changed(wasSendable);
}

void ChatAttachments::setWithheld(bool withheld)
{
    if (m_withheld == withheld)
        return;
    const bool wasSendable = m_controller.canSend();
    if (withheld) {
        for (auto &staged : m_staged) {
            if (!staged->previewKey.isEmpty())
                PanelMediaLibrary::instance().release(staged->previewKey);
        }
        m_withheld = true;
        m_model.clear();
        m_sendWhenReady = false;
        setNotice({});
        dropMedia();
    } else {
        m_withheld = false;
        for (auto &staged : m_staged) {
            if (!staged->previewKey.isEmpty() && staged->prepared)
                PanelMediaLibrary::instance().put(staged->previewKey, staged->prepared->preview);
            m_model.append(cardFor(*staged));
        }
    }
    changed(wasSendable);
}

int ChatAttachments::currentRecipientCount() const
{
    if (const auto *group = m_controller.currentGroup())
        return int(group->members.size());
    if (!m_controller.m_live)
        return 1;
    const auto chat = m_controller.m_liveChats.constFind(m_controller.m_currentContactId);
    return chat != m_controller.m_liveChats.cend() && chat->peerDevice ? 1 : 0;
}

bool ChatAttachments::withinFanOut(qint64 bytes, int recipients)
{
    return recipients <= 0 || bytes <= AttachmentLimits::maxFanOutBytes / recipients;
}

QList<DeviceId> ChatAttachments::rosterFor(const ConversationId &conversation) const
{
    QList<DeviceId> devices;
    const QString id = m_controller.contactForConversation(conversation);
    if (const auto group = m_controller.m_liveGroups.constFind(id); group != m_controller.m_liveGroups.cend()) {
        for (const auto &member : group->members)
            devices.append(member.device);
        return devices;
    }
    if (const auto chat = m_controller.m_liveChats.constFind(id);
        chat != m_controller.m_liveChats.cend() && chat->peerDevice)
        devices.append(*chat->peerDevice);
    return devices;
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

ChatAttachments::SendOutcome ChatAttachments::send(const QString &caption,
                                                   const std::optional<Message> &answered)
{
    const bool wasSendable = m_controller.canSend();
    if (busy()) {
        if (!m_sendWhenReady) {
            m_sendWhenReady = true;
            changed(wasSendable);
        }
        return SendOutcome::Waiting;
    }
    QVector<Staged *> ready;
    const int recipients = currentRecipientCount();
    for (auto &staged : m_staged) {
        if (!staged->prepared || staged->failed)
            continue;
        // The group may have grown since the card was ready.
        if (!withinFanOut(staged->prepared->descriptor.byteCount, recipients)) {
            staged->failed = true;
            staged->error = fanOutText;
            publish(*staged);
            continue;
        }
        ready.append(staged.get());
    }
    m_sendWhenReady = false;
    if (ready.isEmpty()) {
        changed(wasSendable);
        return SendOutcome::Nothing;
    }
    if (m_controller.m_live) {
        if (!sendLive(ready, caption, answered)) {
            changed(wasSendable);
            return SendOutcome::Nothing;
        }
    } else {
        sendMock(ready, caption, answered);
    }
    // The cards that went leave the tray; a refused one stays to say why.
    for (Staged *staged : std::as_const(ready)) {
        const QString id = staged->id;
        releaseCard(*staged);
        m_model.remove(id);
        m_staged.erase(std::remove_if(m_staged.begin(), m_staged.end(),
                                      [&](const auto &candidate) { return candidate->id == id; }),
                       m_staged.end());
    }
    setNotice({});
    changed(wasSendable);
    return SendOutcome::Sent;
}

void ChatAttachments::sendMock(const QVector<Staged *> &ready, const QString &caption,
                               const std::optional<Message> &answered)
{
    const QDateTime sentAt = QDateTime::currentDateTime();
    bool first = true;
    for (Staged *staged : ready) {
        const PreparedAttachment &prepared = *staged->prepared;
        Message message{MessageDirection::Outgoing, first ? caption : QString(), sentAt.time(),
                        MessageKind::Attachment, sentAt.date()};
        message.stableId = newId();
        message.sharedId = true;
        describe(message, prepared.descriptor);
        message.hasPreview = !prepared.preview.isEmpty();
        message.transferState = AttachmentTransferState::Ready;
        message.transferDone = message.transferTotal;
        if (first && answered) {
            message.replyToId = answered->stableId;
            message.quotedSender = m_controller.authorName(*answered);
            message.quotedBody = quoteExcerpt(summaryOf(*answered));
        }
        m_mockMedia.insert(message.stableId,
                           MockMedia{prepared.descriptor, prepared.blob, prepared.preview, false});
        m_controller.m_messages.appendMessage(message);
        m_controller.m_messagesByContact[m_controller.m_currentContactId].append(message);
        first = false;
    }
    m_controller.m_contacts.setActivity(m_controller.m_currentContactId, sentAt.toMSecsSinceEpoch(), 0);
}

bool ChatAttachments::sendLive(const QVector<Staged *> &ready, const QString &caption,
                               const std::optional<Message> &answered)
{
    if (!m_transfer || m_controller.m_engine == nullptr) {
        setNotice(QStringLiteral("Attachments can't be sent right now."));
        return false;
    }
    Batch batch;
    if (const auto *group = m_controller.currentGroup()) {
        batch.conversation = group->conversation;
        batch.group = true;
        for (const auto &member : group->members)
            batch.recipients.append(member.device);
    } else if (const auto chat = m_controller.m_liveChats.constFind(m_controller.m_currentContactId);
               chat != m_controller.m_liveChats.cend() && chat->peerDevice) {
        batch.conversation = chat->conversation;
        batch.recipients.append(*chat->peerDevice);
    }
    if (batch.recipients.isEmpty())
        return false;
    batch.caption = caption;
    batch.quote = answered ? m_controller.quoteFor(*answered) : std::nullopt;
    for (Staged *staged : ready)
        batch.items.push_back(*staged->prepared);
    m_batches.push_back(std::move(batch));
    runBatches();
    return true;
}

// One attachment at a time, in the order they were sent: sealed, then handed
// to the engine, so their messages keep that order.
void ChatAttachments::runBatches()
{
    while (!m_batchRunning && !m_batches.empty()) {
        Batch &batch = m_batches.front();
        if (batch.items.empty()) {
            // A caption none of its attachments could carry is given back,
            // when the composer is free to take it.
            if (!batch.captionSent && !batch.caption.isEmpty()
                && m_controller.m_composerText.trimmed().isEmpty() && m_controller.m_editingMessageId.isEmpty())
                m_controller.setComposerText(batch.caption);
            m_batches.pop_front();
            continue;
        }
        if (!m_transfer)
            return;
        m_batchRunning = true;
        PreparedAttachment item = std::move(batch.items.front());
        batch.items.pop_front();
        OutgoingAttachment outgoing{std::move(item.descriptor), std::move(item.blob), std::move(item.preview)};
        const QPointer<ChatAttachments> guard(this);
        m_transfer->stageOutgoing(batch.conversation, std::move(outgoing),
                                  [guard](AttachmentTransfer::Staged staged) {
                                      if (!guard)
                                          return;
                                      if (const auto *sealed = std::get_if<AttachmentDescriptor>(&staged))
                                          guard->batchItemDone(*sealed, {});
                                      else
                                          guard->batchItemDone(std::nullopt, std::get<QString>(staged));
                                  });
    }
}

void ChatAttachments::batchItemDone(const std::optional<AttachmentDescriptor> &sealed, const QString &error)
{
    m_batchRunning = false;
    if (m_batches.empty())
        return;
    Batch &batch = m_batches.front();
    if (!sealed) {
        setNotice(error);
    } else if (m_controller.m_engine == nullptr) {
        setNotice(refusalText(AttachmentSendRefusal::FailedClosed));
        if (m_transfer)
            m_transfer->discardStaged(batch.conversation, sealed->attachmentId);
    } else {
        const bool first = !batch.captionSent;
        const AttachmentSendRefusal refusal = m_controller.m_engine->enqueueAttachment(
            batch.conversation, batch.recipients, batch.group, first ? batch.caption : QString(), *sealed,
            first ? batch.quote : std::nullopt);
        if (refusal == AttachmentSendRefusal::None) {
            batch.captionSent = true;
        } else {
            qCWarning(mediaLog) << "An attachment was refused before sending:" << int(refusal);
            setNotice(refusalText(refusal));
            if (m_transfer)
                m_transfer->discardStaged(batch.conversation, sealed->attachmentId);
        }
    }
    runBatches();
}

// ---------------------------------------------------------------------------
// The bubbles
// ---------------------------------------------------------------------------

std::optional<Message> ChatAttachments::findRow(const QString &stableId) const
{
    // Only the open chat's rows are acted on, and none while plaintext is
    // withheld (the model is empty then).
    const std::optional<Message> row = m_controller.m_messages.messageById(stableId);
    if (!row || row->kind != MessageKind::Attachment)
        return std::nullopt;
    return row;
}

void ChatAttachments::applyTransfer(const QString &stableId, AttachmentTransferState state, int reason,
                                    int done, int total)
{
    for (auto &history : m_controller.m_messagesByContact) {
        for (Message &message : history) {
            if (message.stableId != stableId)
                continue;
            message.transferState = state;
            message.transferReason = reason;
            message.transferDone = done;
            message.transferTotal = total;
        }
    }
    m_controller.m_messages.updateTransfer(stableId, state, reason, done, total);
}

void ChatAttachments::onTransferChanged(const MessageId &messageId, int state, int reason, int done, int total)
{
    const QString stableId = messageId.toHex();
    applyTransfer(stableId, transferStateOf(state), reason, done, total);
    // A blob read before it was complete is not one to keep.
    if (transferStateOf(state) != AttachmentTransferState::Ready)
        removeCached(stableId);
}

void ChatAttachments::onPreviewArrived(const MessageId &messageId)
{
    const QString stableId = messageId.toHex();
    m_previews.remove(stableId);
    for (auto &history : m_controller.m_messagesByContact) {
        for (Message &message : history) {
            if (message.stableId == stableId) {
                message.hasPreview = true;
                ++message.previewRevision;
            }
        }
    }
    m_controller.m_messages.bumpPreview(stableId);
}

bool ChatAttachments::cancel(const QString &stableId)
{
    const std::optional<Message> row = findRow(stableId);
    if (!row || !row->canCancelTransfer())
        return false;
    if (!m_controller.m_live) {
        applyTransfer(stableId, AttachmentTransferState::Cancelled, int(AttachmentFailure::SenderCancelled),
                      row->transferDone, row->transferTotal);
        return true;
    }
    const auto messageId = messageIdOf(stableId);
    return m_transfer && messageId && m_transfer->cancel(*messageId);
}

bool ChatAttachments::retry(const QString &stableId)
{
    const std::optional<Message> row = findRow(stableId);
    if (!row || !row->canRetryTransfer() || m_withheld || !m_controller.m_editingMessageId.isEmpty())
        return false;
    if (int(m_staged.size()) >= AttachmentLimits::maxStaged) {
        setNotice(tooManyText);
        return false;
    }
    const bool wasSendable = m_controller.canSend();
    auto staged = std::make_unique<Staged>();
    staged->id = newId();
    staged->kind = kindOf(row->attachmentKind);
    staged->name = row->fileName.isEmpty() ? summaryOf(*row) : row->fileName;
    staged->started = true; // nothing to import: the bytes are this device's own
    const QString id = staged->id;
    if (!m_controller.m_live) {
        const auto media = m_mockMedia.constFind(stableId);
        if (media == m_mockMedia.cend() || media->blob.isEmpty())
            return false;
        const PreparedAttachment prepared{media->descriptor, media->blob, media->preview, {}};
        stage(std::move(staged));
        onPrepared(id, prepared);
    } else {
        const auto messageId = messageIdOf(stableId);
        SqlCipherAttachmentRepository *repository = m_session ? m_session->attachments() : nullptr;
        if (!messageId || !m_transfer || repository == nullptr)
            return false;
        const auto stored = repository->descriptorFor(*messageId);
        if (!stored.hasValue() || !stored.value())
            return false;
        // A new attachment: the transfer mints its id and key when it goes.
        AttachmentDescriptor descriptor = stored.value()->descriptor;
        descriptor.key.clear();
        descriptor.attachmentId = AttachmentId::generate();
        const QByteArray preview = m_transfer->previewFor(*messageId);
        stage(std::move(staged));
        const QPointer<ChatAttachments> guard(this);
        m_transfer->loadBlob(*messageId, [guard, id, descriptor, preview](const QByteArray &blob) {
            if (!guard)
                return;
            if (blob.isEmpty())
                guard->onFailed(id, QStringLiteral("This attachment can't be sent again."));
            else
                guard->onPrepared(id, PreparedAttachment{descriptor, blob, preview, {}});
        });
    }
    // The caption comes back too, unless something else is being written.
    if (!row->body.isEmpty() && m_controller.m_composerText.trimmed().isEmpty())
        m_controller.setComposerText(row->body);
    setNotice({});
    changed(wasSendable);
    return true;
}

QString ChatAttachments::suggestedSaveName(const QString &stableId) const
{
    const std::optional<Message> row = findRow(stableId);
    if (!row)
        return {};
    if (row->attachmentKind == int(AttachmentKind::Image)) {
        // A photo is a JPEG whatever it was called.
        const QString base = QFileInfo(row->fileName).completeBaseName();
        return (base.isEmpty() ? QStringLiteral("photo") : base) + QStringLiteral(".jpg");
    }
    return row->fileName.isEmpty() ? QStringLiteral("file") : row->fileName;
}

void ChatAttachments::withCompleteBlob(const QString &stableId, std::function<void(const QByteArray &)> done)
{
    loadBlob(stableId, this, std::move(done));
}

bool ChatAttachments::save(const QString &stableId, const QUrl &target)
{
    const std::optional<Message> row = findRow(stableId);
    if (!row || !row->canSaveAttachment() || !target.isLocalFile())
        return false;
    const QString path = target.toLocalFile();
    if (path.isEmpty() || QFileInfo(path).isDir())
        return false;
    const QPointer<ChatAttachments> guard(this);
    withCompleteBlob(stableId, [guard, path](const QByteArray &blob) {
        if (!guard)
            return;
        if (blob.isEmpty()) {
            guard->setNotice(saveFailedText);
            return;
        }
        // Written off the GUI thread: up to 16 MB, onto any disk.
        QThreadPool::globalInstance()->start([guard, path, blob] {
            QSaveFile file(path);
            bool saved = file.open(QIODevice::WriteOnly) && file.write(blob) == blob.size() && file.commit();
            if (saved)
                markAsDownloaded(path);
            QMetaObject::invokeMethod(
                qApp,
                [guard, saved] {
                    if (guard && !saved)
                        guard->setNotice(saveFailedText);
                },
                Qt::QueuedConnection);
        });
    });
    return true;
}

bool ChatAttachments::copyImage(const QString &stableId)
{
    const std::optional<Message> row = findRow(stableId);
    if (!row || !row->canSaveAttachment() || row->attachmentKind != int(AttachmentKind::Image))
        return false;
    if (qobject_cast<QGuiApplication *>(QCoreApplication::instance()) == nullptr)
        return false;
    const QPointer<ChatAttachments> guard(this);
    withCompleteBlob(stableId, [guard](const QByteArray &blob) {
        if (!guard || blob.isEmpty())
            return;
        // Decoded off the GUI thread; the picture passed its checks on
        // arrival, and the reader is bounded all the same.
        QThreadPool::globalInstance()->start([guard, blob] {
            QBuffer buffer;
            buffer.setData(blob);
            buffer.open(QIODevice::ReadOnly);
            QImageReader reader(&buffer, "jpeg");
            reader.setAllocationLimit(256);
            const QImage picture = reader.read();
            QMetaObject::invokeMethod(
                qApp,
                [guard, picture] {
                    if (!guard || picture.isNull() || guard->m_withheld)
                        return;
                    if (QClipboard *clipboard = QGuiApplication::clipboard())
                        clipboard->setImage(picture);
                },
                Qt::QueuedConnection);
        });
    });
    return true;
}

// ---------------------------------------------------------------------------
// Media for the bubbles
// ---------------------------------------------------------------------------

QByteArray ChatAttachments::preview(const QString &stableId)
{
    if (m_withheld || stableId.isEmpty())
        return {};
    if (!m_controller.m_live) {
        const auto media = m_mockMedia.constFind(stableId);
        return media == m_mockMedia.cend() ? QByteArray() : media->preview;
    }
    if (const auto cached = m_previews.constFind(stableId); cached != m_previews.cend())
        return *cached;
    const auto messageId = messageIdOf(stableId);
    if (!messageId || !m_transfer)
        return {};
    const QByteArray jpeg = m_transfer->previewFor(*messageId);
    if (!jpeg.isEmpty()) {
        if (m_previews.size() >= maxCachedPreviews)
            m_previews.clear();
        m_previews.insert(stableId, jpeg);
    }
    return jpeg;
}

void ChatAttachments::loadBlob(const QString &stableId, const QObject *context,
                               std::function<void(const QByteArray &)> done)
{
    Waiter waiter{QPointer<QObject>(const_cast<QObject *>(context)), std::move(done)};
    const auto post = [this](Waiter posted, const QByteArray &blob) {
        QMetaObject::invokeMethod(
            this,
            [posted = std::move(posted), blob] {
                if (posted.context)
                    posted.done(blob);
            },
            Qt::QueuedConnection);
    };
    if (m_withheld || stableId.isEmpty() || context == nullptr) {
        post(std::move(waiter), {});
        return;
    }
    if (!m_controller.m_live) {
        const auto media = m_mockMedia.constFind(stableId);
        if (media != m_mockMedia.cend() && media->blobPending) {
            m_waiters[stableId].append(std::move(waiter));
            return;
        }
        post(std::move(waiter), media == m_mockMedia.cend() ? QByteArray() : media->blob);
        return;
    }
    if (const QByteArray cached = cachedBlob(stableId); !cached.isEmpty()) {
        post(std::move(waiter), cached);
        return;
    }
    QList<Waiter> &waiters = m_waiters[stableId];
    waiters.append(std::move(waiter));
    if (waiters.size() > 1)
        return; // a read is already under way
    const auto messageId = messageIdOf(stableId);
    if (!messageId || !m_transfer) {
        deliverBlob(stableId, {});
        return;
    }
    const QPointer<ChatAttachments> guard(this);
    const quint64 generation = m_mediaGeneration;
    m_transfer->loadBlob(*messageId, [guard, stableId, generation](const QByteArray &blob) {
        // Plaintext was withheld (and every waiter answered) meanwhile.
        if (!guard || guard->m_mediaGeneration != generation)
            return;
        if (!blob.isEmpty())
            guard->cacheBlob(stableId, blob);
        guard->deliverBlob(stableId, blob);
    });
}

void ChatAttachments::deliverBlob(const QString &stableId, const QByteArray &blob)
{
    const QList<Waiter> waiters = m_waiters.take(stableId);
    for (const Waiter &waiter : waiters) {
        if (waiter.context)
            waiter.done(blob);
    }
}

void ChatAttachments::cacheBlob(const QString &stableId, const QByteArray &blob)
{
    removeCached(stableId);
    if (m_withheld || blob.size() > s_mediaCacheBudget)
        return;
    m_cache.push_front({stableId, blob});
    m_cacheBytes += blob.size();
    trimCache();
}

QByteArray ChatAttachments::cachedBlob(const QString &stableId)
{
    const auto it = std::find_if(m_cache.begin(), m_cache.end(),
                                 [&](const CachedBlob &cached) { return cached.stableId == stableId; });
    if (it == m_cache.end())
        return {};
    // Most recently used first.
    m_cache.splice(m_cache.begin(), m_cache, it);
    return m_cache.front().bytes;
}

void ChatAttachments::removeCached(const QString &stableId)
{
    const auto it = std::find_if(m_cache.begin(), m_cache.end(),
                                 [&](const CachedBlob &cached) { return cached.stableId == stableId; });
    if (it == m_cache.end())
        return;
    m_cacheBytes -= it->bytes.size();
    m_cache.erase(it);
}

void ChatAttachments::trimCache()
{
    while (!m_cache.empty() && m_cacheBytes > s_mediaCacheBudget) {
        m_cacheBytes -= m_cache.back().bytes.size();
        m_cache.pop_back();
    }
}

void ChatAttachments::dropMedia()
{
    ++m_mediaGeneration;
    m_cache.clear();
    m_cacheBytes = 0;
    m_previews.clear();
    // Whoever waits hears there is nothing (and is told to stop waiting).
    const QStringList pending = m_waiters.keys();
    for (const QString &stableId : pending)
        deliverBlob(stableId, {});
}

// ---------------------------------------------------------------------------
// A live profile
// ---------------------------------------------------------------------------

void ChatAttachments::setLiveServices(ProfileSession &session, SyncEngine &engine)
{
    clear();
    dropTransfer();
    dropMedia();
    m_mockMedia.clear();
    m_session = &session;
    m_transfer = std::make_unique<AttachmentTransfer>(
        session, engine, [this](const ConversationId &conversation) { return rosterFor(conversation); },
        [] { return QDateTime::currentMSecsSinceEpoch(); });
    m_transfer->setCallActive(m_callActive);
    connect(m_transfer.get(), &AttachmentTransfer::transferChanged, this, &ChatAttachments::onTransferChanged);
    connect(m_transfer.get(), &AttachmentTransfer::previewArrived, this, &ChatAttachments::onPreviewArrived);
    // It borrows the engine (and the session's store): it goes with it.
    connect(&engine, &QObject::destroyed, this, [this] { dropTransfer(); });
}

void ChatAttachments::dropTransfer()
{
    m_batches.clear();
    m_batchRunning = false;
    m_transfer.reset();
    // Reads under way will never finish now.
    if (m_controller.m_live) {
        const QStringList pending = m_waiters.keys();
        for (const QString &stableId : pending)
            deliverBlob(stableId, {});
    }
}

void ChatAttachments::rosterChanged()
{
    if (m_transfer)
        m_transfer->wake();
}

void ChatAttachments::setCallActive(bool active)
{
    m_callActive = active;
    if (m_transfer)
        m_transfer->setCallActive(active);
}

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------

void ChatAttachments::describe(Message &message, const AttachmentDescriptor &descriptor)
{
    message.attachmentKind = int(kindOf(int(descriptor.kind)));
    // Whoever wrote it: a peer's name is cleaned again here.
    message.fileName = sanitizeAttachmentFileName(descriptor.fileName);
    message.mimeType = isValidMimeType(descriptor.mimeType) ? descriptor.mimeType : QString();
    message.byteCount = descriptor.byteCount;
    message.mediaWidth = std::clamp(descriptor.width, 0, AttachmentLimits::maxImageDimension);
    message.mediaHeight = std::clamp(descriptor.height, 0, AttachmentLimits::maxImageDimension);
    message.durationMs = std::max<qint64>(0, descriptor.durationMs);
    message.peaks.clear();
    const qsizetype bars = std::min<qsizetype>(descriptor.peaks.size(), AttachmentLimits::maxPeaks);
    message.peaks.reserve(bars);
    for (qsizetype bar = 0; bar < bars; ++bar)
        message.peaks.append(int(quint8(descriptor.peaks.at(bar))));
    message.hasPreview = descriptor.hasPreview;
    message.transferTotal = descriptor.partCount;
}

QString ChatAttachments::summaryOf(const Message &message)
{
    if (message.kind != MessageKind::Attachment)
        return message.body;
    return attachmentSummary(kindOf(message.attachmentKind), message.fileName, message.body);
}

// ---------------------------------------------------------------------------
// --attachment-demo
// ---------------------------------------------------------------------------

void ChatAttachments::injectDemo()
{
    if (m_controller.m_live || m_controller.m_currentContactId.isEmpty())
        return;
    const QString peer = m_controller.currentContactName();
    const QDateTime now = QDateTime::currentDateTime();
    const auto row = [&](MessageDirection direction, const QString &caption,
                         const AttachmentDescriptor &descriptor, const QByteArray &blob, const QByteArray &preview,
                         AttachmentTransferState state, int done) {
        Message message{direction, caption, now.time(), MessageKind::Attachment, now.date()};
        message.stableId = newId();
        message.sharedId = true;
        describe(message, descriptor);
        message.hasPreview = !preview.isEmpty();
        message.transferState = state;
        message.transferDone = done;
        if (direction == MessageDirection::Incoming)
            message.transferPeer = peer;
        m_mockMedia.insert(message.stableId, MockMedia{descriptor, blob, preview, false});
        m_controller.m_messages.appendMessage(message);
        m_controller.m_messagesByContact[m_controller.m_currentContactId].append(message);
        return message.stableId;
    };

    // A photo that came in, with a caption.
    const QImage ferry = ChatAttachmentDemo::landscape(QSize(1600, 1200), 0.35);
    const QByteArray ferryJpeg = encodeBaselineJpeg(ferry, 82);
    AttachmentDescriptor photo = ChatAttachmentDemo::descriptor(
        AttachmentKind::Image, ferryJpeg, QStringLiteral("IMG_2041.jpg"), QStringLiteral("image/jpeg"));
    photo.width = ferry.width();
    photo.height = ferry.height();
    photo.hasPreview = true;
    row(MessageDirection::Incoming, QStringLiteral("The view from the ferry this morning"), photo, ferryJpeg,
        ChatAttachmentDemo::preview(ferry), AttachmentTransferState::Ready, photo.partCount);

    // A video this device sent: the poster at once, the clip itself encoded
    // in the background (only needed once it is played).
    const QSize clipSize(320, 180);
    const QImage poster = ChatAttachmentDemo::landscape(QSize(640, 360), 0.25, true);
    AttachmentDescriptor video;
    video.key = QByteArray(AttachmentLimits::keyBytes, '\0');
    video.kind = AttachmentKind::Video;
    video.fileName = QStringLiteral("Evening walk.mp4");
    video.width = clipSize.width();
    video.height = clipSize.height();
    video.durationMs = 4'000;
    video.hasPreview = true;
    // Its size once encoded is not known yet; this is what the bubble shows.
    video.byteCount = 540 * 1024;
    video.partCount = attachmentPartCount(video.byteCount);
    // Without libvpx there is no clip to play: it shows as still on its way.
    const bool clips = clipCodecAvailable();
    const QString videoId =
        row(MessageDirection::Outgoing, QString(), video, QByteArray(), ChatAttachmentDemo::preview(poster),
            clips ? AttachmentTransferState::Ready : AttachmentTransferState::Transferring,
            clips ? video.partCount : 1);
    if (clips) {
        m_mockMedia[videoId].blobPending = true;
        const QPointer<ChatAttachments> guard(this);
        QThreadPool::globalInstance()->start([guard, videoId, clipSize] {
            const QByteArray sequence = ChatAttachmentDemo::video(clipSize);
            QMetaObject::invokeMethod(
                qApp,
                [guard, videoId, sequence] {
                    if (!guard)
                        return;
                    const auto media = guard->m_mockMedia.find(videoId);
                    if (media == guard->m_mockMedia.end())
                        return;
                    media->blob = sequence;
                    media->blobPending = false;
                    guard->deliverBlob(videoId, sequence);
                },
                Qt::QueuedConnection);
        });
    }

    // Audio that came in: a few seconds of a real song container.
    const ChatAttachmentDemo::Song song = ChatAttachmentDemo::song();
    if (!song.container.isEmpty()) {
        AttachmentDescriptor audio = ChatAttachmentDemo::descriptor(AttachmentKind::Audio, song.container,
                                                    QStringLiteral("Harbour tune.m4a"), QString());
        audio.durationMs = song.durationMs;
        audio.peaks = song.peaks;
        row(MessageDirection::Incoming, QString(), audio, song.container, QByteArray(),
            AttachmentTransferState::Ready, audio.partCount);
    }

    // A file this device sent, with a caption.
    QByteArray itinerary("%PDF-1.4\n% OpenChat sample\n");
    while (itinerary.size() < 148 * 1024)
        itinerary += "Day 1: ferry at 08:15, lunch by the harbour, walk to the lighthouse.\n";
    const AttachmentDescriptor file = ChatAttachmentDemo::descriptor(AttachmentKind::File, itinerary,
                                                     QStringLiteral("Trip itinerary.pdf"),
                                                     QStringLiteral("application/pdf"));
    row(MessageDirection::Outgoing, QStringLiteral("Here's the plan for Saturday"), file, itinerary, QByteArray(),
        AttachmentTransferState::Ready, file.partCount);

    // A photo still coming in: three of its seven parts are here.
    const QImage night = ChatAttachmentDemo::landscape(QSize(800, 600), 0.7, true);
    AttachmentDescriptor arriving;
    arriving.key = QByteArray(AttachmentLimits::keyBytes, '\0');
    arriving.kind = AttachmentKind::Image;
    arriving.fileName = QStringLiteral("IMG_2057.jpg");
    arriving.mimeType = QStringLiteral("image/jpeg");
    arriving.width = 2048;
    arriving.height = 1536;
    arriving.byteCount = 1'536'000;
    arriving.partCount = attachmentPartCount(arriving.byteCount);
    arriving.hasPreview = true;
    row(MessageDirection::Incoming, QString(), arriving, QByteArray(), ChatAttachmentDemo::preview(night),
        AttachmentTransferState::Transferring, 3);
    m_controller.m_contacts.setActivity(m_controller.m_currentContactId, now.toMSecsSinceEpoch(), 0);

    // And a photo waiting in the tray.
    const QImage harbour = ChatAttachmentDemo::landscape(QSize(1200, 900), 0.55);
    const QByteArray harbourJpeg = encodeBaselineJpeg(harbour, 82);
    AttachmentDescriptor staged = ChatAttachmentDemo::descriptor(AttachmentKind::Image, harbourJpeg,
                                                 QStringLiteral("Harbour.jpg"), QStringLiteral("image/jpeg"));
    staged.key.clear();
    staged.width = harbour.width();
    staged.height = harbour.height();
    const QByteArray stagedPreview = ChatAttachmentDemo::preview(harbour);
    staged.hasPreview = !stagedPreview.isEmpty();
    if (int(m_staged.size()) < AttachmentLimits::maxStaged && !m_withheld) {
        const bool wasSendable = m_controller.canSend();
        auto card = std::make_unique<Staged>();
        card->id = newId();
        card->name = staged.fileName;
        card->kind = AttachmentKind::Image;
        card->started = true;
        const QString id = card->id;
        stage(std::move(card));
        onPrepared(id, PreparedAttachment{staged, harbourJpeg, stagedPreview, {}});
        changed(wasSendable);
    }
}

} // namespace OpenChat
