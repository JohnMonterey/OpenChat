import QtQuick
import OpenChat

// Choose what to share: a display, or one window.
//
// The list is enumerated when the dialog opens and never cached, because a
// window can close between one press and the next; picking a row that has since
// gone away is refused by the controller and reported as an error rather than
// leaving a capture half-started. Dismissing the dialog starts nothing, which
// is the whole of "the user cancelled the picker".
Item {
    id: root
    objectName: "screenSharePicker"

    required property var controller
    property bool open: false
    anchors.fill: parent
    visible: root.open

    // Rebuilt on every open, deliberately.
    property var sources: []

    function show() {
        root.sources = root.controller ? root.controller.screenShareSources() : [];
        root.open = true;
    }

    function dismiss() {
        root.open = false;
        root.sources = [];
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.dialogScrim

        MouseArea {
            anchors.fill: parent
            onClicked: root.dismiss()
        }
    }

    Rectangle {
        id: card
        anchors.centerIn: parent
        width: Math.min(440, parent.width - 48)
        height: Math.min(parent.height - 48, column.y + column.height + 20)
        radius: 6
        color: Theme.contentBackground
        border.width: 1
        border.color: Theme.inputBorder

        MouseArea { anchors.fill: parent }

        Rectangle { x: 6; y: 1; width: parent.width - 12; height: 1; color: Theme.insetTop }

        Column {
            id: column
            x: 20
            y: 18
            width: parent.width - 40
            spacing: 12

            Text {
                objectName: "screenSharePickerTitle"
                width: parent.width
                text: "Share your screen"
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 15
                font.bold: true
                renderType: Text.NativeRendering
            }

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                visible: root.sources.length === 0
                text: "Nothing here can be captured. On some desktops screen sharing has to "
                      + "be allowed in system settings first."
                color: Theme.textSecondary
                font.family: Theme.uiFont
                font.pixelSize: 13
                renderType: Text.NativeRendering
            }

            ListView {
                id: sourceList
                objectName: "screenSourceList"
                width: parent.width
                height: Math.min(260, contentHeight)
                visible: root.sources.length > 0
                clip: true
                model: root.sources
                boundsBehavior: Flickable.StopAtBounds
                spacing: 2

                delegate: Rectangle {
                    id: sourceRow
                    required property int index
                    required property var modelData
                    objectName: "screenSource_" + index
                    width: sourceList.width
                    height: 34
                    radius: 4
                    color: rowMouse.containsMouse ? Theme.buttonMid : "transparent"
                    border.width: rowMouse.containsMouse ? 1 : 0
                    border.color: Theme.buttonBorder

                    Row {
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 9

                        // A display for a screen, a stacked pane for a window,
                        // so the two kinds are told apart at a glance.
                        Item {
                            width: 16
                            height: 16
                            anchors.verticalCenter: parent.verticalCenter

                            Rectangle {
                                visible: sourceRow.modelData.isScreen
                                x: 0; y: 3; width: 16; height: 10; radius: 2
                                color: "transparent"
                                border.width: 1.5
                                border.color: Theme.textSecondary
                            }
                            Rectangle {
                                visible: sourceRow.modelData.isScreen
                                x: 5.5; y: 13; width: 5; height: 1.5
                                color: Theme.textSecondary
                            }
                            Rectangle {
                                visible: !sourceRow.modelData.isScreen
                                x: 1; y: 2; width: 13; height: 12; radius: 2
                                color: "transparent"
                                border.width: 1.5
                                border.color: Theme.textSecondary
                            }
                            Rectangle {
                                visible: !sourceRow.modelData.isScreen
                                x: 2.5; y: 4.5; width: 10; height: 1.5
                                color: Theme.textSecondary
                            }
                        }

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            width: sourceList.width - 46
                            elide: Text.ElideRight
                            text: sourceRow.modelData.name
                            color: Theme.textPrimary
                            font.family: Theme.uiFont
                            font.pixelSize: 13
                            renderType: Text.NativeRendering
                        }
                    }

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            const chosen = sourceRow.index;
                            root.dismiss();
                            if (root.controller)
                                root.controller.startScreenShare(chosen);
                        }
                    }
                }
            }

            Row {
                anchors.right: parent.right
                spacing: 10

                CallActionButton {
                    objectName: "screenSharePickerCancel"
                    label: "Cancel"
                    accent: "neutral"
                    onClicked: root.dismiss()
                }
            }
        }
    }
}
