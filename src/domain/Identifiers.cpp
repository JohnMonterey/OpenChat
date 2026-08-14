#include "domain/Identifiers.h"

#include <QRandomGenerator>

#include <array>

namespace OpenChat::Detail {

QByteArray generateIdentifierBytes()
{
    std::array<quint32, 4> words{};
    QByteArray bytes;
    do {
        QRandomGenerator::system()->fillRange(words.data(), words.size());
        bytes = QByteArray(reinterpret_cast<const char *>(words.data()),
                           static_cast<qsizetype>(sizeof(words)));
    } while (!StrongId<MessageIdTag>::fromBytes(bytes).has_value());
    return bytes;
}

} // namespace OpenChat::Detail
