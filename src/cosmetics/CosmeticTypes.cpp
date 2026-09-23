#include "cosmetics/CosmeticTypes.h"

#include "cosmetics/AvatarFrameItem.h"
#include "cosmetics/BeadArtItem.h"
#include "cosmetics/CosmeticCatalog.h"
#include "cosmetics/NameFlairItem.h"
#include "cosmetics/ProfileSceneItem.h"

#include <qqml.h>

namespace OpenChat {

void registerCosmeticQmlTypes()
{
    qmlRegisterType<AvatarFrame>("OpenChat.Native", 1, 0, "AvatarFrame");
    qmlRegisterType<BeadArt>("OpenChat.Native", 1, 0, "BeadArt");
    qmlRegisterType<NameFlair>("OpenChat.Native", 1, 0, "NameFlair");
    qmlRegisterType<ProfileScene>("OpenChat.Native", 1, 0, "ProfileScene");
    qmlRegisterSingletonType<CosmeticsCatalogObject>(
        "OpenChat.Native", 1, 0, "Cosmetics",
        [](QQmlEngine *, QJSEngine *) -> QObject * { return new CosmeticsCatalogObject; });
}

} // namespace OpenChat
