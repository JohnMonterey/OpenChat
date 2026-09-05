#include "render/AvatarArtwork.h"

#include "render/AvatarPainter.h"

#include <QPainter>

#include <algorithm>

namespace OpenChat {

AvatarArtwork::AvatarArtwork(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
}

QString AvatarArtwork::avatarKey() const
{
    return m_avatarKey;
}

void AvatarArtwork::setAvatarKey(const QString &avatarKey)
{
    if (m_avatarKey == avatarKey)
        return;
    m_avatarKey = avatarKey;
    emit avatarKeyChanged();
    update();
}

qreal AvatarArtwork::cornerRadius() const
{
    return m_cornerRadius;
}

void AvatarArtwork::setCornerRadius(qreal cornerRadius)
{
    const qreal normalizedRadius = std::max(0.0, cornerRadius);
    if (qFuzzyCompare(m_cornerRadius, normalizedRadius))
        return;
    m_cornerRadius = normalizedRadius;
    emit cornerRadiusChanged();
    update();
}

QPainterPath AvatarArtwork::makeClipPath(const QRectF &bounds, qreal cornerRadius)
{
    return AvatarPainter::makeClipPath(bounds, cornerRadius);
}

void AvatarArtwork::paint(QPainter *painter)
{
    // The artwork itself lives in AvatarPainter so that off-screen consumers —
    // desktop notifications — draw exactly the picture the interface shows.
    AvatarPainter::paint(*painter, boundingRect(), m_avatarKey, m_cornerRadius);
}

} // namespace OpenChat
