#pragma once

#include "domain/ProfilePage.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QVector>

#include <optional>

namespace OpenChat {

// Profile pages travel inside ProfileUpdate envelopes (kind 7), beside the
// legacy [1, presence, status, avatar] array. A page message is the byte 0xFF
// followed by one canonical CBOR map with small integer keys in ascending
// order: {0: version, 1: type, …}. 0xFF is the CBOR "break" byte, which can
// never start a top-level item, so 0.2.8's decodeProfileUpdate() fails on it
// and silently ignores every page message; new clients classify first.
//
// Everything arrives over MLS from an authenticated contact, but a contact's
// page is still hostile input: every decoder checks the byte cap before
// parsing, rejects wrong shapes, and normalises values it can repair (unknown
// keys ignored, enums reset, numbers clamped, text sanitised), so an older
// client can read a newer client's page.
inline constexpr quint64 pageWireVersion = 1;
// The MLS plaintext cap on sender and receiver. Documentation only: nothing
// here may ever reach it, because a failed encrypt stops the whole engine.
inline constexpr qsizetype mlsPlaintextCapBytes = 256 * 1024;
inline constexpr qsizetype maxPageSendBytes = 240 * 1024;       // asserted before every send
inline constexpr qsizetype maxBackgroundImageBytes = 224 * 1024; // JPEG data only
inline constexpr qsizetype maxSongBytes = 224 * 1024;            // song container only
inline constexpr qsizetype maxPageMediaMessageBytes = 224 * 1024 + 512;
inline constexpr qsizetype maxPageCoreBytes = 24 * 1024;
inline constexpr qsizetype maxPageRequestBytes = 256;
inline constexpr int maxJpegScans = 32; // progressive encoders emit at most 12
static_assert(maxPageMediaMessageBytes <= maxPageSendBytes && maxPageSendBytes < mlsPlaintextCapBytes);

enum class ProfilePayloadKind {
    Legacy,      // first byte is not 0xFF: hand it to decodeProfileUpdate as before
    PageCore,
    PageMedia,
    PageRequest,
    UnknownPage, // tagged but unreadable, a future version, an unknown type or media kind: ignore silently
};
// Reads only as much as it must to tell the kinds apart. A PageCore/Media/
// Request verdict does not mean the message is valid; its decoder decides.
[[nodiscard]] ProfilePayloadKind classifyProfilePayload(QByteArrayView payload);

// Normalises first, so encode(decode(x)) is a fixed point and the encoding of
// a page is deterministic.
[[nodiscard]] QByteArray encodePageCore(const Profile::Page &page);
// The result is always normalised.
[[nodiscard]] std::optional<Profile::Page> decodePageCore(QByteArrayView payload);

// One media blob per message. Checked on arrival: the hash, the per-kind cap
// and a cheap structural test (a JPEG marker walk with a scan limit, or a
// well-formed song container); the full decode happens later, off the GUI
// thread, only if the page is shown.
struct PageMediaMessage final {
    Profile::MediaKind kind = Profile::MediaKind::BackgroundImageMedia;
    QByteArray sha256; // 32 bytes
    QByteArray data;

    friend bool operator==(const PageMediaMessage &, const PageMediaMessage &) = default;
};
// Callers pass the hash they stored (asserted equal in debug builds).
[[nodiscard]] QByteArray encodePageMedia(const PageMediaMessage &message);
[[nodiscard]] std::optional<PageMediaMessage> decodePageMedia(QByteArrayView payload);

// A viewer asking a contact for their page (haveRevision absent: "I have no
// page of yours") and/or for up to two blobs its stored core names.
struct PageRequestMessage final {
    std::optional<qint64> haveRevision;
    QVector<QByteArray> wantMedia; // each 32 bytes, at most 2

    friend bool operator==(const PageRequestMessage &, const PageRequestMessage &) = default;
};
[[nodiscard]] QByteArray encodePageRequest(const PageRequestMessage &message);
[[nodiscard]] std::optional<PageRequestMessage> decodePageRequest(QByteArrayView payload);

[[nodiscard]] QByteArray pageMediaHash(QByteArrayView data); // SHA-256
// Counts the scans (SOS markers) of a JPEG by walking its marker segments and
// skipping entropy-coded data. -1 when the walk fails: no SOI, a bad segment
// length, a stray byte where a marker belongs, or no EOI before the end.
[[nodiscard]] int jpegScanCount(QByteArrayView jpeg);

} // namespace OpenChat
