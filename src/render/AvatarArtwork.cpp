#include "render/AvatarArtwork.h"

#include "render/AvatarStore.h"

#include <QLinearGradient>
#include <QImage>
#include <QPainter>

#include <algorithm>

namespace OpenChat {

namespace {

QString photoResourcePath(const QString &avatarKey)
{
    if (avatarKey == QStringLiteral("michael"))
        return QStringLiteral(":/qt/qml/OpenChat/assets/michael.png");
    if (avatarKey == QStringLiteral("jessica"))
        return QStringLiteral(":/qt/qml/OpenChat/assets/Jessica.png");
    if (avatarKey == QStringLiteral("alex"))
        return QStringLiteral(":/qt/qml/OpenChat/assets/alex.png");
    if (avatarKey == QStringLiteral("ryan"))
        return QStringLiteral(":/qt/qml/OpenChat/assets/ryan.png");
    if (avatarKey == QStringLiteral("userpfp_none"))
        return QStringLiteral(":/qt/qml/OpenChat/assets/userpfp_none.png");
    return {};
}

QRectF centeredCrop(const QSize &imageSize, const QSizeF &targetSize)
{
    QRectF source(QPointF(), imageSize);
    if (imageSize.isEmpty() || targetSize.isEmpty())
        return source;

    const qreal sourceAspect = source.width() / source.height();
    const qreal targetAspect = targetSize.width() / targetSize.height();
    if (sourceAspect > targetAspect) {
        const qreal croppedWidth = source.height() * targetAspect;
        source.setLeft((source.width() - croppedWidth) / 2.0);
        source.setWidth(croppedWidth);
    } else if (sourceAspect < targetAspect) {
        const qreal croppedHeight = source.width() / targetAspect;
        source.setTop((source.height() - croppedHeight) / 2.0);
        source.setHeight(croppedHeight);
    }
    return source;
}

QRectF photoSourceRect(const QString &avatarKey, const QSize &imageSize,
                       const QSizeF &targetSize)
{
    if (avatarKey == QStringLiteral("userpfp_none") || AvatarStore::isBlobKey(avatarKey))
        return centeredCrop(imageSize, targetSize);

    const qreal side = std::min(imageSize.width(), imageSize.height()) * 0.62;
    return QRectF((imageSize.width() - side) / 2.0, 0.0, side, side);
}

} // namespace

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
    const QRectF rect = bounds.normalized();
    const qreal radius = std::clamp(cornerRadius, 0.0, std::min(rect.width(), rect.height()) / 2.0);
    QPainterPath path;
    path.addRoundedRect(rect, radius, radius);
    return path;
}

void AvatarArtwork::paint(QPainter *painter)
{
    const QRectF rect = boundingRect();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setClipPath(makeClipPath(rect, m_cornerRadius), Qt::IntersectClip);

    QImage photo;
    // A received or chosen picture lives in the store under a content key; the
    // bundled artwork is a resource. Either way the photo is drawn the same.
    if (const auto stored = AvatarStore::instance().image(m_avatarKey))
        photo = *stored;
    const QString resourcePath = photoResourcePath(m_avatarKey);
    if (photo.isNull() && !resourcePath.isEmpty())
        photo.load(resourcePath);
    if (!photo.isNull()) {
        painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter->drawImage(rect, photo, photoSourceRect(m_avatarKey, photo.size(), rect.size()));
        return;
    }

    QColor top("#e9c9ae");
    QColor bottom("#a86e4c");
    if (m_avatarKey == QStringLiteral("landscape")) {
        top = QColor("#79b9ed");
        bottom = QColor("#d9edfb");
    } else if (m_avatarKey == QStringLiteral("beach")) {
        top = QColor("#86d8ee");
        bottom = QColor("#f5df9d");
    } else if (m_avatarKey == QStringLiteral("mono") || m_avatarKey == QStringLiteral("alex")) {
        top = QColor("#d5d5d5");
        bottom = QColor("#8b8b8b");
    }

    QLinearGradient background(rect.topLeft(), rect.bottomLeft());
    background.setColorAt(0.0, top);
    background.setColorAt(1.0, bottom);
    painter->fillRect(rect, background);

    if (m_avatarKey == QStringLiteral("landscape") || m_avatarKey == QStringLiteral("beach") ||
        m_avatarKey == QStringLiteral("mono")) {
        const qreal horizon =
            rect.height() * (m_avatarKey == QStringLiteral("beach") ? 0.57 : 0.66);
        painter->fillRect(QRectF(0, horizon, rect.width(), rect.height() - horizon),
                          m_avatarKey == QStringLiteral("beach")  ? QColor("#62c2cf")
                          : m_avatarKey == QStringLiteral("mono") ? QColor("#626262")
                                                                  : QColor("#506554"));
        if (m_avatarKey == QStringLiteral("landscape")) {
            painter->save();
            painter->translate(rect.width() * 0.30, rect.height() * 0.72);
            painter->rotate(-8);
            painter->setBrush(QColor("#3f5548"));
            painter->setPen(Qt::NoPen);
            painter->drawRoundedRect(QRectF(-rect.width() * 0.42, -8, rect.width() * 0.72, 16), 8,
                                     8);
            painter->restore();
        } else if (m_avatarKey == QStringLiteral("beach")) {
            painter->fillRect(QRectF(0, rect.height() * 0.76, rect.width(), rect.height() * 0.24),
                              QColor("#f0d18c"));
        }
        return;
    }

    painter->setPen(Qt::NoPen);
    if (m_avatarKey == QStringLiteral("sarah") || m_avatarKey == QStringLiteral("jessica") ||
        m_avatarKey == QStringLiteral("alex")) {
        painter->setBrush(m_avatarKey == QStringLiteral("alex") ? QColor("#3e3e3e")
                                                                : QColor("#704a34"));
        painter->drawEllipse(QRectF(rect.width() * 0.20, rect.height() * 0.14, rect.width() * 0.60,
                                    rect.height() * 0.62));
        painter->setBrush(m_avatarKey == QStringLiteral("alex") ? QColor("#d2d2d2")
                                                                : QColor("#f1c6a7"));
        painter->drawEllipse(QRectF(rect.width() * 0.29, rect.height() * 0.18, rect.width() * 0.43,
                                    rect.height() * 0.47));
        painter->setBrush(m_avatarKey == QStringLiteral("alex")      ? QColor("#353535")
                          : m_avatarKey == QStringLiteral("jessica") ? QColor("#6ba3a5")
                                                                     : QColor("#e3a58a"));
        painter->drawEllipse(QRectF(rect.width() * 0.14, rect.height() * 0.68, rect.width() * 0.72,
                                    rect.height() * 0.42));
        return;
    }

    painter->setBrush(QColor("#7f8c97"));
    painter->drawEllipse(QRectF(rect.width() * 0.31, rect.height() * 0.18, rect.width() * 0.38,
                                rect.width() * 0.38));
    painter->setBrush(QColor("#6e7b86"));
    painter->drawEllipse(QRectF(rect.width() * 0.15, rect.height() * 0.56, rect.width() * 0.70,
                                rect.height() * 0.42));
}

} // namespace OpenChat
