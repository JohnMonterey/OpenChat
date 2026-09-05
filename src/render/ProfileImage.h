#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QImage>
#include <QString>

namespace OpenChat {

// Why a candidate profile picture was refused. Every value maps to a short
// message the interface can show; none of them is a crash.
enum class ProfileImageError {
    FileMissing,   // the chosen path does not exist or cannot be read
    FileTooLarge,  // the file is bigger than a photo has any reason to be
    Unreadable,    // not an image format Qt can decode, or a corrupt one
    TooSmall,      // fewer pixels on a side than makes a usable picture
    TooLarge,      // more pixels on a side than a sane photo (decode bomb guard)
    EncodeFailed,  // could not be brought under the payload bound
};

struct ProfileImageLimits final {
    qint64 maxFileBytes = 25LL * 1024 * 1024; // the biggest file even opened
    int minSide = 16;                          // smallest accepted source side
    int maxSide = 12'000;                      // largest accepted source side
    int outputSide = 256;                      // the square the picture becomes
    qsizetype maxOutputBytes = 48 * 1024;      // the JPEG must fit under this
};

// Reads an image file, refuses anything outside the limits BEFORE decoding it
// in full (size on disk, then header dimensions with a bounded allocation),
// centre-crops it square, scales it to `outputSide` and JPEG-encodes it,
// lowering quality and then the side length until it fits `maxOutputBytes`.
// The result is what a ProfileUpdate carries and what the local profile stores.
[[nodiscard]] Result<QByteArray, ProfileImageError>
processProfileImageFile(const QString &path, const ProfileImageLimits &limits = {});

// The same pipeline from an already decoded image (the file step's tail; also
// what tests drive directly).
[[nodiscard]] Result<QByteArray, ProfileImageError>
processProfileImage(const QImage &image, const ProfileImageLimits &limits = {});

// A short human sentence for an error, for the interface to show.
[[nodiscard]] QString profileImageErrorText(ProfileImageError error);

} // namespace OpenChat
