#include "profile/ProfileQmlTypes.h"

#include "domain/ProfilePage.h"
#include "profile/ProfileAmbientItem.h"
#include "profile/ProfileBackdropItem.h"
#include "profile/ProfileImageLayerItem.h"
#include "profile/ProfileMoodFaceItem.h"
#include "profile/ProfileNameTextItem.h"
#include "profile/ProfilePresetThumbItem.h"
#include "profile/ProfileRenderPolicy.h"
#include "profile/ProfileTicker.h"

// The song player belongs to the song unit, which lands beside this one; it
// is registered here as soon as its header is part of the tree.
#if __has_include("profile/SongPlayer.h")
#include "profile/SongPlayer.h"
#define OPENCHAT_HAS_SONG_PLAYER 1
#endif

#include <QQmlEngine>

namespace OpenChat {

namespace {

// The process instances, handed to every engine and never owned by one.
template<typename T>
QObject *processInstance()
{
    QObject *instance = &T::instance();
    QQmlEngine::setObjectOwnership(instance, QQmlEngine::CppOwnership);
    return instance;
}

} // namespace

void registerProfileQmlTypes()
{
    qmlRegisterUncreatableMetaObject(Profile::staticMetaObject, "OpenChat.Native", 1, 0, "Profile",
                                     QStringLiteral("Profile holds enums only"));
    qmlRegisterType<ProfileBackdrop>("OpenChat.Native", 1, 0, "ProfileBackdrop");
    qmlRegisterType<ProfileImageLayer>("OpenChat.Native", 1, 0, "ProfileImageLayer");
    qmlRegisterType<ProfileNameText>("OpenChat.Native", 1, 0, "ProfileNameText");
    qmlRegisterType<ProfileAmbient>("OpenChat.Native", 1, 0, "ProfileAmbient");
    qmlRegisterType<ProfilePresetThumb>("OpenChat.Native", 1, 0, "ProfilePresetThumb");
    qmlRegisterType<ProfileMoodFace>("OpenChat.Native", 1, 0, "ProfileMoodFace");
    qmlRegisterType<ProfileTickerClient>("OpenChat.Native", 1, 0, "ProfileTickerClient");
#ifdef OPENCHAT_HAS_SONG_PLAYER
    qmlRegisterType<SongPlayer>("OpenChat.Native", 1, 0, "SongPlayer");
#endif
    qmlRegisterSingletonType<ProfileRenderPolicy>(
        "OpenChat.Native", 1, 0, "ProfileRenderPolicy",
        [](QQmlEngine *, QJSEngine *) { return processInstance<ProfileRenderPolicy>(); });
    qmlRegisterSingletonType<ProfileTicker>("OpenChat.Native", 1, 0, "ProfileTicker",
                                            [](QQmlEngine *, QJSEngine *) { return processInstance<ProfileTicker>(); });
}

} // namespace OpenChat
