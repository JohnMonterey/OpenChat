#pragma once

#include <QString>
#include <QTime>

namespace OpenChat {

enum class MessageDirection {
    Incoming,
    Outgoing,
};

enum class MessageKind {
    Text,
    Emoji,
};

struct Message {
    MessageDirection direction = MessageDirection::Incoming;
    QString body;
    QTime timestamp;
    MessageKind kind = MessageKind::Text;
};

} // namespace OpenChat
