#include "render/AvatarPainter.h"

#include "render/AvatarStore.h"

#include <QLinearGradient>
#include <QPainter>

#include <algorithm>

namespace OpenChat::AvatarPainter {

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

QRectF photoSourceRect(const QString &avatarKey, const QSize &imageSize, const QSizeF &targetSize)
{
    if (avatarKey == QStringLiteral("userpfp_none") || AvatarStore::isBlobKey(avatarKey))
        return centeredCrop(imageSize, targetSize);

    const qreal side = std::min(imageSize.width(), imageSize.height()) * 0.62;
    return QRectF((imageSize.width() - side) / 2.0, 0.0, side, side);
}

} // namespace

QPainterPath makeClipPath(const QRectF &bounds, qreal cornerRadius)
{
    const QRectF rect = bounds.normalized();
    const qreal radius = std::clamp(cornerRadius, 0.0, std::min(rect.width(), rect.height()) / 2.0);
    QPainterPath path;
    path.addRoundedRect(rect, radius, radius);
    return path;
}

void paint(QPainter &painter, const QRectF &rect, const QString &avatarKey, qreal cornerRadius)
{
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setClipPath(makeClipPath(rect, cornerRadius), Qt::IntersectClip);

    QImage photo;
    // A received or chosen picture lives in the store under a content key; the
    // bundled artwork is a resource. Either way the photo is drawn the same.
    if (const auto stored = AvatarStore::instance().image(avatarKey))
        photo = *stored;
    const QString resourcePath = photoResourcePath(avatarKey);
    if (photo.isNull() && !resourcePath.isEmpty())
        photo.load(resourcePath);
    if (!photo.isNull()) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(rect, photo, photoSourceRect(avatarKey, photo.size(), rect.size()));
        return;
    }

    QColor top("#e9c9ae");
    QColor bottom("#a86e4c");
    if (avatarKey == QStringLiteral("landscape")) {
        top = QColor("#79b9ed");
        bottom = QColor("#d9edfb");
    } else if (avatarKey == QStringLiteral("beach")) {
        top = QColor("#86d8ee");
        bottom = QColor("#f5df9d");
    } else if (avatarKey == QStringLiteral("mono") || avatarKey == QStringLiteral("alex")) {
        top = QColor("#d5d5d5");
        bottom = QColor("#8b8b8b");
    }

    QLinearGradient background(rect.topLeft(), rect.bottomLeft());
    background.setColorAt(0.0, top);
    background.setColorAt(1.0, bottom);
    painter.fillRect(rect, background);

    if (avatarKey == QStringLiteral("landscape") || avatarKey == QStringLiteral("beach") ||
        avatarKey == QStringLiteral("mono")) {
        const qreal horizon = rect.height() * (avatarKey == QStringLiteral("beach") ? 0.57 : 0.66);
        painter.fillRect(QRectF(0, horizon, rect.width(), rect.height() - horizon),
                         avatarKey == QStringLiteral("beach")  ? QColor("#62c2cf")
                         : avatarKey == QStringLiteral("mono") ? QColor("#626262")
                                                               : QColor("#506554"));
        if (avatarKey == QStringLiteral("landscape")) {
            painter.save();
            painter.translate(rect.width() * 0.30, rect.height() * 0.72);
            painter.rotate(-8);
            painter.setBrush(QColor("#3f5548"));
            painter.setPen(Qt::NoPen);
            painter.drawRoundedRect(QRectF(-rect.width() * 0.42, -8, rect.width() * 0.72, 16), 8,
                                    8);
            painter.restore();
        } else if (avatarKey == QStringLiteral("beach")) {
            painter.fillRect(QRectF(0, rect.height() * 0.76, rect.width(), rect.height() * 0.24),
                             QColor("#f0d18c"));
        }
        return;
    }

    painter.setPen(Qt::NoPen);
    if (avatarKey == QStringLiteral("group")) {
        // Two silhouettes, one a step behind the other: a group, not a person.
        QLinearGradient sky(rect.topLeft(), rect.bottomLeft());
        sky.setColorAt(0.0, QColor("#dcebf6"));
        sky.setColorAt(1.0, QColor("#a9c6dc"));
        painter.fillRect(rect, sky);
        painter.setBrush(QColor("#8fa4b5"));
        painter.drawEllipse(QRectF(rect.width() * 0.50, rect.height() * 0.22, rect.width() * 0.28,
                                   rect.width() * 0.28));
        painter.drawEllipse(QRectF(rect.width() * 0.40, rect.height() * 0.54, rect.width() * 0.50,
                                   rect.height() * 0.40));
        painter.setBrush(QColor("#5f7789"));
        painter.drawEllipse(QRectF(rect.width() * 0.20, rect.height() * 0.28, rect.width() * 0.32,
                                   rect.width() * 0.32));
        painter.drawEllipse(QRectF(rect.width() * 0.06, rect.height() * 0.62, rect.width() * 0.60,
                                   rect.height() * 0.44));
        return;
    }
    if (avatarKey == QStringLiteral("sarah") || avatarKey == QStringLiteral("jessica") ||
        avatarKey == QStringLiteral("alex")) {
        painter.setBrush(avatarKey == QStringLiteral("alex") ? QColor("#3e3e3e")
                                                             : QColor("#704a34"));
        painter.drawEllipse(QRectF(rect.width() * 0.20, rect.height() * 0.14, rect.width() * 0.60,
                                   rect.height() * 0.62));
        painter.setBrush(avatarKey == QStringLiteral("alex") ? QColor("#d2d2d2")
                                                             : QColor("#f1c6a7"));
        painter.drawEllipse(QRectF(rect.width() * 0.29, rect.height() * 0.18, rect.width() * 0.43,
                                   rect.height() * 0.47));
        painter.setBrush(avatarKey == QStringLiteral("alex")      ? QColor("#353535")
                         : avatarKey == QStringLiteral("jessica") ? QColor("#6ba3a5")
                                                                  : QColor("#e3a58a"));
        painter.drawEllipse(QRectF(rect.width() * 0.14, rect.height() * 0.68, rect.width() * 0.72,
                                   rect.height() * 0.42));
        return;
    }

    painter.setBrush(QColor("#7f8c97"));
    painter.drawEllipse(QRectF(rect.width() * 0.31, rect.height() * 0.18, rect.width() * 0.38,
                               rect.width() * 0.38));
    painter.setBrush(QColor("#6e7b86"));
    painter.drawEllipse(QRectF(rect.width() * 0.15, rect.height() * 0.56, rect.width() * 0.70,
                               rect.height() * 0.42));
}

QImage render(const QString &avatarKey, int sizePx, qreal cornerRadius)
{
    if (sizePx <= 0)
        return {};
    QImage image(sizePx, sizePx, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    paint(painter, QRectF(0, 0, sizePx, sizePx), avatarKey, cornerRadius);
    painter.end();
    return image;
}

} // namespace OpenChat::AvatarPainter
