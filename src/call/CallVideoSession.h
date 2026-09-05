#pragma once

#include "call/CallMediaCrypto.h"
#include <QImage>
#include <memory>

namespace OpenChat {

// Independently decodable JPEG frames over the existing ephemeral media path.
// Loss never requires waiting for a keyframe. Version 2 distinguishes video
// from the unchanged version 1 audio packets, including on older voice clients.
class CallVideoSession final
{
public:
    static constexpr quint8 wireVersion = 2;
    static constexpr int maxDimension = 640;
    static constexpr int maxPayloadBytes = 96 * 1024;
    static std::unique_ptr<CallVideoSession> create(const CallId &id, CallDirection direction,
                                                    QByteArrayView secret);
    // Null image means camera off. A disengaged receive result means rejection;
    // an engaged null image is an authenticated camera-off update.
    QByteArray encode(const QImage &image);
    std::optional<QImage> decode(QByteArrayView packet);

private:
    CallVideoSession(CallId id, CallMediaKeys send, CallMediaKeys receive);
    CallId m_id;
    CallMediaSealer m_sealer;
    CallMediaOpener m_opener;
    quint64 m_sequence = 0;
    std::optional<quint32> m_lastReceived;
};

} // namespace OpenChat
