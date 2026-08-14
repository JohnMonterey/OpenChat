#pragma once

#include <QAbstractListModel>
#include <QVector>

#include <optional>

#include "models/Message.h"

namespace OpenChat {

class MessageListModel final : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Role {
        DirectionRole = Qt::UserRole + 1,
        BodyRole,
        TimestampRole,
        KindRole,
    };
    Q_ENUM(Role)

    explicit MessageListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setMessages(QVector<Message> messages);
    bool appendOutgoing(const QString &body, const QTime &timestamp);
    [[nodiscard]] std::optional<Message> messageAt(int row) const;

private:
    QVector<Message> m_messages;
};

} // namespace OpenChat

