#include "call/VideoFrameCopy.h"

#include <QScopeGuard>
#include <algorithm>
#include <cstring>

namespace OpenChat {

QVideoFrame copyVideoFrameToMemory(QVideoFrame source)
{
    if (!source.isValid() || !source.map(QVideoFrame::ReadOnly))
        return {};
    const auto unmapSource = qScopeGuard([&] { source.unmap(); });

    QVideoFrame owned(source.surfaceFormat());
    if (!owned.map(QVideoFrame::WriteOnly))
        return {};
    {
        const auto unmapOwned = qScopeGuard([&] { owned.unmap(); });
        if (source.planeCount() != owned.planeCount())
            return {};
        for (int plane = 0; plane < owned.planeCount(); ++plane) {
            const int sourceStride = source.bytesPerLine(plane);
            const int targetStride = owned.bytesPerLine(plane);
            const int targetBytes = owned.mappedBytes(plane);
            if (!source.bits(plane) || !owned.bits(plane)
                || sourceStride <= 0 || targetStride <= 0 || targetBytes <= 0)
                return {};
            // Allocation may include extra chroma rows for alignment. Copy only
            // visible rows: a 32x18 NV12 image has 9 UV rows even when Qt
            // allocates space for 10. Other planar Qt formats are 4:2:0,
            // except YUV422P, whose chroma has the full image height.
            const int rows = plane == 0 || source.pixelFormat() == QVideoFrameFormat::Format_YUV422P
                ? source.height() : (source.height() + 1) / 2;
            const int rowBytes = std::min(sourceStride, targetStride);
            if (rows <= 0 || qint64(rows) * targetStride > targetBytes
                || qint64(rows - 1) * sourceStride + rowBytes > source.mappedBytes(plane))
                return {};
            // Camera buffers can have wider, platform-aligned scanlines than
            // Qt's allocation. Copy each plane row by row, never as one blob.
            std::memset(owned.bits(plane), 0, targetBytes);
            for (int row = 0; row < rows; ++row) {
                std::memcpy(owned.bits(plane) + qint64(row) * targetStride,
                            source.bits(plane) + qint64(row) * sourceStride, rowBytes);
            }
        }
    }
    owned.setStartTime(source.startTime());
    owned.setEndTime(source.endTime());
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    owned.setRotation(source.rotation());
#else
    owned.setRotationAngle(source.rotationAngle());
#endif
    owned.setMirrored(source.mirrored());
    return owned;
}

} // namespace OpenChat
