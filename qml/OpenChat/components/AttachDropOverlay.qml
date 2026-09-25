import QtQuick
import QtQuick.Shapes
import OpenChat

// Files dragged over the open chat: while they hover, a dashed well over the
// conversation says they will be attached, and dropping them puts them in
// the composer's tray. Only local files count (a link dragged from a browser
// does not), and only while `accepting`: a chat is open with its messages
// shown, nothing covers it (a profile, a dialog, the viewer) and no message
// is being edited.
DropArea {
    id: drop
    objectName: "attachDropOverlay"
    property bool accepting: false
    signal filesDropped(var files)
    readonly property bool hovering: containsDrag && accepting

    enabled: accepting
    keys: ["text/uri-list"]

    function localFiles(urls) {
        const files = [];
        for (let i = 0; i < urls.length; ++i) {
            if (String(urls[i]).startsWith("file:"))
                files.push(urls[i]);
        }
        return files;
    }

    onEntered: drag => {
        if (drop.localFiles(drag.urls).length === 0)
            drag.accepted = false;
    }
    onDropped: event => {
        const files = drop.localFiles(event.urls);
        if (files.length === 0)
            return;
        event.acceptProposedAction();
        drop.filesDropped(files);
    }

    Item {
        anchors.fill: parent
        visible: drop.hovering
        Accessible.ignored: true

        // The chat stays faintly there under the well.
        Rectangle {
            anchors.fill: parent
            color: Theme.contentBackground
            opacity: 0.72
        }
        Shape {
            anchors.fill: parent
            anchors.margins: 14
            ShapePath {
                fillColor: Qt.rgba(Theme.navSelected.r, Theme.navSelected.g, Theme.navSelected.b, 0.8)
                strokeColor: Theme.focusBorder
                strokeWidth: 2
                strokeStyle: ShapePath.DashLine
                dashPattern: [4, 3]
                PathRectangle {
                    x: 1
                    y: 1
                    width: drop.width - 30
                    height: drop.height - 30
                    radius: 10
                }
            }
        }
        Column {
            anchors.centerIn: parent
            spacing: 8

            AttachmentChip {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 52
                height: 52
                kind: 4
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Drop to attach"
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 16
                font.bold: true
                renderType: Text.NativeRendering
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Photos, videos, audio and files, sent with your next message"
                color: Theme.textSecondary
                font.family: Theme.uiFont
                font.pixelSize: 13
                renderType: Text.NativeRendering
            }
        }
    }
}
