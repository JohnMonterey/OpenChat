#include "controllers/ChatAttachmentDemo.h"

#include "domain/ProfilePageCodec.h"
#include "media/SongCodec.h"
#include "profile/ClipCodec.h"
#include "profile/ProfileBackgroundImage.h"

#include <QColor>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace OpenChat::ChatAttachmentDemo {

QImage landscape(QSize size, qreal phase, bool night)
{
    QImage image(size, QImage::Format_RGB32);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal w = size.width();
    const qreal h = size.height();
    QLinearGradient sky(0, 0, 0, h);
    sky.setColorAt(0.0, night ? QColor(0x14, 0x1d, 0x3a) : QColor(0x2f, 0x5f, 0xa8));
    sky.setColorAt(0.55, night ? QColor(0x4a, 0x3a, 0x6e) : QColor(0xf0, 0xa8, 0x72));
    sky.setColorAt(1.0, night ? QColor(0x8a, 0x5a, 0x7a) : QColor(0xf6, 0xd3, 0x9c));
    painter.fillRect(image.rect(), sky);

    const QPointF sun(w * (0.25 + 0.5 * phase), h * (0.5 - 0.12 * std::sin(phase * std::numbers::pi)));
    const qreal glowRadius = h * 0.4;
    QRadialGradient glow(sun, glowRadius);
    glow.setColorAt(0.0, QColor(255, 244, 214, 190));
    glow.setColorAt(1.0, QColor(255, 244, 214, 0));
    painter.setPen(Qt::NoPen);
    painter.setBrush(glow);
    painter.drawEllipse(sun, glowRadius, glowRadius);
    painter.setBrush(night ? QColor(0xf2, 0xee, 0xe0) : QColor(0xff, 0xf1, 0xc9));
    painter.drawEllipse(sun, h * 0.07, h * 0.07);

    const QColor ranges[] = {night ? QColor(0x3a, 0x33, 0x5c) : QColor(0x8c, 0x6f, 0x9e),
                             night ? QColor(0x27, 0x25, 0x46) : QColor(0x5b, 0x4d, 0x7e),
                             night ? QColor(0x17, 0x18, 0x30) : QColor(0x33, 0x33, 0x58)};
    for (int layer = 0; layer < 3; ++layer) {
        QPainterPath hills;
        const qreal base = h * (0.62 + 0.08 * layer);
        hills.moveTo(0, h);
        for (int x = 0; x <= 48; ++x) {
            const qreal t = qreal(x) / 48.0;
            const qreal y = base - h * 0.07 * std::sin(t * (5.0 + 2.0 * layer) + layer * 1.7)
                            - h * 0.04 * std::sin(t * 13.0 + layer);
            hills.lineTo(t * w, y);
        }
        hills.lineTo(w, h);
        hills.closeSubpath();
        painter.setBrush(ranges[layer]);
        painter.drawPath(hills);
    }
    QLinearGradient lake(0, h * 0.84, 0, h);
    lake.setColorAt(0.0, night ? QColor(0x2c, 0x2f, 0x55) : QColor(0x6d, 0x8f, 0xc4));
    lake.setColorAt(1.0, night ? QColor(0x10, 0x14, 0x2a) : QColor(0x2e, 0x4d, 0x86));
    painter.fillRect(QRectF(0, h * 0.84, w, h * 0.16), lake);
    painter.end();
    return image;
}

QByteArray preview(const QImage &image)
{
    const QImage scaled = image.scaled(AttachmentLimits::maxPreviewSide, AttachmentLimits::maxPreviewSide,
                                       Qt::KeepAspectRatio, Qt::SmoothTransformation);
    for (const int quality : {72, 58, 44, 30}) {
        const QByteArray jpeg = encodeBaselineJpeg(scaled, quality);
        if (!jpeg.isEmpty() && previewIsAcceptable(jpeg))
            return jpeg;
    }
    return {};
}

AttachmentDescriptor descriptor(AttachmentKind kind, const QByteArray &blob, const QString &fileName,
                                const QString &mimeType)
{
    AttachmentDescriptor descriptor;
    descriptor.key = QByteArray(AttachmentLimits::keyBytes, '\0');
    descriptor.kind = kind;
    descriptor.byteCount = blob.size();
    descriptor.sha256 = pageMediaHash(blob);
    descriptor.partCount = attachmentPartCount(descriptor.byteCount);
    descriptor.fileName = sanitizeAttachmentFileName(fileName);
    descriptor.mimeType = mimeType;
    return descriptor;
}

Song song()
{
    constexpr int rate = 48'000;
    constexpr int seconds = 6;
    WavAudio pcm;
    pcm.sampleRate = rate;
    pcm.channels = 1;
    pcm.samples.resize(rate * seconds);
    const double notes[] = {261.63, 329.63, 392.00, 523.25, 392.00, 329.63}; // C major, up and down
    constexpr int noteSamples = rate * seconds / 6;
    for (int i = 0; i < pcm.samples.size(); ++i) {
        const int note = std::min(i / noteSamples, 5);
        const double t = double(i % noteSamples) / rate;
        const double envelope = std::exp(-2.2 * t) * std::min(1.0, t * 200.0);
        const double tone = std::sin(2.0 * std::numbers::pi * notes[note] * i / rate)
                            + 0.3 * std::sin(4.0 * std::numbers::pi * notes[note] * i / rate);
        pcm.samples[i] = qint16(std::clamp(tone * envelope * 9'000.0, -32'000.0, 32'000.0));
    }
    Song song;
    auto encoded = encodeSong(SongClip{pcm, false, false}, chatSongEncodeOptions());
    if (!encoded.hasValue())
        return song;
    song.container = encoded.value();
    song.durationMs = pcm.durationMs();
    const qsizetype bucket = pcm.samples.size() / AttachmentLimits::maxPeaks;
    int loudest = 1;
    QVector<int> maxima;
    for (int b = 0; b < AttachmentLimits::maxPeaks; ++b) {
        int peak = 0;
        for (qsizetype i = b * bucket; i < (b + 1) * bucket; ++i)
            peak = std::max(peak, std::abs(int(pcm.samples.at(i))));
        maxima.append(peak);
        loudest = std::max(loudest, peak);
    }
    for (const int peak : maxima)
        song.peaks.append(char(quint8(std::clamp(peak * 255 / loudest, 0, 255))));
    return song;
}

QByteArray video(QSize size)
{
    if (!clipCodecAvailable())
        return {};
    constexpr int fps = 15;
    constexpr int frames = 4 * fps;
    QVector<QImage> pictures;
    pictures.reserve(frames);
    for (int frame = 0; frame < frames; ++frame)
        pictures.append(landscape(size, qreal(frame) / frames, true).convertToFormat(QImage::Format_RGB32));
    const auto video = encodeClipVideo(pictures, fps, {240, 150, 85, 60}, 224 * 1024);
    if (!video)
        return {};
    const QVector<QByteArray> segments =
        packClip({*video}, {qint64(frames) * 1000 / fps}, size, fps, ClipAudio{}, 224 * 1024);
    return segments.isEmpty() ? QByteArray() : encodeVideoSequence(segments);
}

} // namespace OpenChat::ChatAttachmentDemo
