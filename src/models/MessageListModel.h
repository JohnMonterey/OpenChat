#pragma once

#include <QAbstractListModel>
#include <QDateTime>
#include <QVector>

#include <optional>

#include "models/Message.h"

namespace OpenChat {

class MessageListModel final : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        DirectionRole = Qt::UserRole + 1,
        BodyRole,
        TimestampRole,
        KindRole,
        DateLabelRole,
        ShowDateDividerRole,
        StableIdRole,
        DeliveryStateRole,
        FailureReasonRole,
        SenderDeviceRole,
        SecurityEventRole,
        SenderNameRole,
        EditedRole,
        EditableRole,
        ReplyToIdRole,
        QuotedSenderRole,
        QuotedBodyRole,
        SenderAccountRole,
        // Attachments (docs/chat-attachments.md); empty or zero on any other
        // kind of row.
        AttachmentKindRole,
        FileNameRole,
        MimeTypeRole,
        ByteCountRole,
        SizeTextRole,
        MediaWidthRole,
        MediaHeightRole,
        DurationMsRole,
        PeaksRole,
        TransferStateRole,
        TransferReasonRole,
        TransferProgressRole,
        TransferTextRole,
        HasPreviewRole,
        PreviewRevisionRole,
        CanCancelRole,
        CanRetryRole,
        CanSaveRole,
    };
    Q_ENUM(Role)

    explicit MessageListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] int count() const;

    // A size as the attachment cards print it: "812 bytes", "340 KB",
    // "2.4 MB", "16 MB" (1024-based, as the limits are).
    [[nodiscard]] static QString sizeText(qint64 bytes);
    // A length as "0:42" or "4:05".
    [[nodiscard]] static QString durationText(qint64 durationMs);
    // The line an attachment bubble prints under its media while its bytes
    // are not all there: "Sending… 40%", "Receiving… 3 of 7", "Waiting for
    // Alice", "Couldn't send", …; empty once they are.
    [[nodiscard]] QString transferText(const Message &message) const;

    void setMessages(QVector<Message> messages);
    bool appendOutgoing(const QString &body, const QTime &timestamp);
    bool appendOutgoing(const QString &body, const QDateTime &sentAt);
    // Append a fully-formed row (a durable record from the engine).
    void appendMessage(Message message);
    // Update the delivery state of the row with `stableId`. Returns whether a
    // row was found.
    bool updateDeliveryState(const QString &stableId, MessageDeliveryState state,
                             MessageFailureReason failureReason = MessageFailureReason::None);
    // The sender changed the text of the row with `stableId`: it shows the new
    // text and says it was edited. Returns whether a row was found.
    bool updateBody(const QString &stableId, const QString &body);
    // The bytes of the attachment with `stableId` moved on: its transfer
    // state, why it stopped and its progress. Only the transfer roles
    // change. Returns whether a row was found.
    bool updateTransfer(const QString &stableId, AttachmentTransferState state, int reason, int done,
                        int total);
    // The attachment with `stableId` has a preview now (or a newer one).
    bool bumpPreview(const QString &stableId);
    // While a call is on, this device's attachments wait for it to end, and
    // their bubbles say so.
    void setCallActive(bool active);
    [[nodiscard]] bool callActive() const noexcept { return m_callActive; }
    [[nodiscard]] std::optional<Message> messageAt(int row) const;
    [[nodiscard]] std::optional<Message> messageById(const QString &stableId) const;
    // The row of `stableId`, or -1.
    Q_INVOKABLE [[nodiscard]] int rowOf(const QString &stableId) const;

signals:
    void countChanged();

private:
    QVector<Message> m_messages;
    bool m_callActive = false;
};

} // namespace OpenChat
