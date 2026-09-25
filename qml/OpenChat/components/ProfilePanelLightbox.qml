import QtQuick
import OpenChat
import OpenChat.Native

// A panel's picture opened large over the page: the whole picture fitted
// into the view on a dimmed page, its caption under it, and the block's
// other pictures a click or an arrow key away. Esc, a click outside the
// picture or the close button puts it away.
FocusScope {
    id: box
    objectName: "profilePanelLightbox"
    property var images: []
    property int index: 0
    readonly property bool opened: visible
    readonly property var current: images.length > 0 ? images[Math.max(0, Math.min(index, images.length - 1))] : null

    function show(list, at) {
        box.images = list || [];
        box.index = at;
        box.visible = box.images.length > 0;
        if (box.visible)
            box.forceActiveFocus(Qt.OtherFocusReason);
    }
    function close() {
        box.visible = false;
        box.images = [];
    }
    function step(delta) {
        if (box.images.length > 1)
            box.index = (box.index + delta + box.images.length) % box.images.length;
    }

    visible: false
    Accessible.role: Accessible.Dialog
    Accessible.name: box.current && box.current.caption ? "Picture: " + box.current.caption : "Picture"
    Keys.onEscapePressed: box.close()
    Keys.onLeftPressed: box.step(-1)
    Keys.onRightPressed: box.step(1)

    Rectangle {
        anchors.fill: parent
        color: "#d8101318"
        MouseArea {
            anchors.fill: parent
            onClicked: box.close()
        }
    }

    ProfilePanelImage {
        id: picture
        objectName: "profilePanelLightboxImage"
        readonly property real room: caption.visible ? caption.height + 24 : 0
        anchors.centerIn: parent
        anchors.verticalCenterOffset: -room / 2
        width: parent.width - 120
        height: parent.height - 80 - room
        mediaKey: box.current ? box.current.mediaKey : ""
        crop: false
        radius: 4
    }
    Text {
        id: caption
        visible: text.length > 0
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 30
        width: Math.min(parent.width - 80, 640)
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        text: box.current ? (box.current.caption || "") : ""
        color: "#f2f2f2"
        font.family: Theme.uiFont
        font.pixelSize: 15
        renderType: Text.NativeRendering
    }

    component RoundButton: Rectangle {
        id: button
        property string glyph: ""
        property string label: ""
        signal clicked()
        width: 40
        height: 40
        radius: 20
        color: area.containsMouse ? "#60ffffff" : "#38ffffff"
        border.width: 1
        border.color: "#80ffffff"
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: button.label
        Keys.onReturnPressed: button.clicked()
        Keys.onSpacePressed: button.clicked()
        ProfileGlyph {
            anchors.centerIn: parent
            width: 20
            height: 20
            kind: button.glyph
            ink: "white"
        }
        ProfileFocusRing {
            anchors.fill: parent
            radius: 20
            shown: button.activeFocus
            onDark: true
        }
        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: button.clicked()
        }
    }

    RoundButton {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 16
        glyph: "cross"
        label: "Close"
        onClicked: box.close()
    }
    RoundButton {
        visible: box.images.length > 1
        anchors.left: parent.left
        anchors.leftMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        glyph: "back"
        label: "Previous picture"
        onClicked: box.step(-1)
    }
    RoundButton {
        visible: box.images.length > 1
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        glyph: "chevron"
        label: "Next picture"
        onClicked: box.step(1)
    }
}
