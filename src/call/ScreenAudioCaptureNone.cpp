// Platforms without a way to capture a share's sound yet (macOS, and Linux
// builds without the PulseAudio client library). Shares go out silent, and
// the picker says why.

#include "call/ScreenAudioCapture.h"

namespace OpenChat::ScreenAudioCapturePlatform {

bool isSupported()
{
    return false;
}

QString unsupportedReason()
{
#if defined(Q_OS_MACOS)
    return QStringLiteral("Sharing sound is not available on macOS yet.");
#else
    return QStringLiteral("This build of OpenChat cannot share sound (it was built without the "
                          "PulseAudio client library).");
#endif
}

std::unique_ptr<ScreenAudioCapture> create(const ScreenAudioTarget &)
{
    return nullptr;
}

QString backendName()
{
    return QStringLiteral("none");
}

} // namespace OpenChat::ScreenAudioCapturePlatform
