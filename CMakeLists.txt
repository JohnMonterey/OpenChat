cmake_minimum_required(VERSION 3.21)

project(OpenChat VERSION 0.1.0 LANGUAGES C CXX)

# The macOS notification backend is Objective-C++ (UNUserNotificationCenter has
# no C interface). Enabled here rather than beside the target, because a
# language must be enabled in the top-level scope before anything uses it.
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core DBus Gui HttpServer Multimedia Network Quick QuickControls2 Sql Svg Test WebSockets)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)

include(cmake/dependencies/Opus.cmake)
include(cmake/dependencies/QtKeychain.cmake)
include(cmake/dependencies/SqlCipher.cmake)
include(cmake/OpenChatRust.cmake)
add_openchat_rust_library()

qt_standard_project_setup(REQUIRES 6.5)
if(Qt6_VERSION VERSION_GREATER_EQUAL 6.8)
    qt_policy(SET QTP0004 NEW)
endif()

set_source_files_properties(qml/OpenChat/Theme.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE
)

qt_add_executable(OpenChat
    src/main.cpp
    src/controllers/ChatController.cpp
    src/controllers/ContactController.cpp
    src/controllers/OnboardingController.cpp
    src/controllers/CallController.cpp
    src/models/ContactListModel.cpp
    src/models/MessageListModel.cpp
    src/models/CallParticipantModel.cpp
    src/models/RequestListModel.cpp
    src/render/BubbleBackground.cpp
    src/render/AvatarArtwork.cpp
    src/render/CallVideoItem.cpp
    src/app/AppearanceSettings.cpp
    src/app/MicrophoneSettings.cpp
)

if(APPLE)
    configure_file(cmake/Info.plist.in ${CMAKE_CURRENT_BINARY_DIR}/OpenChat-Info.plist @ONLY)
    # Declare usage strings to Qt's permission-plugin finalizer and embed them
    # in the existing standalone executable as well as any future app bundle.
    set_target_properties(OpenChat PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_BINARY_DIR}/OpenChat-Info.plist)
    target_link_options(OpenChat PRIVATE
        "LINKER:-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_BINARY_DIR}/OpenChat-Info.plist")
    set_property(TARGET OpenChat APPEND PROPERTY LINK_DEPENDS
        ${CMAKE_CURRENT_BINARY_DIR}/OpenChat-Info.plist)
endif()

qt_add_qml_module(OpenChat
    URI OpenChat
    VERSION 1.0
    RESOURCE_PREFIX /qt/qml
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/qml/OpenChat
    QML_FILES
        qml/OpenChat/Main.qml
        qml/OpenChat/Onboarding.qml
        qml/OpenChat/Theme.qml
        qml/OpenChat/components/PresenceBead.qml
        qml/OpenChat/components/Avatar.qml
        qml/OpenChat/components/AeroTextField.qml
        qml/OpenChat/components/AeroButton.qml
        qml/OpenChat/components/AeroSwitch.qml
        qml/OpenChat/components/AeroSlider.qml
        qml/OpenChat/components/MicrophoneSettingsPanel.qml
        qml/OpenChat/components/ContactSidebar.qml
        qml/OpenChat/components/ContactCategory.qml
        qml/OpenChat/components/ContactRow.qml
        qml/OpenChat/components/ContactStatusBubble.qml
        qml/OpenChat/components/AddContactDialog.qml
        qml/OpenChat/components/SafetyNumberDialog.qml
        qml/OpenChat/components/RequestRow.qml
        qml/OpenChat/components/ConversationHeader.qml
        qml/OpenChat/components/GroupMemberPicker.qml
        qml/OpenChat/components/CallParticipant.qml
        qml/OpenChat/components/CallActionButton.qml
        qml/OpenChat/components/CallHeader.qml
        qml/OpenChat/components/ScreenShareStage.qml
        qml/OpenChat/components/ScreenSharePicker.qml
        qml/OpenChat/components/MediaIconButton.qml
        qml/OpenChat/components/MediaZoomOverlay.qml
        qml/OpenChat/components/MessageDelegate.qml
        qml/OpenChat/components/MessageHistory.qml
        qml/OpenChat/components/Composer.qml
    RESOURCES
        assets/Jessica.png
        assets/alex.png
        assets/michael.png
        assets/ryan.png
        assets/userpfp_none.png
        assets/icons/video-call.svg
        assets/icons/video-call-dark.svg
        assets/icons/phone-call.svg
        assets/icons/phone-call-dark.svg
        assets/icons/settings.svg
        assets/icons/settings-dark.svg
        assets/icons/chevron-down.svg
        assets/icons/chevron-down-dark.svg
        assets/icons/openchat.png
)

