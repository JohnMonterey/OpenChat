#pragma once
#include "call/ScreenCanvas.h"

#include <QImage>
#include <QQuickPaintedItem>
#include <QRect>
#include <QVariant>

namespace OpenChat {

// The one media view on the call screen, for both video sources.
//
// A camera arrives as whole frames and is set through `frame`. A screen arrives
// as tiles written into a surface that outlives any one packet, and is set
// through `canvas` — the shared ScreenCanvas itself, not a copy of its pixels,
// because copying a desktop into a QImage property would cost several megabytes
// on every update for a change that is usually one small rectangle.
//
// Setting a canvas repaints only the rectangle that actually changed, and the
// scene graph re-uploads only that part of the texture. The texture is the size
// of THIS ITEM, not of the far end's display: a 4K share in a 400 px panel costs
// 400 px worth of video memory.
class CallVideoItem : public QQuickPaintedItem
{
    Q_OBJECT
    Q_PROPERTY(QImage frame READ frame WRITE setFrame NOTIFY frameChanged)
    // Holds a ScreenCanvasPtr. QML only ever passes it along.
    Q_PROPERTY(QVariant canvas READ canvas WRITE setCanvas NOTIFY canvasChanged)
    Q_PROPERTY(bool mirrored READ mirrored WRITE setMirrored NOTIFY mirroredChanged)
    // The natural aspect of whatever is being shown, so the layout can size a
    // slot for it without the caller having to know which source it came from.
    Q_PROPERTY(double sourceAspect READ sourceAspect NOTIFY sourceAspectChanged)
    // True while a screen share is still filling in its first sweep.
    Q_PROPERTY(bool canvasComplete READ canvasComplete NOTIFY canvasChanged)
    // A paused view keeps its source bound but stops repainting for it: the
    // tile an enlarged copy was opened from sits under the scrim, where nobody
    // can see it move, so it costs nothing while the copy does the moving. It
    // repaints itself whole the moment it is resumed.
    Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY pausedChanged)
    // Another view to show the same thing as. A copy follows its source's
    // frame, canvas and mirroring in C++, straight from signal to setter, so
    // an enlarged camera does not push thirty frames a second through a QML
    // binding and an enlarged share never leaves a desktop-sized pointer on the
    // JavaScript heap waiting for a garbage collection.
    Q_PROPERTY(OpenChat::CallVideoItem *source READ source WRITE setSource NOTIFY sourceChanged)

public:
    explicit CallVideoItem(QQuickItem *parent = nullptr);

    [[nodiscard]] QImage frame() const { return m_frame; }
    void setFrame(const QImage &frame);
    [[nodiscard]] QVariant canvas() const { return QVariant::fromValue(m_canvas); }
    void setCanvas(const QVariant &value);
    [[nodiscard]] bool mirrored() const { return m_mirrored; }
    void setMirrored(bool value);
    [[nodiscard]] double sourceAspect() const;
    [[nodiscard]] bool canvasComplete() const { return m_canvas && m_canvas->isComplete(); }
    [[nodiscard]] bool paused() const { return m_paused; }
    void setPaused(bool value);
    [[nodiscard]] CallVideoItem *source() const { return m_source; }
    void setSource(CallVideoItem *source);

    void paint(QPainter *painter) override;

signals:
    void frameChanged();
    void canvasChanged();
    void mirroredChanged();
    void sourceAspectChanged();
    void pausedChanged();
    void sourceChanged();

private:
    void applyCanvas(ScreenCanvasPtr next);
    // The item-space rectangle a canvas-space rectangle lands in, inflated by a
    // pixel so smooth scaling cannot leave a seam at the edge of the repaint.
    [[nodiscard]] QRect mapFromSource(const QRect &rect) const;
    [[nodiscard]] QRectF targetRect(QSize sourceSize) const;
    [[nodiscard]] const QImage *sourceImage() const;

    QImage m_frame;
    ScreenCanvasPtr m_canvas;
    QSize m_lastSourceSize;
    bool m_mirrored = false;
    bool m_paused = false;
    CallVideoItem *m_source = nullptr;
};

} // namespace OpenChat
