#pragma once
#include "call/ScreenCanvas.h"

#include <QImage>
#include <QQuickItem>
#include <QRect>
#include <QVariant>

namespace OpenChat {

// The one media view on the call screen, for both video sources.
//
// A camera arrives as whole frames and is set through `frame`. A screen arrives
// on a ScreenCanvas — the shared surface itself, not a copy of its pixels — set
// through `canvas`.
//
// Each new picture is uploaded to the GPU once, as a texture, and the GPU
// scales it into the item: scaling a 1080p desktop into a panel on the CPU
// thirty times a second is exactly the work a screen share cannot afford. A
// share shown much smaller than it is gets mipmaps, so text shrinks smoothly
// instead of shimmering. Nothing is uploaded for a picture that has not
// changed, a paused view, or one that is not visible.
//
// The rounded corners are geometry, not a clip. On Qt's software renderer
// (headless tests, machines without a usable GPU) the view falls back to a
// plain image node with square corners.
class CallVideoItem : public QQuickItem
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
    // A paused view keeps its source bound but stops uploading for it: the
    // tile an enlarged copy was opened from sits under the scrim, where nobody
    // can see it move, so it costs nothing while the copy does the moving. It
    // catches up the moment it is resumed.
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

signals:
    void frameChanged();
    void canvasChanged();
    void mirroredChanged();
    void sourceAspectChanged();
    void pausedChanged();
    void sourceChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    void applyCanvas(ScreenCanvasPtr next);
    void requestRepaint();
    [[nodiscard]] QRectF targetRect(QSize sourceSize) const;
    [[nodiscard]] const QImage *sourceImage() const;

    QImage m_frame;
    ScreenCanvasPtr m_canvas;
    QSize m_lastSourceSize;
    bool m_mirrored = false;
    bool m_paused = false;
    CallVideoItem *m_source = nullptr;

    // What the node's texture currently shows, so an unchanged picture is
    // never uploaded again. Touched only in updatePaintNode, on the render
    // thread, while the GUI thread is blocked.
    // A canvas revision is unique across canvases, so it alone says which
    // picture of which canvas is on the GPU; 0 means a camera frame, or nothing.
    quint64 m_uploadedRevision = 0;
    qint64 m_uploadedFrameKey = 0;
    bool m_uploadedMipmaps = false;
};

} // namespace OpenChat
