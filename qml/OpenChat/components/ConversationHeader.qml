import QtQuick
import QtQuick.Shapes
import QtQuick.Window
import OpenChat

Item {
    id: header
    objectName: "conversationHeader"
    required property var controller
    // Optional voice-call bridge; null in the default/capture paths, where the
    // call button stays inert exactly as it was.
    property var callController: null
    readonly property bool isGroup: controller.currentIsGroup
    implicitHeight: Theme.conversationHeaderHeight

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.headerTop }
            GradientStop { position: 1; color: Theme.headerBottom }
        }
    }

    Avatar {
        id: contactAvatar
        x: 24
        y: 21
        width: 68
        height: 68
        cornerRadius: 6
        avatarKey: header.controller.currentAvatarKey
    }

    // The name. For a group it is the title, and editable the same way the
    // status line is: click to type, Enter or leaving commits, Escape reverts.
    Item {
        id: titleEditor
        objectName: "groupTitleEditor"
        x: 106
        y: 27
        width: Math.min(Math.max(80, titleLabel.implicitWidth + 8),
                        addButton.visible ? leaveButton.x - x - 44 : header.width - x - 180)
        height: 30
        property bool editing: false

        function beginEditing() {
            if (!header.isGroup)
                return;
            titleInput.text = header.controller.currentGroupTitle;
            editing = true;
            titleInput.forceActiveFocus();
            titleInput.selectAll();
        }
        function commit() {
            if (!editing)
                return;
            editing = false;
            header.controller.renameCurrentGroup(titleInput.text.trim());
        }
        function cancel() {
            editing = false;
        }

        Rectangle {
            objectName: "groupTitleHoverShade"
            anchors.fill: parent
            radius: 3
            color: titleEditor.editing ? Theme.fieldBackground : Theme.profileHover
            border.width: titleEditor.editing ? 1 : 0
            border.color: Theme.inputBorder
            visible: header.isGroup && (titleMouse.containsMouse || titleEditor.editing)
        }

        Text {
            id: titleLabel
            objectName: "conversationTitle"
            x: 4
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 8
            elide: Text.ElideRight
            visible: !titleEditor.editing
            text: header.controller.currentContactName
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 20
            renderType: Text.NativeRendering
        }

        TextInput {
            id: titleInput
            objectName: "groupTitleInput"
            x: 4
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 8
            visible: titleEditor.editing
            clip: true
            selectByMouse: true
            selectionColor: Theme.selectionBackground
            selectedTextColor: Theme.selectionText
            maximumLength: 80
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 20
            onAccepted: titleEditor.commit()
            onActiveFocusChanged: {
                if (!activeFocus)
                    titleEditor.commit();
            }
            Keys.onEscapePressed: titleEditor.cancel()
        }

        MouseArea {
            id: titleMouse
            anchors.fill: parent
            hoverEnabled: true
            enabled: header.isGroup && !titleEditor.editing
            cursorShape: header.isGroup ? Qt.IBeamCursor : Qt.ArrowCursor
            onClicked: titleEditor.beginEditing()
        }
    }

    // The "+" next to the name: start a group with this person, or add
    // someone to this group. Two crossed strokes, in the sidebar's style.
    Item {
        id: addButton
        objectName: "addToGroupButton"
        x: titleEditor.x + titleEditor.width + 6
        anchors.verticalCenter: titleEditor.verticalCenter
        width: 24
        height: 24
        visible: header.controller.hasCurrentContact

        Rectangle {
            anchors.fill: parent
            radius: 12
            color: addMouse.containsMouse || memberPicker.visible ? Theme.navSelected : "transparent"
            border.width: 1
            border.color: addMouse.containsMouse || memberPicker.visible ? Theme.iconHover
                                                                          : Theme.iconDisabled
        }
        Rectangle {
            anchors.centerIn: parent
            width: 12
            height: 2
            radius: 1
            color: addMouse.containsMouse ? Theme.iconHover : Theme.categoryText
        }
        Rectangle {
            anchors.centerIn: parent
            width: 2
            height: 12
            radius: 1
            color: addMouse.containsMouse ? Theme.iconHover : Theme.categoryText
        }
        MouseArea {
            id: addMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                const corner = addButton.mapToItem(header.Window.contentItem, 0, addButton.height + 4);
                memberPicker.anchorX = Math.max(8, Math.min(corner.x,
                    header.Window.contentItem.width - memberPicker.width - 8));
                memberPicker.anchorY = corner.y;
                memberPicker.toggle();
            }
        }
    }

    GroupMemberPicker {
        id: memberPicker
        parent: header.Window.contentItem
        controller: header.controller
    }

    PresenceBead {
        x: 110
        y: 63
        beadSize: 11
        presence: header.controller.currentPresence
        visible: !header.isGroup
    }

    Text {
        objectName: "conversationSubtitle"
        x: header.isGroup ? 110 : 127
        y: 58
        width: Math.max(60, leaveButton.x - x - 16)
        elide: Text.ElideRight
        text: header.controller.currentStatusText
        color: Theme.textSecondary
        font.family: Theme.uiFont
        font.pixelSize: 14
        renderType: Text.NativeRendering
    }

    // Why a group change was refused, shown under the subtitle for a few
    // seconds. Collapsed and invisible while there is nothing to say.
    Rectangle {
        objectName: "groupNotice"
        x: 110
        y: 82
        width: Math.max(80, leaveButton.x - x - 16)
        height: groupNoticeText.implicitHeight + 6
        radius: 3
        color: Theme.warningBackground
        border.width: 1
        border.color: Theme.noticeBorder
        visible: header.controller.groupNotice.length > 0
        z: 5

        Text {
            id: groupNoticeText
            x: 5
            y: 3
            width: parent.width - 10
            elide: Text.ElideRight
            text: header.controller.groupNotice
            color: Theme.noticeText
            font.family: Theme.uiFont
            font.pixelSize: 12
            renderType: Text.NativeRendering
        }
        Timer {
            running: header.controller.groupNotice.length > 0
            interval: 6000
            onTriggered: header.controller.clearGroupNotice()
        }
    }

    // Leaving a group: a small red pill left of the call buttons, group only.
    CallActionButton {
        id: leaveButton
        objectName: "leaveGroupButton"
        visible: header.isGroup
        width: visible ? implicitWidth : 0
        label: "Leave group"
        accent: "end"
        anchors.right: videoButton.left
        anchors.rightMargin: visible ? 18 : 0
        anchors.verticalCenter: parent.verticalCenter
        onClicked: header.controller.leaveCurrentGroup()
    }

    Item {
        id: videoButton
        objectName: "videoCallButton"
        opacity: phoneButton.callable || header.callController === null ? 1.0 : 0.4
        width: 40
        height: 42
        anchors.right: phoneButton.left
        anchors.rightMargin: 22
        anchors.verticalCenter: parent.verticalCenter

        Image {
            id: videoImage
            anchors.centerIn: parent
            width: 28
            height: 22
            source: Qt.resolvedUrl("../../../assets/icons/video-call" + (Theme.darkMode ? "-dark.svg" : ".svg"))
            sourceSize: Qt.size(width * 2, height * 2)
        }
        Item {
            objectName: "videoCallFallback"
            anchors.centerIn: parent
            width: 28
            height: 22
            visible: videoImage.status === Image.Error || videoImage.status === Image.Null

            Rectangle {
                x: 0; y: 3; width: 20; height: 16; radius: 3
                color: Theme.iconInk
            }
            Shape {
                x: 20; y: 4; width: 8; height: 14
                ShapePath {
                    fillColor: Theme.iconInk
                    strokeWidth: 0
                    PathSvg { path: "M 0 4 L 8 0 L 8 14 L 0 10 Z" }
                }
            }
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            enabled: phoneButton.callable
            onClicked: header.callController.callCurrentContact(true)
        }
    }

    Item {
        id: phoneButton
        objectName: "phoneCallButton"
        // Calls need a bridge and a usable microphone; without either the button
        // says so by dimming rather than by silently doing nothing when pressed.
        readonly property bool callable: header.callController !== null
                                         && header.callController.callsAvailable
        width: 42
        height: 42
        anchors.right: menuButton.left
        anchors.rightMargin: 20
        anchors.verticalCenter: parent.verticalCenter
        opacity: callable || header.callController === null ? 1.0 : 0.4

        Image {
            id: phoneImage
            anchors.centerIn: parent
            width: 28
            height: 28
            source: Qt.resolvedUrl("../../../assets/icons/phone-call" + (Theme.darkMode ? "-dark.svg" : ".svg"))
            sourceSize: Qt.size(width * 2, height * 2)
        }
        Shape {
            objectName: "phoneCallFallback"
            anchors.centerIn: parent
            width: 28
            height: 28
            visible: phoneImage.status === Image.Error || phoneImage.status === Image.Null
            ShapePath {
                fillColor: Theme.iconInk
                strokeWidth: 0
                PathSvg {
                    path: "M 6.1 1.5 C 6.9 1 7.9 1.2 8.4 2 L 11.5 7 C 11.9 7.7 11.8 8.6 11.2 9.2 L 9 11.1 C 10.5 14.2 12.9 16.7 16 18.2 L 17.9 15.9 C 18.4 15.3 19.4 15.1 20.1 15.6 L 25.1 18.7 C 25.9 19.2 26.1 20.2 25.6 21 L 23.6 24.4 C 22.9 25.6 21.5 26.3 20.1 26 C 10.6 24.4 3.6 17.4 2 7.9 C 1.8 6.5 2.4 5.1 3.6 4.4 Z"
                }
            }
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            enabled: phoneButton.callable
            onClicked: header.callController.callCurrentContact()
        }
    }

    Item {
        id: menuButton
        width: 34
        height: 42
        anchors.right: parent.right
        anchors.rightMargin: 17
        anchors.verticalCenter: parent.verticalCenter

        Image {
            anchors.centerIn: parent
            width: 13
            height: 9
            source: Qt.resolvedUrl("../../../assets/icons/chevron-down" + (Theme.darkMode ? "-dark.svg" : ".svg"))
        }
        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Theme.rule
    }
}
