#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

#include <optional>

namespace OpenChat {

// Bounds on what a user may publish about themselves. The status line is a
// single short sentence; the picture is a small JPEG the sender already scaled
// and compressed (see render/ProfileImage.h), well inside one envelope.
inline constexpr int maxStatusTextLength = 80;
inline constexpr qsizetype maxAvatarJpegBytes = 64 * 1024;

// What one user tells their contacts about themselves. Travels as an MLS
// application message inside each 2-party group (EnvelopeMessageKind::
// ProfileUpdate), so confidentiality, integrity and sender authentication all
// come from the ratchet; this codec is defensive about shape only.
struct ProfileUpdateMessage final {
    int presence = 0;      // a models/Contact.h Presence value
    QString statusText;    // empty: show the presence name instead
    QByteArray avatarJpeg; // empty: no picture (clears any earlier one)

    friend bool operator==(const ProfileUpdateMessage &, const ProfileUpdateMessage &) = default;
};

// Serialises to a fixed-arity CBOR array. Decoding rejects anything that is
// not exactly the expected shape or exceeds the bounds above rather than
// clamping, so a malformed update can never be mistaken for a well-formed one.
[[nodiscard]] QByteArray encodeProfileUpdate(const ProfileUpdateMessage &message);
[[nodiscard]] std::optional<ProfileUpdateMessage> decodeProfileUpdate(QByteArrayView bytes);

} // namespace OpenChat
