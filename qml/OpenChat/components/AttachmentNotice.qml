import QtQuick
import QtQuick.Shapes
import OpenChat

// Why something just picked (or dropped, or pasted) will not be sent: one
// line in the notice colours above the tray ("Files up to 16 MB can be
// sent."). Its cross, or the next thing attached, puts it away.
Item {
    id: notice
    objectName: "attachmentNotice"
    property var controller: null
    readonly property string message: controller !== null && typeof controller.attachmentNotice === "string"
                                      ? controller.attachmentNotice : ""
    readonly property bool shown: message.length > 0

    visible: shown
    implicitHeight: 22
    Accessible.role: Accessible.AlertMessage
    Accessible.name: notice.message

    Rectangle {
        anchors.fill: parent
        radius: 3
        color: Theme.warningBackground
        border.width: 1
        border.color: Theme.noticeBorder
    }
    Text {
        objectName: "attachmentNoticeText"
        x: 9
        anchors.verticalCenter: parent.verticalCenter
        width: dismiss.x - x - 4
        text: notice.message
        textFormat: Text.PlainText
        elide: Text.ElideRight
        color: Theme.noticeText
        font.family: Theme.uiFont
        font.pixelSize: 12
        renderType: Text.NativeRendering
    }
    Item {
        id: dismiss
        objectName: "attachmentNoticeDismiss"
        anchors.right: parent.right
        width: 22
        height: parent.height
        Accessible.role: Accessible.Button
        Accessible.name: "Dismiss"
        Accessible.onPressAction: notice.dismiss()

        // Even, like the 22 px slot, so centring lands on whole pixels.
        Shape {
            anchors.centerIn: parent
            width: 8
            height: 8
            ShapePath {
                fillColor: "transparent"
                strokeColor: dismissMouse.containsMouse ? Theme.textPrimary : Theme.noticeText
                strokeWidth: 1.3
                capStyle: ShapePath.RoundCap
                PathSvg { path: "M 0 0 L 8 8 M 8 0 L 0 8" }
            }
        }
        MouseArea {
            id: dismissMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: notice.dismiss()
        }
    }

    function dismiss() {
        if (notice.controller !== null && typeof notice.controller.clearAttachmentNotice === "function")
            notice.controller.clearAttachmentNotice();
    }
}
