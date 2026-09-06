#pragma once

#include <QImage>
#include <QMetaType>
#include <QRect>
#include <QSize>

#include <memory>

namespace OpenChat {

// The single pixel surface one received screen share is reconstructed into.
//
// A screen share arrives as tiles, not as whole frames, so the receiver needs a
// canvas that persists between packets. Handing that canvas to the UI as a
// QImage would defeat the point: QImage is copy-on-write, so the moment a view
// holds one the next tile written detaches it and copies the whole desktop —
// several megabytes per frame, for a change that may be one 128x128 square.
//
// So the canvas is a shared, mutable object instead of a shared value. Everyone
// who displays it holds a ScreenCanvasPtr, which keeps the pixels alive without
// ever copying them, and the session writes tiles straight into the one buffer.
// The geometry is fixed for the object's life — a resolution change produces a
// NEW canvas rather than reallocating this one — so a holder's pointer can never
// be left pointing at freed pixels.
//
// Ownership is single-threaded by construction: the canvas is written on the GUI
// thread (the call engine's thread) and read in QQuickPaintedItem::paint(),
// which Qt runs with the GUI thread blocked in the scene-graph sync. Nothing
// else may touch it.
class ScreenCanvas final
{
public:
    explicit ScreenCanvas(QSize size)
        : m_image(size.isEmpty() ? QImage() : QImage(size, QImage::Format_RGB32))
    {
        if (!m_image.isNull())
            m_image.fill(Qt::black);
    }

    // A canvas that already holds a picture, for the preview and capture paths
    // that show a share without one arriving. The pixels are detached on the
    // way in, so the canvas owns them alone exactly as it would otherwise.
    explicit ScreenCanvas(QImage picture)
        : m_image(picture.format() == QImage::Format_RGB32
                      ? std::move(picture)
                      : picture.convertToFormat(QImage::Format_RGB32))
    {
        if (!m_image.isNull() && !m_image.isDetached())
            m_image = m_image.copy();
        m_complete = !m_image.isNull();
    }

    ScreenCanvas(const ScreenCanvas &) = delete;
    ScreenCanvas &operator=(const ScreenCanvas &) = delete;

    [[nodiscard]] const QImage &image() const noexcept { return m_image; }
    [[nodiscard]] QSize size() const { return m_image.size(); }
    [[nodiscard]] bool isEmpty() const { return m_image.isNull(); }

    // Bumped once per applied update. A view that painted revision N and is
    // asked to paint N+1 only has to repaint dirtyRect(); any other jump means
    // it missed an update and must repaint everything.
    [[nodiscard]] quint64 revision() const noexcept { return m_revision; }
    [[nodiscard]] QRect dirtyRect() const noexcept { return m_dirty; }

    // True once every tile has been written at least once. Until then the canvas
    // is still filling in from the sender's first sweep and the view fades it,
    // rather than showing black squares as if the desktop had holes in it.
    [[nodiscard]] bool isComplete() const noexcept { return m_complete; }

private:
    friend class CallScreenSession;

    // Non-const pixel access, for the session that owns the reconstruction.
    [[nodiscard]] uchar *scanLine(int y) { return m_image.scanLine(y); }
    [[nodiscard]] qsizetype bytesPerLine() const { return m_image.bytesPerLine(); }

    void beginUpdate() { m_dirty = QRect(); }
    void addDirty(const QRect &rect) { m_dirty = m_dirty.isNull() ? rect : m_dirty.united(rect); }
    void endUpdate(bool complete)
    {
        m_complete = m_complete || complete;
        ++m_revision;
    }

    QImage m_image;
    QRect m_dirty;
    quint64 m_revision = 0;
    bool m_complete = false;
};

using ScreenCanvasPtr = std::shared_ptr<ScreenCanvas>;

} // namespace OpenChat

Q_DECLARE_METATYPE(OpenChat::ScreenCanvasPtr)
