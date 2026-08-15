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
    [[nodiscard]] std::optional<Message> messageAt(int row) const;

signals:
    void countChanged();

private:
    QVector<Message> m_messages;
};

} // namespace OpenChat
