#pragma once

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QString>

#include <optional>

namespace OpenChat {

// The process-wide registry of decoded profile pictures, addressed by content.
// Avatars in the interface are named by a string key: the built-in artwork
// keys ("michael", "userpfp_none", …) and, from here, "blob:<hash>" keys for
// pictures received from contacts or chosen locally. A key names exactly one
// image for as long as the process lives, so a changed picture is a changed
// key and every QML binding on it refreshes on its own.
class AvatarStore final
{
public:
    static AvatarStore &instance();

    // Decodes a JPEG (bounded: at most maxAvatarJpegBytes and 1024 px a side)
    // and registers it. Returns its "blob:…" key, or an empty string when the
    // bytes are empty or not a decodable picture.
    [[nodiscard]] QString registerJpeg(const QByteArray &jpeg);

    // The image behind a "blob:…" key, or nullopt for any other key or one this
    // process has not registered.
    [[nodiscard]] std::optional<QImage> image(const QString &key) const;

    [[nodiscard]] static bool isBlobKey(const QString &key);
    // The key `jpeg` would register under, without decoding or registering it.
    [[nodiscard]] static QString keyFor(const QByteArray &jpeg);

    void clear();

private:
    AvatarStore() = default;

    mutable QMutex m_mutex;
    QHash<QString, QImage> m_images;
};

} // namespace OpenChat
