#pragma once

#include <QColor>
#include <QPainterPath>
#include <QQuickPaintedItem>

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

private:
    void repaint();

    bool m_outgoing = false;
    qreal m_radius = 6.0;
    qreal m_tailWidth = 9.0;
    qreal m_tailHeight = 13.0;
    QColor m_fillTop = QColor(QStringLiteral("#f5fbff"));
    QColor m_fillBottom = QColor(QStringLiteral("#e6f3fb"));
    QColor m_strokeColor = QColor(QStringLiteral("#9ec3de"));
};

} // namespace OpenChat
