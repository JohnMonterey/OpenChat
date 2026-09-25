#pragma once

#include "domain/Attachment.h"
#include "domain/ChatTypes.h"
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
//
// An attachment (a photo, video, audio clip or file) is tagged too, as type 3:
// 0.2.8 and 0.2.9 clients consume it without showing anything.
struct MessageContent final {
    enum class Type : quint8 {
        Text = 0,
        // A message answering `target`. The answered message's sender and an
        // excerpt of it travel along, so the quote still shows on a device
        // that does not hold the answered message.
        Reply = 1,
        // New text for `target`, which the same device sent earlier.
        Edit = 2,
        // A photo, video, audio clip or file (`attachment`) with an optional
        // caption (`body`, may be empty); it may also answer `target` the way
        // a Reply does. The bytes follow as AttachmentControl frames.
        Attachment = 3,
    };

    Type type = Type::Text;
    QString body;
    std::optional<MessageId> target;      // Reply and Edit
    std::optional<DeviceId> quotedSender; // Reply (and an Attachment that answers)
    QString quotedBody;                   // Reply (and an Attachment that answers)
    std::optional<AttachmentDescriptor> attachment; // Attachment

    [[nodiscard]] static MessageContent text(const QString &body);
    [[nodiscard]] static MessageContent reply(const QString &body, const MessageId &target,
                                              const DeviceId &quotedSender,
                                              const QString &quotedBody);
    [[nodiscard]] static MessageContent edit(const MessageId &target, const QString &body);
    // `quote` makes it answer another message, as a Reply does.
    [[nodiscard]] static MessageContent attachmentMessage(const QString &caption,
                                                          const AttachmentDescriptor &attachment,
                                                          const std::optional<MessageQuote> &quote
                                                          = std::nullopt);

    friend bool operator==(const MessageContent &, const MessageContent &) = default;
};

// Text encodes as its bare UTF-8; Reply, Edit and Attachment as the tag and
// CBOR above. An attachment encodes to nothing (an empty array) when it would
// break a rule decodeMessageContent enforces, so it is never sent at all.
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
