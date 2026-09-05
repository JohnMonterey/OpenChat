#include "render/CallVideoItem.h"
#include <QPainter>
#include <QPainterPath>

namespace OpenChat {

void CallVideoItem::paint(QPainter *painter)
{
    if (m_frame.isNull())
        return;
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setRenderHint(QPainter::SmoothPixmapTransform);
    const QSizeF size = QSizeF(m_frame.size()).scaled(boundingRect().size(), Qt::KeepAspectRatio);
    const QRectF target((width() - size.width()) / 2, (height() - size.height()) / 2,
                        size.width(), size.height());
    QPainterPath clip;
    clip.addRoundedRect(target, 6, 6);
    painter->setClipPath(clip);
    if (m_mirrored) {
        painter->translate(width(), 0);
        painter->scale(-1, 1);
    }
    painter->drawImage(target, m_frame);
}

} // namespace OpenChat
