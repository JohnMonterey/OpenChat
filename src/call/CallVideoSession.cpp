#include "call/CallVideoSession.h"
#include "call/CallMediaPacket.h"
#include <QBuffer>
#include <QImageReader>
#include <QtEndian>
#include <limits>

namespace OpenChat {

CallVideoSession::CallVideoSession(CallId id, CallMediaKeys send, CallMediaKeys receive)
    : m_id(id), m_sealer(std::move(send)), m_opener(std::move(receive)) {}

std::unique_ptr<CallVideoSession> CallVideoSession::create(const CallId &id,
    CallDirection direction, QByteArrayView secret)
{
    const auto keys = CallMediaKeySchedule::deriveVideo(secret, id);
    if (!keys)
        return {};
    return std::unique_ptr<CallVideoSession>(new CallVideoSession(
        id, keys->sendKeys(direction), keys->receiveKeys(direction)));
}

QByteArray CallVideoSession::encode(const QImage &image)
{
    // Exhaustion must never wrap the nonce, even during an exceptionally long call.
    if (m_sequence > std::numeric_limits<quint32>::max())
        return {};
    QByteArray payload;
    if (!image.isNull()) {
        QImage scaled = image.width() > maxDimension || image.height() > maxDimension
            ? image.scaled(maxDimension, maxDimension, Qt::KeepAspectRatio, Qt::SmoothTransformation)
            : image;
        for (int attempt = 0; attempt < 2; ++attempt) {
            payload.clear();
            QBuffer buffer(&payload);
            buffer.open(QIODevice::WriteOnly);
            if (!scaled.save(&buffer, "JPEG", 70))
                return {};
            if (payload.size() <= maxPayloadBytes)
                break;
            // Detailed/noisy cameras still produce a frame within the bandwidth
            // cap. Reduce resolution while retaining their exact proportions.
            scaled = scaled.scaled(maxDimension / 2, maxDimension / 2,
                                   Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        if (payload.size() > maxPayloadBytes)
            return {};
    }
    CallMediaPacket packet;
    packet.version = wireVersion;
    packet.callId = m_id;
    packet.flags = image.isNull() ? 0 : 1;
    packet.sequence = static_cast<quint32>(m_sequence++);
    packet.sealed = m_sealer.seal(packet.sequence, payload, packet.header());
    return packet.sealed.isEmpty() ? QByteArray() : packet.encode();
}

std::optional<QImage> CallVideoSession::decode(QByteArrayView packet)
{
    constexpr auto headerSize = CallMediaPacket::headerBytes;
    if (packet.size() < headerSize + CallMediaSealer::tagBytes
        || packet.size() > headerSize + CallMediaSealer::tagBytes + maxPayloadBytes)
        return std::nullopt;
    const auto *data = reinterpret_cast<const uchar *>(packet.data());
    if (data[0] != wireVersion || data[1] > 1
        || packet.sliced(2, CallId::byteCount) != QByteArrayView(m_id.bytes()))
        return std::nullopt;
    const quint32 sequence = qFromBigEndian<quint32>(data + 18);
    // Display only newer frames; a late frame must not resurrect a stopped camera.
    if (m_lastReceived && sequence <= *m_lastReceived)
        return std::nullopt;
    auto payload = m_opener.open(sequence, packet.sliced(headerSize), packet.first(headerSize));
    if (!payload)
        return std::nullopt;
    if (data[1] == 0) {
        if (!payload->isEmpty())
            return std::nullopt;
        m_lastReceived = sequence;
        return QImage();
    }
    QBuffer buffer(&*payload);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "JPEG");
    const QSize size = reader.size();
    // Validate dimensions before allocating pixels, even from an authenticated peer.
    if (size.isEmpty() || size.width() > maxDimension || size.height() > maxDimension)
        return std::nullopt;
    QImage image = reader.read();
    if (image.isNull())
        return std::nullopt;
    m_lastReceived = sequence;
    return image;
}

} // namespace OpenChat
