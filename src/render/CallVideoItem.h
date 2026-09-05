#pragma once
#include <QImage>
#include <QQuickPaintedItem>

namespace OpenChat {

class CallVideoItem : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(QImage frame READ frame WRITE setFrame NOTIFY frameChanged)
    Q_PROPERTY(bool mirrored READ mirrored WRITE setMirrored NOTIFY mirroredChanged)
public:
    explicit CallVideoItem(QQuickItem *parent = nullptr) : QQuickPaintedItem(parent) {}
    QImage frame() const { return m_frame; }
    void setFrame(const QImage &frame) { m_frame = frame; update(); emit frameChanged(); }
    bool mirrored() const { return m_mirrored; }
    void setMirrored(bool value) { m_mirrored = value; update(); emit mirroredChanged(); }
    void paint(QPainter *painter) override;
signals:
    void frameChanged();
    void mirroredChanged();
private:
    QImage m_frame;
    bool m_mirrored = false;
};

} // namespace OpenChat
