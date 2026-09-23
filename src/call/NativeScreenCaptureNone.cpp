#include "call/NativeScreenCapture.h"

#include "call/QtScreenCapture.h"

// Platforms whose capture is Qt's own (Linux: the distributions' Qt ships the
// FFmpeg backend, with X11 and PipeWire capture). Nothing native to offer, and
// nothing to ask permission for.
namespace OpenChat::NativeScreenCapturePlatform {

bool isAvailable()
{
    return false;
}

QString name()
{
    return {};
}

QVector<ScreenShareSource> sources()
{
    return {};
}

std::unique_ptr<NativeScreenCapture> create(const ScreenShareSource &)
{
    return nullptr;
}

ScreenCaptureAccess access(bool)
{
    return ScreenCaptureAccess::Granted;
}

QString accessDeniedMessage()
{
    return {};
}

bool openAccessSettings()
{
    return false;
}

} // namespace OpenChat::NativeScreenCapturePlatform
