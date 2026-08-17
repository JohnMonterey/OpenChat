cmake_minimum_required(VERSION 3.21)

project(OpenChat VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core DBus Gui HttpServer Network Quick QuickControls2 Sql Svg Test WebSockets)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)

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
    src/controllers/OnboardingController.cpp
    src/models/ContactListModel.cpp
    src/models/MessageListModel.cpp
    src/render/BubbleBackground.cpp
    src/render/AvatarArtwork.cpp
)

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
        qml/OpenChat/components/ContactSidebar.qml
        qml/OpenChat/components/ContactCategory.qml
        qml/OpenChat/components/ContactRow.qml
        qml/OpenChat/components/ConversationHeader.qml
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
        assets/icons/phone-call.svg
        assets/icons/chevron-down.svg
        assets/icons/openchat.svg
)

target_include_directories(OpenChat PRIVATE src)
target_link_libraries(OpenChat PRIVATE
    openchat_app
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Svg
)

enable_testing()

add_executable(tst_appmetadata tests/tst_appmetadata.cpp)
target_include_directories(tst_appmetadata PRIVATE src)
target_link_libraries(tst_appmetadata PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_appmetadata COMMAND tst_appmetadata)

add_library(openchat_domain STATIC
    src/domain/Identifiers.cpp
)
target_include_directories(openchat_domain PUBLIC src)
target_link_libraries(openchat_domain PUBLIC Qt6::Core)

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

add_library(openchat_security STATIC
    src/security/SecureBuffer.cpp
    src/security/QtKeychainVault.cpp
    src/security/DeviceIdentity.cpp
    src/security/RecoveryCode.cpp
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
)
target_include_directories(openchat_security PUBLIC src)
target_link_libraries(openchat_security
    PUBLIC openchat_domain Qt6::Core
    PRIVATE OpenSSL::Crypto qt6keychain
)

add_library(openchat_app STATIC
    src/app/ProfileSession.cpp
    src/app/AccountBootstrap.cpp
)
target_include_directories(openchat_app PUBLIC src)
target_link_libraries(openchat_app
    PUBLIC openchat_network openchat_crypto openchat_storage openchat_security Qt6::Core
)

add_executable(tst_domain tests/tst_domain.cpp)
target_include_directories(tst_domain PRIVATE src)
target_link_libraries(tst_domain PRIVATE openchat_domain Qt6::Core Qt6::Test)
add_test(NAME tst_domain COMMAND tst_domain)

add_executable(tst_keyvault tests/tst_keyvault.cpp)
target_include_directories(tst_keyvault PRIVATE src tests)
target_link_libraries(tst_keyvault PRIVATE openchat_security Qt6::Core Qt6::Test)
add_test(NAME tst_keyvault COMMAND tst_keyvault)

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

add_executable(tst_models
    tests/tst_models.cpp
    src/models/ContactListModel.cpp
    src/models/MessageListModel.cpp
)
target_include_directories(tst_models PRIVATE src)
target_link_libraries(tst_models PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_models COMMAND tst_models)

add_executable(tst_chatcontroller
    tests/tst_chatcontroller.cpp
    src/controllers/ChatController.cpp
    src/models/ContactListModel.cpp
    src/models/MessageListModel.cpp
)
target_include_directories(tst_chatcontroller PRIVATE src)
target_link_libraries(tst_chatcontroller PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_chatcontroller COMMAND tst_chatcontroller)

add_executable(tst_onboardingcontroller
    tests/tst_onboardingcontroller.cpp
    src/controllers/OnboardingController.cpp
)
target_include_directories(tst_onboardingcontroller PRIVATE src)
target_link_libraries(tst_onboardingcontroller PRIVATE Qt6::Core Qt6::Test)
add_test(NAME tst_onboardingcontroller COMMAND tst_onboardingcontroller)

add_executable(tst_bubblebackground
    tests/tst_bubblebackground.cpp
    src/render/BubbleBackground.cpp
    src/render/AvatarArtwork.cpp
)
target_include_directories(tst_bubblebackground PRIVATE src)
target_link_libraries(tst_bubblebackground PRIVATE Qt6::Core Qt6::Gui Qt6::Quick Qt6::Test)
add_test(NAME tst_bubblebackground COMMAND tst_bubblebackground)

add_executable(tst_qmlload
    tests/tst_qmlload.cpp
    src/controllers/ChatController.cpp
    src/controllers/OnboardingController.cpp
    src/models/ContactListModel.cpp
    src/models/MessageListModel.cpp
    src/render/BubbleBackground.cpp
    src/render/AvatarArtwork.cpp
)
target_include_directories(tst_qmlload PRIVATE src)
target_compile_definitions(tst_qmlload PRIVATE
    OPENCHAT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(tst_qmlload PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickControls2
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
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/verify_capture.cmake
    )
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 5)
endfunction()

add_openchat_capture_test(capture_smoke 860 680)
add_openchat_capture_test(capture_minimum 720 560)
add_openchat_capture_test(capture_large 1024 768)
