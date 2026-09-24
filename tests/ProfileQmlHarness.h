#pragma once

// The one place a test that hosts the app's QML (or the profile controller)
// registers the app's native types, so the suites that load Main.qml or the
// profile pages (tst_qmlload, tst_profilepageqml, tst_profileeditor,
// tst_profilecontroller) never drift apart. It registers exactly what
// src/main.cpp registers for the chat window, plus the profile types, and runs
// the suite offscreen on the software scene graph with its settings in a
// throwaway directory.
//
// Use OPENCHAT_PROFILE_QML_TEST_MAIN(MyTest) at the end of the test file, or
// OpenChat::ProfileQmlHarness::exec<MyTest>(argc, argv, options) from a main()
// of its own; call resetProfileSingletons() from the suite's init(). The
// sources and libraries this needs are the CMake lists
// profile_qml_test_sources and profile_qml_test_libs.

#include "app/AppearanceSettings.h"
#include "app/ComposerEditing.h"
#include "app/MemorySettings.h"
#include "app/MicrophoneSettings.h"
#include "app/TextLineSpacing.h"
#include "app/TransportSettings.h"
#include "app/VoiceEffectHost.h"
#include "case/DailyCaseController.h"
#include "controllers/ChatController.h"
#include "controllers/ContactController.h"
#include "controllers/OnboardingController.h"
#include "controllers/VoiceDebugController.h"
#include "cosmetics/CosmeticTypes.h"
#include "profile/ProfileFonts.h"
#include "profile/ProfileMediaStore.h"
#include "profile/ProfileQmlTypes.h"
#include "profile/ProfileRenderPolicy.h"
#include "profile/SongPlayer.h"
#include "render/AvatarArtwork.h"
#include "render/BubbleBackground.h"
#include "render/CallVideoItem.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlEngine>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest/QtTest>

namespace OpenChat::ProfileQmlHarness {

struct Options final {
    // Names the settings file (a fresh one in a temporary directory).
    const char *applicationName = "qml-profile";
    // The bundled profile faces. A suite that checks they are registered
    // only when a page first opens turns this off.
    bool registerFonts = true;
};

// Every native QML type the chat window uses, and the profile types.
inline void registerNativeTypes()
{
    qmlRegisterType<DailyCaseController>("OpenChat.Native", 1, 0, "DailyCaseController");
    qmlRegisterSingletonType<AppearanceSettings>(
        "OpenChat.Native", 1, 0, "AppearanceSettings",
        [](QQmlEngine *, QJSEngine *) -> QObject * { return new AppearanceSettings; });
    qmlRegisterSingletonType<MicrophoneSettings>(
        "OpenChat.Native", 1, 0, "MicrophoneSettings",
        [](QQmlEngine *, QJSEngine *) -> QObject * { return new MicrophoneSettings; });
    qmlRegisterSingletonType<VoiceEffectHost>(
        "OpenChat.Native", 1, 0, "VoiceEffectHost",
        [](QQmlEngine *, QJSEngine *) -> QObject * { return new VoiceEffectHost; });
    qmlRegisterSingletonType<MemorySettings>(
        "OpenChat.Native", 1, 0, "MemorySettings",
        [](QQmlEngine *, QJSEngine *) -> QObject * { return new MemorySettings; });
    qmlRegisterSingletonType<TransportSettings>(
        "OpenChat.Native", 1, 0, "TransportSettings",
        [](QQmlEngine *, QJSEngine *) -> QObject * { return new TransportSettings; });
    qmlRegisterType<BubbleBackground>("OpenChat.Native", 1, 0, "BubbleBackground");
    qmlRegisterType<CallVideoItem>("OpenChat.Native", 1, 0, "CallVideoItem");
    qmlRegisterType<AvatarArtwork>("OpenChat.Native", 1, 0, "AvatarArtwork");
    qmlRegisterType<ComposerEditing>("OpenChat.Native", 1, 0, "ComposerEditing");
    qmlRegisterType<TextLineSpacing>("OpenChat.Native", 1, 0, "TextLineSpacing");
    registerCosmeticQmlTypes();
    registerProfileQmlTypes();
    qmlRegisterUncreatableType<ChatController>("OpenChat.Native", 1, 0, "ChatController",
                                               QStringLiteral("ChatController is provided by the application"));
    qmlRegisterUncreatableType<OnboardingController>(
        "OpenChat.Native", 1, 0, "OnboardingController",
        QStringLiteral("OnboardingController is provided by the application"));
    qmlRegisterUncreatableType<ContactController>(
        "OpenChat.Native", 1, 0, "ContactController",
        QStringLiteral("ContactController is provided by the application"));
    qmlRegisterUncreatableType<VoiceDebugController>(
        "OpenChat.Native", 1, 0, "VoiceDebugController",
        QStringLiteral("VoiceDebugController is provided by the application"));
}

// The process-wide profile state one test could leave behind for the next:
// Low memory mode and reduced motion, decoded pictures, and song bytes.
inline void resetProfileSingletons()
{
    ProfileRenderPolicy::instance().resetForTesting();
    ProfileMediaStore::instance().resetForTesting();
    SongLibrary::instance().clear();
}

template<typename Test>
int exec(int argc, char **argv, const Options &options = {})
{
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    qputenv("QT_QUICK_BACKEND", QByteArrayLiteral("software"));
    QGuiApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("OpenChatTests"));
    QCoreApplication::setApplicationName(QString::fromLatin1(options.applicationName));
    // Settings (recent colours, the last editor tab, appearance) and app data
    // never touch the developer's own.
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settingsDirectory;
    qputenv("XDG_DATA_HOME", settingsDirectory.path().toUtf8());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    registerNativeTypes();
    if (options.registerFonts)
        ProfileFonts::ensureRegistered();
    Test test;
    return QTest::qExec(&test, argc, argv);
}

} // namespace OpenChat::ProfileQmlHarness

// main() for a QML-hosting suite; optional arguments are ProfileQmlHarness::Options fields in order.
#define OPENCHAT_PROFILE_QML_TEST_MAIN(TestClass, ...)                                                          \
    int main(int argc, char **argv)                                                                             \
    {                                                                                                           \
        return OpenChat::ProfileQmlHarness::exec<TestClass>(argc, argv,                                         \
                                                            OpenChat::ProfileQmlHarness::Options{__VA_ARGS__}); \
    }
