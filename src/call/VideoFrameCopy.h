#pragma once

#include <QVideoFrame>

namespace OpenChat {

// Detach raw camera pixels from platform texture handles. A QVideoFrame copy
// alone only shares the original native buffer and its texture-cache lifetime.
// Returns an invalid frame if the source cannot be mapped or safely copied.
[[nodiscard]] QVideoFrame copyVideoFrameToMemory(QVideoFrame source);

} // namespace OpenChat
