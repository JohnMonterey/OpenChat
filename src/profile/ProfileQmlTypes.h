#pragma once

namespace OpenChat {

// Registers the profile page's native types under OpenChat.Native 1.0: the
// Profile enums (uncreatable), the painted items, ProfileTickerClient and the
// song player, and the ProfileRenderPolicy and ProfileTicker singletons (the
// process instances, owned by C++). Call once per process, after
// registerCosmeticQmlTypes(). The bundled fonts are NOT registered here
// (ProfileFonts::ensureRegistered(), when a page first opens), so starting
// the app never loads them.
void registerProfileQmlTypes();

} // namespace OpenChat