target_include_directories(OpenChat PRIVATE src)
target_link_libraries(OpenChat PRIVATE
    openchat_app
    openchat_avatar
    openchat_call
    openchat_notify
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Svg
)

if(UNIX AND NOT APPLE)
    # The desktop entry is how a freedesktop desktop learns OpenChat's name and
    # icon. Notifications carry its basename in the "desktop-entry" hint, which
    # is what puts the application's icon on the notification and its name in
    # the notification settings. Nothing at build or run time depends on this
    # being installed; without it the desktop falls back to the plain name.
    include(GNUInstallDirs)
    install(TARGETS OpenChat RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES deploy/openchat.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
    install(FILES assets/icons/openchat.png
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/256x256/apps
        RENAME openchat.png)
endif()

# Posts one real desktop notification through the application's own code. Not
# part of the application: it is how the platform backends are checked by hand
# on a desktop session, which no automated test can do.
add_executable(openchat-notify-check tools/notify_check.cpp)
target_include_directories(openchat-notify-check PRIVATE src)
target_link_libraries(openchat-notify-check PRIVATE
    openchat_notify
    openchat_avatar
    Qt6::Core
    Qt6::Gui
)

# Renders the generated call sounds to WAV. Not part of the application: it is
# how the sounds are auditioned and handed over, since they exist as code rather
# than as bundled audio files.
add_executable(openchat-render-call-sounds tools/render_call_sounds.cpp)
target_include_directories(openchat-render-call-sounds PRIVATE src)
target_link_libraries(openchat-render-call-sounds PRIVATE
    openchat_call
    openchat_media
    Qt6::Core
)

# Runs one side of a real call against a real relay, so the two-machine question
# can be answered by actually doing it rather than by inference.
add_executable(openchat-call-check tools/call_check.cpp)
target_include_directories(openchat-call-check PRIVATE src)
target_link_libraries(openchat-call-check PRIVATE
    openchat_app
    openchat_call
    openchat_media
    openchat_network
    openchat_crypto
    openchat_storage
    openchat_security
    openchat_protocol
    openchat_domain
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
)

enable_testing()

add_executable(tst_appmetadata tests/tst_appmetadata.cpp)
target_include_directories(tst_appmetadata PRIVATE src)
target_link_libraries(tst_appmetadata PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_appmetadata COMMAND tst_appmetadata)

add_library(openchat_domain STATIC
    src/diagnostics/Logging.cpp
    src/domain/Identifiers.cpp
    src/domain/ProfileUpdate.cpp
    src/domain/GroupUpdate.cpp
)
target_include_directories(openchat_domain PUBLIC src)
target_link_libraries(openchat_domain PUBLIC Qt6::Core)

# Profile pictures: the file -> bounded JPEG pipeline a user's chosen picture
# goes through before it is stored or sent, and the in-process registry the
# avatar artwork draws received pictures from. Needs Qt Gui for QImage, which
# is why it is not part of openchat_domain.
add_library(openchat_avatar STATIC
    src/render/ProfileImage.cpp
    src/render/AvatarStore.cpp
    src/render/AvatarPainter.cpp
)
target_include_directories(openchat_avatar PUBLIC src)
target_link_libraries(openchat_avatar PUBLIC openchat_domain Qt6::Core Qt6::Gui)

# Desktop notifications. The policy (what is worth announcing, and what it says)
# is platform-neutral and tested on its own; exactly one backend is compiled in,
# chosen by the platform this binary targets. On the freedesktop desktops that
# backend is a D-Bus client, so the same code serves Wayland and X11.
set(openchat_notify_sources
    src/notify/NotificationService.cpp
    src/notify/NotificationBackendFactory.cpp
    # Listed so AUTOMOC processes the QObject headers that have no source file
    # of their own: the abstract backend and the do-nothing one.
    src/notify/NotificationBackend.h
    src/notify/NullNotifier.h
)
if(APPLE)
    list(APPEND openchat_notify_sources
        src/notify/MacNotifier.mm
        src/notify/MacNotifier.h)
elseif(WIN32)
    list(APPEND openchat_notify_sources
        src/notify/WindowsNotifier.cpp
        src/notify/WindowsNotifier.h)
elseif(UNIX)
    list(APPEND openchat_notify_sources
        src/notify/FreedesktopNotifier.cpp
        src/notify/FreedesktopNotifier.h)
endif()

add_library(openchat_notify STATIC ${openchat_notify_sources})
target_include_directories(openchat_notify PUBLIC src)
target_link_libraries(openchat_notify PUBLIC openchat_avatar Qt6::Core Qt6::Gui)
if(APPLE)
    set_source_files_properties(src/notify/MacNotifier.mm PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(openchat_notify PRIVATE
        "-framework Foundation"
        "-framework UserNotifications"
    )
elseif(WIN32)
    # shell32/ole32/propsys register the Start Menu shortcut that gives an
    # unpackaged application the identity Windows attributes its toasts to;
    # shlwapi carries the SHStrDupW that propvarutil.h's inline string helpers
    # call, which the Windows SDK links in implicitly and mingw-w64 does not.
    target_link_libraries(openchat_notify PRIVATE
        shell32 ole32 oleaut32 propsys uuid shlwapi)
    # C++/WinRT is what draws the sender's picture on a real toast. It ships
    # with the Windows SDK and with recent mingw-w64, but not with every
    # toolchain, so it is detected rather than assumed: the source picks the
    # same branch through __has_include, and this keeps the libraries it needs
    # in step with that choice.
    include(CheckIncludeFileCXX)
    check_include_file_cxx("winrt/Windows.UI.Notifications.h" OPENCHAT_HAVE_CPPWINRT)
    if(OPENCHAT_HAVE_CPPWINRT)
        target_link_libraries(openchat_notify PRIVATE runtimeobject windowsapp)
    else()
        message(STATUS
            "OpenChat: no C++/WinRT; notifications fall back to shell balloons.")
    endif()
elseif(UNIX)
    target_link_libraries(openchat_notify PUBLIC Qt6::DBus)
endif()

add_library(openchat_protocol STATIC
    src/protocol/CiphertextEnvelope.cpp
    src/protocol/CanonicalCborCodec.cpp
)
target_include_directories(openchat_protocol PUBLIC src)
target_link_libraries(openchat_protocol PUBLIC openchat_domain Qt6::Core)

add_library(openchat_network STATIC
    src/network/BackoffPolicy.cpp
    src/network/RelayClient.cpp
    src/network/MlsTransactionCoordinator.cpp
    src/network/SyncEngine.cpp
    src/network/MlsSyncSession.cpp
    src/network/RelayTransport.cpp
)
target_include_directories(openchat_network PUBLIC src)
target_link_libraries(openchat_network
    PUBLIC openchat_protocol openchat_domain Qt6::Core Qt6::Network Qt6::WebSockets
)

add_library(openchat_crypto STATIC
    src/crypto/MlsClient.cpp
)
target_include_directories(openchat_crypto PUBLIC src)
target_link_libraries(openchat_crypto
    PUBLIC openchat_domain Qt6::Core
    PRIVATE openchat_mls_rust
)

# Format-agnostic audio: file decoding, rate/channel conditioning, the codec
# surface (PCM and Opus), the adaptive jitter buffer, and the level meter.
# Deliberately free of Qt Multimedia and of any call concept, so it is testable
# with nothing but a file on disk.
add_library(openchat_media STATIC
    src/media/ToneSynth.cpp
    src/media/WavFile.cpp
    src/media/AudioConvert.cpp
    src/media/AudioCodec.cpp
    src/media/OpusAudioCodec.cpp
    src/media/JitterBuffer.cpp
    src/media/SpeechLevelMeter.cpp
    src/media/MicrophoneProcessor.cpp
)
target_include_directories(openchat_media PUBLIC src)
target_link_libraries(openchat_media
    PUBLIC openchat_domain Qt6::Core
    PRIVATE openchat_opus
)

add_library(openchat_security STATIC
    src/security/SecureBuffer.cpp
    src/security/QtKeychainVault.cpp
    src/security/DeviceIdentity.cpp
    src/security/RecoveryCode.cpp
    src/security/SafetyNumber.cpp
)

add_library(openchat_storage STATIC
    src/storage/SqlCipherDatabase.cpp
    src/storage/SqlCipherChatRepository.cpp
    src/storage/SqlCipherContactRepository.cpp
    src/storage/SqlCipherOutboxRepository.cpp
    src/storage/SqlCipherSyncRepository.cpp
    src/storage/SqlCipherSyncStore.cpp
    src/storage/CapturingMlsStateStore.cpp
)
target_include_directories(openchat_storage PUBLIC src)
target_link_libraries(openchat_storage
    PUBLIC openchat_security Qt6::Core
    PRIVATE OpenSSL::Crypto openchat_sqlcipher
)
qt_add_resources(openchat_storage "openchat_migrations"
    PREFIX "/openchat"
    BASE "src/storage/migrations"
    FILES
        src/storage/migrations/001_initial.sql
        src/storage/migrations/002_indexes.sql
        src/storage/migrations/003_domain_alignment.sql
        src/storage/migrations/004_profile_identity.sql
        src/storage/migrations/005_outbox_control.sql
        src/storage/migrations/006_account_identity.sql
        src/storage/migrations/007_contacts.sql
        src/storage/migrations/008_pending_handshakes.sql
        src/storage/migrations/009_contact_verification.sql
        src/storage/migrations/010_profile_display_name.sql
        src/storage/migrations/011_contact_device.sql
        src/storage/migrations/012_profiles.sql
        src/storage/migrations/013_group_chats.sql
        src/storage/migrations/014_chat_read_state.sql
)
target_include_directories(openchat_security PUBLIC src)
target_link_libraries(openchat_security
    PUBLIC openchat_domain Qt6::Core
    PRIVATE OpenSSL::Crypto qt6keychain
)

# The voice call itself: signalling codec, per-call media keys, the media
# pipeline, the state machine, and the Qt Multimedia device adapters. Everything
# except QtAudioIo is device- and network-free.
add_library(openchat_call STATIC
    src/call/CallTypes.cpp
    src/call/CallSounds.cpp
    src/call/CallSignal.cpp
    src/call/CallMediaCrypto.cpp
    src/call/CallMediaPacket.cpp
    src/call/CallSession.cpp
    src/call/CallEngine.cpp
    src/call/SyncCallTransport.cpp
    src/call/QtAudioIo.cpp
    src/call/CallVideoSession.cpp
    src/call/CallScreenSession.cpp
    src/call/QtVideoCapture.cpp
    src/call/QtScreenCapture.cpp
    src/call/VideoFrameCopy.cpp
)
target_include_directories(openchat_call PUBLIC src)
target_link_libraries(openchat_call
    PUBLIC openchat_media openchat_network openchat_domain Qt6::Core Qt6::Gui Qt6::Multimedia
    PRIVATE OpenSSL::Crypto
)

add_library(openchat_app STATIC
    src/app/ProfileSession.cpp
    src/app/AccountBootstrap.cpp
    src/app/AddContactService.cpp
    src/app/ContactRequestService.cpp
    src/app/GroupService.cpp
    src/app/DeviceLink.cpp
    src/app/KeyPackageSupply.cpp
)
target_include_directories(openchat_app PUBLIC src)
target_link_libraries(openchat_app
    PUBLIC openchat_network openchat_crypto openchat_storage openchat_security Qt6::Core
)

add_executable(tst_domain tests/tst_domain.cpp)
target_include_directories(tst_domain PRIVATE src)
target_link_libraries(tst_domain PRIVATE openchat_domain Qt6::Core Qt6::Test)
add_test(NAME tst_domain COMMAND tst_domain)

add_executable(tst_groupupdate tests/tst_groupupdate.cpp)
target_include_directories(tst_groupupdate PRIVATE src)
target_link_libraries(tst_groupupdate PRIVATE openchat_domain Qt6::Core Qt6::Test)
add_test(NAME tst_groupupdate COMMAND tst_groupupdate)

add_executable(tst_keyvault tests/tst_keyvault.cpp)
target_include_directories(tst_keyvault PRIVATE src tests)
target_link_libraries(tst_keyvault PRIVATE openchat_security Qt6::Core Qt6::Test)
add_test(NAME tst_keyvault COMMAND tst_keyvault)

add_executable(tst_safetynumber tests/tst_safetynumber.cpp)
target_include_directories(tst_safetynumber PRIVATE src)
target_link_libraries(tst_safetynumber PRIVATE openchat_security Qt6::Core Qt6::Test)
add_test(NAME tst_safetynumber COMMAND tst_safetynumber)

add_executable(tst_sqlcipherstorage tests/tst_sqlcipherstorage.cpp)
target_include_directories(tst_sqlcipherstorage PRIVATE src)
target_link_libraries(tst_sqlcipherstorage PRIVATE openchat_storage Qt6::Core Qt6::Test)
add_test(NAME tst_sqlcipherstorage COMMAND tst_sqlcipherstorage)

add_executable(tst_repositories tests/tst_repositories.cpp)
target_include_directories(tst_repositories PRIVATE src)
target_link_libraries(tst_repositories PRIVATE openchat_storage Qt6::Core Qt6::Test)
add_test(NAME tst_repositories COMMAND tst_repositories)

add_executable(tst_syncstore tests/tst_syncstore.cpp)
target_include_directories(tst_syncstore PRIVATE src)
target_link_libraries(tst_syncstore PRIVATE openchat_storage Qt6::Core Qt6::Test)
add_test(NAME tst_syncstore COMMAND tst_syncstore)

add_executable(tst_contactrepository tests/tst_contactrepository.cpp)
target_include_directories(tst_contactrepository PRIVATE src)
target_link_libraries(tst_contactrepository PRIVATE openchat_storage Qt6::Core Qt6::Test)
add_test(NAME tst_contactrepository COMMAND tst_contactrepository)

add_executable(tst_envelopecodec tests/tst_envelopecodec.cpp)
target_include_directories(tst_envelopecodec PRIVATE src)
target_compile_definitions(tst_envelopecodec PRIVATE
    OPENCHAT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(tst_envelopecodec PRIVATE openchat_protocol Qt6::Core Qt6::Test)
add_test(NAME tst_envelopecodec COMMAND tst_envelopecodec)

add_executable(tst_mlsbridge tests/tst_mlsbridge.cpp)
target_include_directories(tst_mlsbridge PRIVATE src)
target_link_libraries(tst_mlsbridge PRIVATE openchat_crypto Qt6::Core Qt6::Test)
add_test(NAME tst_mlsbridge COMMAND tst_mlsbridge)

add_subdirectory(relay)

add_executable(tst_relayclient
    tests/tst_relayclient.cpp
    tests/relay/RelayTestSupport.cpp
)
target_include_directories(tst_relayclient PRIVATE src tests relay/src)
target_link_libraries(tst_relayclient PRIVATE
    openchat_network
    openchat_protocol
    openchat_domain
    openchat_security
    openchat_relay_core
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Test
    OpenSSL::Crypto
)
add_test(NAME tst_relayclient COMMAND tst_relayclient)
set_tests_properties(tst_relayclient PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(tst_deviceidentity tests/tst_deviceidentity.cpp)
target_include_directories(tst_deviceidentity PRIVATE src relay/src)
target_link_libraries(tst_deviceidentity PRIVATE
    openchat_security
    openchat_protocol
    openchat_domain
    openchat_relay_core
    Qt6::Core
    Qt6::Test
)
add_test(NAME tst_deviceidentity COMMAND tst_deviceidentity)

add_executable(tst_mlscoordinator tests/tst_mlscoordinator.cpp)
target_include_directories(tst_mlscoordinator PRIVATE src)
target_link_libraries(tst_mlscoordinator PRIVATE openchat_network openchat_domain Qt6::Core Qt6::Test)
add_test(NAME tst_mlscoordinator COMMAND tst_mlscoordinator)

add_executable(tst_syncengine tests/tst_syncengine.cpp)
target_include_directories(tst_syncengine PRIVATE src)
target_link_libraries(tst_syncengine PRIVATE
    openchat_network openchat_protocol openchat_domain Qt6::Core Qt6::Test)
add_test(NAME tst_syncengine COMMAND tst_syncengine)

add_executable(tst_syncadapters tests/tst_syncadapters.cpp)
target_include_directories(tst_syncadapters PRIVATE src)
target_link_libraries(tst_syncadapters PRIVATE
    openchat_network
    openchat_crypto
    openchat_storage
    openchat_security
    openchat_protocol
    openchat_domain
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Test)
add_test(NAME tst_syncadapters COMMAND tst_syncadapters)
set_tests_properties(tst_syncadapters PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(tst_profilesession tests/tst_profilesession.cpp)
target_include_directories(tst_profilesession PRIVATE src)
target_link_libraries(tst_profilesession PRIVATE openchat_app Qt6::Core Qt6::Test)
add_test(NAME tst_profilesession COMMAND tst_profilesession)

# Live end-to-end account bootstrap against the Compose relay stack over TLS.
# Labelled "integration": it QSKIPs cleanly when the stack is not reachable, so a
# normal ctest run is unaffected; bring the stack up to make it actually run.
add_executable(tst_accountbootstrap tests/tst_accountbootstrap.cpp)
target_include_directories(tst_accountbootstrap PRIVATE src)
target_compile_definitions(tst_accountbootstrap PRIVATE
    OPENCHAT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(tst_accountbootstrap PRIVATE
    openchat_app
    openchat_network
    openchat_crypto
    openchat_storage
    openchat_security
    openchat_protocol
    openchat_domain
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Test
)
add_test(NAME tst_accountbootstrap COMMAND tst_accountbootstrap)
set_tests_properties(tst_accountbootstrap PROPERTIES
    LABELS integration
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
)

# Real-wiring integration test for the add-contact handshake SEND path: a live
# ProfileSession (SQLCipher + real MlsClient) plus a second MlsClient standing in
# for the peer, driven through AddContactService by emitting the relay's signals.
add_executable(tst_addcontactservice tests/tst_addcontactservice.cpp
    tests/relay/RelayTestSupport.cpp)
target_include_directories(tst_addcontactservice PRIVATE src tests relay/src)
target_link_libraries(tst_addcontactservice PRIVATE
    openchat_app
    openchat_network
    openchat_crypto
    openchat_storage
    openchat_security
    openchat_protocol
    openchat_domain
    openchat_relay_core
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Test
)
add_test(NAME tst_addcontactservice COMMAND tst_addcontactservice)
set_tests_properties(tst_addcontactservice PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Real-wiring integration test for the add-contact handshake RECEIVE path: a live
# ProfileSession (SQLCipher + real MlsClient) plus a second MlsClient standing in
# for the sender, driving ContactRequestService's accept/decline/block/reconcile
# through the SyncEngine over a fake ciphertext transport.
add_executable(tst_contactrequestservice tests/tst_contactrequestservice.cpp)
target_include_directories(tst_contactrequestservice PRIVATE src tests relay/src)
target_link_libraries(tst_contactrequestservice PRIVATE
    openchat_app
    openchat_network
    openchat_crypto
    openchat_storage
    openchat_security
    openchat_protocol
    openchat_domain
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Test
)
add_test(NAME tst_contactrequestservice COMMAND tst_contactrequestservice)
set_tests_properties(tst_contactrequestservice PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Two-client end-to-end integration test over REAL TLS + REAL in-process relay +
# REAL PostgreSQL. Two bootstrapped ProfileSessions exchange a contact handshake
# and messages through an in-process TLS-terminating proxy in front of a live
# RelayServer. Labelled "integration": it QSKIPs cleanly when Postgres is not
# reachable, so a normal ctest run is unaffected.
add_executable(tst_e2e
    tests/tst_e2e.cpp
    tests/relay/RelayTestSupport.cpp
    src/controllers/ChatController.cpp
    src/controllers/CallController.cpp
    src/models/ContactListModel.cpp
    src/models/MessageListModel.cpp
    src/models/CallParticipantModel.cpp
)
target_include_directories(tst_e2e PRIVATE src tests relay/src)
target_link_libraries(tst_e2e PRIVATE
    openchat_app
    openchat_avatar
    openchat_call
    openchat_media
    openchat_network
    openchat_crypto
    openchat_storage
    openchat_security
    openchat_protocol
    openchat_domain
    openchat_relay_server
    openchat_relay_core
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Sql
    Qt6::HttpServer
    Qt6::Test
    OpenSSL::Crypto
)
add_test(NAME tst_e2e COMMAND tst_e2e)
set_tests_properties(tst_e2e PROPERTIES
    LABELS integration
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;OPENCHAT_TEST_PG_PORT=55432;OPENCHAT_TEST_PG_HOST=127.0.0.1;OPENCHAT_TEST_PG_USER=ocrelay;OPENCHAT_TEST_PG_ADMINDB=postgres"
)

add_executable(tst_contactcontroller
    tests/tst_contactcontroller.cpp
    src/controllers/ContactController.cpp
    src/models/RequestListModel.cpp
)
target_include_directories(tst_contactcontroller PRIVATE src tests relay/src)
target_link_libraries(tst_contactcontroller PRIVATE
    openchat_app
    openchat_network
    openchat_crypto
    openchat_storage
    openchat_security
    openchat_protocol
    openchat_domain
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Test
)
add_test(NAME tst_contactcontroller COMMAND tst_contactcontroller)
set_tests_properties(tst_contactcontroller PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The notification policy, checked against a recording backend so no desktop
# notification service is needed to run it.
add_executable(tst_notifications tests/tst_notifications.cpp)
target_include_directories(tst_notifications PRIVATE src)
target_link_libraries(tst_notifications PRIVATE
    openchat_notify
    openchat_avatar
    Qt6::Core
    Qt6::Gui
    Qt6::Test
)
add_test(NAME tst_notifications COMMAND tst_notifications)
set_tests_properties(tst_notifications PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

if(UNIX AND NOT APPLE)
    # The freedesktop backend against a notification daemon of the test's own,
    # on a private session bus. Skips itself where dbus-daemon is absent.
    add_executable(tst_freedesktopnotifier tests/tst_freedesktopnotifier.cpp)
    target_include_directories(tst_freedesktopnotifier PRIVATE src)
    target_link_libraries(tst_freedesktopnotifier PRIVATE
        openchat_notify
        openchat_avatar
        Qt6::Core
        Qt6::DBus
        Qt6::Gui
        Qt6::Test
    )
    add_test(NAME tst_freedesktopnotifier COMMAND tst_freedesktopnotifier)
    set_tests_properties(tst_freedesktopnotifier PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

add_executable(tst_models
    tests/tst_models.cpp
    src/models/ContactListModel.cpp
    src/models/MessageListModel.cpp
    src/models/CallParticipantModel.cpp
)
target_include_directories(tst_models PRIVATE src)
target_link_libraries(tst_models PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
add_test(NAME tst_models COMMAND tst_models)

add_executable(tst_chatcontroller
    tests/tst_chatcontroller.cpp
    src/controllers/ChatController.cpp
    src/controllers/CallController.cpp
    src/models/ContactListModel.cpp
    src/models/MessageListModel.cpp
    src/models/CallParticipantModel.cpp
)
target_include_directories(tst_chatcontroller PRIVATE src tests)
target_link_libraries(tst_chatcontroller PRIVATE
    openchat_call
    openchat_avatar
    openchat_app
    openchat_network
    openchat_crypto
    openchat_storage
    openchat_security
    openchat_protocol
    openchat_domain
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    Qt6::Test
)
add_test(NAME tst_chatcontroller COMMAND tst_chatcontroller)
set_tests_properties(tst_chatcontroller PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(tst_onboardingcontroller
    tests/tst_onboardingcontroller.cpp
    src/controllers/OnboardingController.cpp
)
target_include_directories(tst_onboardingcontroller PRIVATE src)
target_link_libraries(tst_onboardingcontroller PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_onboardingcontroller COMMAND tst_onboardingcontroller)

add_executable(tst_videocall tests/tst_videocall.cpp)
target_link_libraries(tst_videocall PRIVATE openchat_call Qt6::Test)
add_test(NAME tst_videocall COMMAND tst_videocall)

add_executable(tst_screenshare tests/tst_screenshare.cpp)
target_link_libraries(tst_screenshare PRIVATE openchat_call Qt6::Gui Qt6::Test)
add_test(NAME tst_screenshare COMMAND tst_screenshare)

# The voice-call test: pushes a real audio file (and synthesised references)
# through the whole pipeline and checks what comes out the far end, plus the
# malformed-input, loss, reordering, duplication, replay and tampering edges
# that a real link produces. Runs headless -- no audio device is opened.
add_executable(tst_voicecall tests/tst_voicecall.cpp)
target_include_directories(tst_voicecall PRIVATE src tests)
# So the test can check the rendered WAVs in assets/sounds still match the
# sounds this code generates.
target_compile_definitions(tst_voicecall PRIVATE
    OPENCHAT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(tst_voicecall PRIVATE
    openchat_call
    openchat_media
    openchat_domain
    Qt6::Core
    Qt6::Test
)
add_test(NAME tst_voicecall COMMAND tst_voicecall)

# The call state machine over a scripted transport and scripted audio devices:
# offers, answers, declines, hangups, timeouts, glare and busy handling.
add_executable(tst_callengine tests/tst_callengine.cpp)
target_include_directories(tst_callengine PRIVATE src tests)
target_link_libraries(tst_callengine PRIVATE
    openchat_call
    openchat_media
    openchat_domain
    Qt6::Core
    Qt6::Test
)
add_test(NAME tst_callengine COMMAND tst_callengine)

# Group calls over a mesh of scripted endpoints: everyone rings, joins one by
# one, hears the others mixed, leaves without ending it for the rest, and the
# decline/busy/timeout/glare edges.
add_executable(tst_groupcall tests/tst_groupcall.cpp)
target_include_directories(tst_groupcall PRIVATE src tests)
target_link_libraries(tst_groupcall PRIVATE
    openchat_call
    openchat_media
    openchat_domain
    Qt6::Core
    Qt6::Test
)
add_test(NAME tst_groupcall COMMAND tst_groupcall)

# The profile-update codec, the picture pipeline and the avatar registry:
# round trips, every rejected shape, and the size/dimension bounds a chosen
# file must respect before it is decoded in full.
add_executable(tst_profile tests/tst_profile.cpp)
target_include_directories(tst_profile PRIVATE src)
target_link_libraries(tst_profile PRIVATE openchat_avatar openchat_domain Qt6::Core Qt6::Gui Qt6::Test)
add_test(NAME tst_profile COMMAND tst_profile)
set_tests_properties(tst_profile PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(tst_bubblebackground
    tests/tst_bubblebackground.cpp
    src/render/BubbleBackground.cpp
    src/render/AvatarArtwork.cpp
)
target_include_directories(tst_bubblebackground PRIVATE src)
target_link_libraries(tst_bubblebackground PRIVATE openchat_avatar Qt6::Core Qt6::Gui Qt6::Quick Qt6::Test)
add_test(NAME tst_bubblebackground COMMAND tst_bubblebackground)

add_executable(tst_qmlload
    tests/tst_qmlload.cpp
    src/controllers/ChatController.cpp
    src/controllers/ContactController.cpp
    src/controllers/OnboardingController.cpp
    src/controllers/CallController.cpp
    src/models/ContactListModel.cpp
    src/models/MessageListModel.cpp
    src/models/CallParticipantModel.cpp
    src/models/RequestListModel.cpp
    src/render/BubbleBackground.cpp
    src/render/AvatarArtwork.cpp
    src/render/CallVideoItem.cpp
    src/app/AppearanceSettings.cpp
    src/app/MicrophoneSettings.cpp
)
target_include_directories(tst_qmlload PRIVATE src)
target_compile_definitions(tst_qmlload PRIVATE
    OPENCHAT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(tst_qmlload PRIVATE
    openchat_app
    openchat_avatar
    openchat_call
    openchat_network
    openchat_crypto
    openchat_storage
    openchat_security
    openchat_protocol
    openchat_domain
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
    Qt6::WebSockets
    Qt6::Test
)
add_test(NAME tst_qmlload COMMAND tst_qmlload)
set_tests_properties(tst_qmlload PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
)

function(add_openchat_capture_test test_name capture_width capture_height)
    add_test(
        NAME ${test_name}
        COMMAND ${CMAKE_COMMAND}
            -DOPENCHAT_EXECUTABLE=$<TARGET_FILE:OpenChat>
            -DCAPTURE_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/${test_name}.png
            -DCAPTURE_WIDTH=${capture_width}
            -DCAPTURE_HEIGHT=${capture_height}
            "-DCAPTURE_EXTRA_ARG=${ARGV3}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_capture.cmake
    )
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 5)
endfunction()

add_openchat_capture_test(capture_smoke 860 680)
add_openchat_capture_test(capture_minimum 720 560)
add_openchat_capture_test(capture_large 1024 768)
add_openchat_capture_test(capture_video 900 680 --call-video)
add_openchat_capture_test(capture_video_minimum 720 560 --call-video)
add_openchat_capture_test(capture_screen 1100 780 --call-screen)
add_openchat_capture_test(capture_screen_minimum 720 560 --call-screen)
add_openchat_capture_test(capture_screen_picker 900 700 --call-picker)
# A share and a camera enlarged over the window, and the call filling it:
# several flags, quoted into one list so the runner passes them all through.
add_openchat_capture_test(capture_screen_zoom 1100 780 "--call-screen;--call-zoom")
add_openchat_capture_test(capture_video_zoom 900 680 "--call-video;--call-zoom")
add_openchat_capture_test(capture_screen_fullscreen 1100 780 "--call-screen;--call-fullscreen")
add_openchat_capture_test(capture_video_fullscreen 720 560 "--call-video;--call-fullscreen")

# The add-contact preview sub-mode: renders the dialog over a populated mock
# Requests list. Uses the same verify runner with the extra --add-contact argument.
add_test(
    NAME capture_add_contact
    COMMAND ${CMAKE_COMMAND}
        -DOPENCHAT_EXECUTABLE=$<TARGET_FILE:OpenChat>
        -DCAPTURE_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/capture_add_contact.png
        -DCAPTURE_WIDTH=900
        -DCAPTURE_HEIGHT=680
        -DCAPTURE_EXTRA_ARG=--add-contact
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_capture.cmake
)
set_tests_properties(capture_add_contact PROPERTIES TIMEOUT 5)

# The contact-verification preview sub-mode: renders the safety-number dialog over
# the mock chat window. Uses the same verify runner with the extra --verify argument.
add_test(
    NAME capture_verify
    COMMAND ${CMAKE_COMMAND}
        -DOPENCHAT_EXECUTABLE=$<TARGET_FILE:OpenChat>
        -DCAPTURE_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/capture_verify.png
        -DCAPTURE_WIDTH=900
        -DCAPTURE_HEIGHT=680
        -DCAPTURE_EXTRA_ARG=--verify
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_capture.cmake
)
set_tests_properties(capture_verify PROPERTIES TIMEOUT 5)

# The in-call preview sub-mode: renders the call surface in place of the
# conversation header, mid-call, with the far end talking so the green speaking
# ring is in the capture.
add_test(
    NAME capture_call
    COMMAND ${CMAKE_COMMAND}
        -DOPENCHAT_EXECUTABLE=$<TARGET_FILE:OpenChat>
        -DCAPTURE_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/capture_call.png
        -DCAPTURE_WIDTH=900
        -DCAPTURE_HEIGHT=680
        -DCAPTURE_EXTRA_ARG=--call
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_capture.cmake
)
set_tests_properties(capture_call PROPERTIES TIMEOUT 5)
