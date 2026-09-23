#pragma once

#include <QColor>
#include <QPainterPath>
#include <QQuickPaintedItem>
#include <QString>

namespace OpenChat {

class BubbleBackground : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(bool outgoing READ outgoing WRITE setOutgoing NOTIFY outgoingChanged)
    Q_PROPERTY(qreal radius READ radius WRITE setRadius NOTIFY radiusChanged)
    Q_PROPERTY(qreal tailWidth READ tailWidth WRITE setTailWidth NOTIFY tailWidthChanged)
    Q_PROPERTY(qreal tailHeight READ tailHeight WRITE setTailHeight NOTIFY tailHeightChanged)
    Q_PROPERTY(QColor fillTop READ fillTop WRITE setFillTop NOTIFY fillTopChanged)
    Q_PROPERTY(QColor fillBottom READ fillBottom WRITE setFillBottom NOTIFY fillBottomChanged)
    Q_PROPERTY(QColor strokeColor READ strokeColor WRITE setStrokeColor NOTIFY strokeColorChanged)
    // A collectible skin id (e.g. "bubble.aero"). Empty, "classic" or an id this
    // build does not know draws the classic gradient above, unchanged.
    Q_PROPERTY(QString skin READ skin WRITE setSkin NOTIFY skinChanged)
    Q_PROPERTY(bool skinned READ skinned NOTIFY skinChanged)
    // The skin's own message and timestamp colours; invalid when not skinned.
    Q_PROPERTY(QColor skinTextColor READ skinTextColor NOTIFY skinChanged)
    Q_PROPERTY(QColor skinSecondaryTextColor READ skinSecondaryTextColor NOTIFY skinChanged)

public:
    explicit BubbleBackground(QQuickItem *parent = nullptr);

    [[nodiscard]] bool outgoing() const;
    void setOutgoing(bool outgoing);
    [[nodiscard]] qreal radius() const;
    void setRadius(qreal radius);
    [[nodiscard]] qreal tailWidth() const;
    void setTailWidth(qreal width);
    [[nodiscard]] qreal tailHeight() const;
    void setTailHeight(qreal height);
    [[nodiscard]] QColor fillTop() const;
    void setFillTop(const QColor &color);
    [[nodiscard]] QColor fillBottom() const;
    void setFillBottom(const QColor &color);
    [[nodiscard]] QColor strokeColor() const;
    void setStrokeColor(const QColor &color);
    [[nodiscard]] QString skin() const;
    void setSkin(const QString &skin);
    [[nodiscard]] bool skinned() const;
    [[nodiscard]] QColor skinTextColor() const;
    [[nodiscard]] QColor skinSecondaryTextColor() const;

    void paint(QPainter *painter) override;

    static QPainterPath makePath(const QRectF &bounds, bool outgoing, qreal radius,
                                 qreal tailWidth, qreal tailHeight);

signals:
    void outgoingChanged();
    void radiusChanged();
    void tailWidthChanged();
    void tailHeightChanged();
    void fillTopChanged();
    void fillBottomChanged();
    void strokeColorChanged();
    void skinChanged();

private:
    void repaint();
    void paintSkin(QPainter *painter);

    bool m_outgoing = false;
    qreal m_radius = 6.0;
    qreal m_tailWidth = 9.0;
    qreal m_tailHeight = 13.0;
    QColor m_fillTop = QColor(QStringLiteral("#f5fbff"));
    QColor m_fillBottom = QColor(QStringLiteral("#e6f3fb"));
    QColor m_strokeColor = QColor(QStringLiteral("#9ec3de"));
    QString m_skin;
};

} // namespace OpenChat
