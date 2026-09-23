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
    };
    Q_ENUM(Role)

    explicit MessageListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    [[nodiscard]] int count() const;

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
    [[nodiscard]] std::optional<Message> messageAt(int row) const;
    [[nodiscard]] std::optional<Message> messageById(const QString &stableId) const;
    // The row of `stableId`, or -1.
    Q_INVOKABLE [[nodiscard]] int rowOf(const QString &stableId) const;

signals:
    void countChanged();

private:
    QVector<Message> m_messages;
};

} // namespace OpenChat
