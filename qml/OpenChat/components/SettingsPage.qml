import QtQuick
import QtQuick.Controls.Basic
import OpenChat

// Settings → the open category. The sidebar picks the category; this page
// shows everything in it at once, each section a working control under its
// own heading, so nothing here navigates. A section's control is built only
// while the page is on screen, because the Input section has a device behind
// it.
Item {
    id: page
    objectName: "settingsDetail"

    required property var controller
    // A restart ends a call, so Low memory mode withholds it during one.
    property bool restartAllowed: true

    readonly property real sideMargin: 34

    function sectionComponent(name) {
        switch (name) {
        case "Memory": return memorySection;
        case "Input": return inputSection;
        case "Custom Vocal FX": return vocalFxSection;
        case "Connection": return connectionSection;
        case "Theme": return themeSection;
        }
        return null;
    }

    Text {
        id: title
        objectName: "settingsDetailTitle"
        x: page.sideMargin
        y: 28
        width: page.width - 2 * page.sideMargin
        text: page.controller.currentSettingsCategoryName
        color: Theme.textPrimary
        font.family: Theme.uiFont
        font.pixelSize: 22
        elide: Text.ElideRight
        renderType: Text.NativeRendering
    }

    Rectangle {
        id: titleRule
        x: page.sideMargin
        width: title.width
        anchors.top: title.bottom
        anchors.topMargin: 16
        height: 1
        color: Theme.rule
    }

    Flickable {
        id: scroll
        objectName: "settingsScroll"
        x: page.sideMargin
        width: title.width
        anchors.top: titleRule.bottom
        anchors.bottom: parent.bottom
        contentHeight: sections.height + 28
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        Connections {
            target: page.controller
            function onCurrentSettingsCategoryChanged() { scroll.contentY = 0; }
        }

        ScrollBar.vertical: ScrollBar {
            id: scrollBar
            objectName: "settingsScrollBar"
            // In the right margin, clear of the controls.
            parent: page
            x: page.width - page.sideMargin + 14
            y: scroll.y
            height: scroll.height
            padding: 0
            minimumSize: 0.1
            policy: ScrollBar.AsNeeded
            background: Item {}
            contentItem: Rectangle {
                implicitWidth: 6
                radius: 3
                color: scrollBar.pressed || scrollBar.hovered ? Theme.iconHover : Theme.inputBorder
                visible: scrollBar.size < 1.0
            }
        }

        Column {
            id: sections
            width: scroll.width

            Repeater {
                model: page.controller.currentSettingsElements

                Column {
                    id: section
                    required property string modelData
                    required property int index
                    objectName: "settingsSection_" + modelData
                    width: sections.width

                    Accessible.role: Accessible.Grouping
                    Accessible.name: modelData

                    // Sections after the first are set apart by a rule.
                    Item {
                        width: parent.width
                        height: section.index === 0 ? 20 : 41
                        Rectangle {
                            visible: section.index > 0
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width
                            height: 1
                            color: Theme.softRule
                        }
                    }

                    Text {
                        objectName: "settingsSectionTitle_" + section.modelData
                        width: parent.width
                        text: section.modelData
                        elide: Text.ElideRight
                        color: Theme.categoryText
                        font.family: Theme.uiFont
                        font.pixelSize: 17
                        renderType: Text.NativeRendering
                    }

                    Loader {
                        width: parent.width
                        height: item ? item.implicitHeight : 0
                        active: page.visible
                        sourceComponent: page.sectionComponent(section.modelData)
                    }
                }
            }
        }
    }

    Component {
        id: memorySection
        LowMemoryPanel { restartAllowed: page.restartAllowed }
    }

    Component {
        id: inputSection
        MicrophoneSettingsPanel {}
    }

    Component {
        id: vocalFxSection
        VocalFxPanel {}
    }

    Component {
        id: connectionSection
        ConnectionSettingsPanel {}
    }

    Component {
        id: themeSection
        Item {
            implicitHeight: 64

            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                anchors.verticalCenterOffset: -10
                text: "Dark mode"
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 15
                renderType: Text.NativeRendering
            }
            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                anchors.verticalCenterOffset: 12
                width: parent.width - darkModeSwitch.width - 16
                text: "Use dark colors throughout OpenChat"
                elide: Text.ElideRight
                color: Theme.textSecondary
                font.family: Theme.uiFont
                font.pixelSize: 12
            }
            AeroSwitch {
                id: darkModeSwitch
                objectName: "darkModeSwitch"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                Accessible.name: "Dark mode"
                checked: Theme.darkMode
                onToggled: checked => Theme.setDarkMode(checked)
            }
        }
    }
}
