#pragma once

#include <QPainterPath>
#include <QQuickPaintedItem>

namespace OpenChat {

class AvatarArtwork : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(QString avatarKey READ avatarKey WRITE setAvatarKey NOTIFY avatarKeyChanged)
    Q_PROPERTY(
        qreal cornerRadius READ cornerRadius WRITE setCornerRadius NOTIFY cornerRadiusChanged)

  public:
    explicit AvatarArtwork(QQuickItem *parent = nullptr);

    [[nodiscard]] QString avatarKey() const;
    void setAvatarKey(const QString &avatarKey);
    [[nodiscard]] qreal cornerRadius() const;
    void setCornerRadius(qreal cornerRadius);

    void paint(QPainter *painter) override;
    static QPainterPath makeClipPath(const QRectF &bounds, qreal cornerRadius);

  signals:
    void avatarKeyChanged();
    void cornerRadiusChanged();

  private:
    QString m_avatarKey = QStringLiteral("neutral");
    qreal m_cornerRadius = 5.0;
};

} // namespace OpenChat
