#pragma once

#include "domain/Identifiers.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <optional>

namespace OpenChat {

// The longest excerpt of an answered message a reply carries.
inline constexpr int maxQuoteLength = 200;

// What one conversation message (EnvelopeMessageKind::MlsPrivateMessage)
// carries once decrypted.
//
// A plain message is still the bare UTF-8 of its text, exactly as every
// earlier version sends and reads it. Only a reply or an edit is tagged: a
// leading 0xFF, a byte UTF-8 never produces, then a fixed-shape CBOR array.
// So a plain message can never be mistaken for either, and an older client
// still reads everything except those two.
//
// Replies and edits name another message by the id both ends derive from its
// ciphertext (messageIdForCiphertext), never by an id one side made up.
struct MessageContent final {
    enum class Type : quint8 {
        Text = 0,
        // A message answering `target`. The answered message's sender and an
        // excerpt of it travel along, so the quote still shows on a device
        // that does not hold the answered message.
        Reply = 1,
        // New text for `target`, which the same device sent earlier.
        Edit = 2,
    };

    Type type = Type::Text;
    QString body;
    std::optional<MessageId> target;      // Reply and Edit
    std::optional<DeviceId> quotedSender; // Reply
    QString quotedBody;                   // Reply

    [[nodiscard]] static MessageContent text(const QString &body);
    [[nodiscard]] static MessageContent reply(const QString &body, const MessageId &target,
                                              const DeviceId &quotedSender,
                                              const QString &quotedBody);
    [[nodiscard]] static MessageContent edit(const MessageId &target, const QString &body);

    friend bool operator==(const MessageContent &, const MessageContent &) = default;
};

// Text encodes as its bare UTF-8; Reply and Edit as the tag and CBOR above.
[[nodiscard]] QByteArray encodeMessageContent(const MessageContent &content);
// Anything untagged is Text. A tagged payload that is malformed, of a later
// version, of an unknown type or beyond the bounds yields nothing: the caller
// consumes it without showing anything.
[[nodiscard]] std::optional<MessageContent> decodeMessageContent(QByteArrayView bytes);

// The id sender and every recipient give one message: the first 16 bytes of
// the SHA-256 of its MLS ciphertext. A group message is encrypted once for
// every member, so all of them arrive at the same id.
[[nodiscard]] MessageId messageIdForCiphertext(QByteArrayView ciphertext);

// The excerpt a reply quotes: the first maxQuoteLength characters, never
// splitting a surrogate pair.
[[nodiscard]] QString quoteExcerpt(const QString &body);

} // namespace OpenChat
