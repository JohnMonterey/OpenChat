#pragma once

namespace OpenChat {

// Registers the profile cosmetics' QML types (AvatarFrame, BeadArt, NameFlair,
// ProfileScene and the Cosmetics catalogue) in OpenChat.Native. Called by the
// application, the QML tests and the gallery alike.
void registerCosmeticQmlTypes();

} // namespace OpenChat
