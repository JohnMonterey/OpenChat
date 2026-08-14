cmake_minimum_required(VERSION 3.21)

project(OpenChat VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick QuickControls2 Svg Test)

qt_standard_project_setup(REQUIRES 6.5)
qt_policy(SET QTP0004 NEW)

set_source_files_properties(qml/OpenChat/Theme.qml PROPERTIES
    QT_QML_SINGLETON_TYPE TRUE
)

qt_add_executable(OpenChat
    src/main.cpp
    src/controllers/ChatController.cpp
    src/models/ContactListModel.cpp
    src/models/MessageListModel.cpp
    src/render/BubbleBackground.cpp
)

qt_add_qml_module(OpenChat
    URI OpenChat
    VERSION 1.0
    RESOURCE_PREFIX /qt/qml
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/qml/OpenChat
    QML_FILES
        qml/OpenChat/Main.qml
        qml/OpenChat/Theme.qml
        qml/OpenChat/components/AeroWindowFrame.qml
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
        assets/icons/video-call.svg
        assets/icons/phone-call.svg
        assets/icons/chevron-down.svg
)

target_include_directories(OpenChat PRIVATE src)
target_link_libraries(OpenChat PRIVATE
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

add_executable(tst_bubblebackground
    tests/tst_bubblebackground.cpp
    src/render/BubbleBackground.cpp
)
target_include_directories(tst_bubblebackground PRIVATE src)
target_link_libraries(tst_bubblebackground PRIVATE Qt6::Core Qt6::Gui Qt6::Quick Qt6::Test)
add_test(NAME tst_bubblebackground COMMAND tst_bubblebackground)

add_executable(tst_qmlload
    tests/tst_qmlload.cpp
    src/controllers/ChatController.cpp
    src/models/ContactListModel.cpp
    src/models/MessageListModel.cpp
    src/render/BubbleBackground.cpp
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
