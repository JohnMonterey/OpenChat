import QtQuick
import QtQuick.Shapes
import OpenChat

// How a photo's or a video's transfer stands, over its picture: a dark glass
// pill with a ring that fills as the parts go or come ("Sending… 40%",
// "Receiving… 3 of 7", "Waiting for the call to end") and, when it stopped
// for good, why. The ring of one's own sending holds a cross that stops it;
// otherwise an arrow says which way it is going. Nothing in it moves while it
// waits: the ring only fills when a part does.
Item {
    id: overlay
    objectName: "chatTransferOverlay"
    required property MessageDelegate row
    readonly property bool transferring: row.transferState === 0
    readonly property bool stopped: row.transferState >= 2
    readonly property real progress: Math.max(0, Math.min(1, row.transferProgress))

    visible: transferring || stopped

    Rectangle {
        id: pill
        // The mark at the left, then the words, elided if the picture is
        // narrow.
        readonly property real markWidth: overlay.transferring ? 28 : 24
        anchors.centerIn: parent
        // Whole pixels, so the drawn pill and the marks inside it agree.
        width: Math.min(Math.floor(parent.width - 12), 5 + markWidth + 9 + Math.ceil(status.implicitWidth) + 14)
        height: 38
        radius: 19
        color: "#b0101820"
        border.width: 1
        border.color: Theme.mediaChipBorder

        Item {
            anchors.fill: parent

            Item {
                id: ring
                objectName: "chatTransferRing"
                // Centred on the pill's rounded end: 19 − 28 / 2.
                x: 5
                anchors.verticalCenter: parent.verticalCenter
                width: 28
                height: 28
                visible: overlay.transferring
                Accessible.role: overlay.row.canCancel ? Accessible.Button : Accessible.ProgressBar
                Accessible.name: overlay.row.canCancel ? "Stop sending" : overlay.row.transferText
                Accessible.onPressAction: if (overlay.row.canCancel) overlay.row.cancelRequested()

                Shape {
                    preferredRendererType: Shape.CurveRenderer // smooth on the GPU too (see ProfileGlyph)
                    anchors.fill: parent
                    ShapePath {
                        fillColor: "transparent"
                        strokeColor: "#48ffffff"
                        strokeWidth: 2.5
                        PathAngleArc {
                            centerX: 14
                            centerY: 14
                            radiusX: 12
                            radiusY: 12
                            startAngle: 0
                            sweepAngle: 360
                        }
                    }
                    ShapePath {
                        fillColor: "transparent"
                        strokeColor: overlay.progress > 0 ? "#ffffff" : "transparent"
                        strokeWidth: 2.5
                        capStyle: ShapePath.RoundCap
                        PathAngleArc {
                            centerX: 14
                            centerY: 14
                            radiusX: 12
                            radiusY: 12
                            startAngle: -90
                            sweepAngle: 360 * overlay.progress
                        }
                    }
                }
                ProfileGlyph {
                    anchors.centerIn: parent
                    width: overlay.row.canCancel ? 12 : 14
                    height: width
                    kind: overlay.row.canCancel ? "cross" : overlay.row.outgoing ? "upload" : "download"
                    ink: cancelMouse.containsMouse ? "#ffffff" : "#e6ffffff"
                }
                MouseArea {
                    id: cancelMouse
                    objectName: "cancelAttachment"
                    anchors.fill: parent
                    visible: overlay.row.canCancel
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: overlay.row.cancelRequested()
                }
            }
            // Stopped for good: a warning mark in place of the ring.
            Rectangle {
                x: 9 // centred on the pill's rounded end, like the ring
                anchors.verticalCenter: parent.verticalCenter
                visible: overlay.stopped
                width: 20
                height: 20
                radius: 10
                color: overlay.row.transferState === 3 ? "#80ffffff" : "#e8806f"
                // The "!" as two bars, so it sits exactly in the middle
                // (a font's "!" is centred on its line, a pixel high).
                Rectangle {
                    x: 9
                    y: 4
                    width: 2
                    height: 8
                    radius: 1
                    color: "#ffffff"
                }
                Rectangle {
                    x: 9
                    y: 14
                    width: 2
                    height: 2
                    radius: 1
                    color: "#ffffff"
                }
            }
            Text {
                id: status
                objectName: "chatTransferText"
                x: 5 + pill.markWidth + 9
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, pill.width - x - 14)
                text: overlay.row.transferText
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: "#ffffff"
                font.family: Theme.uiFont
                font.pixelSize: 12
                renderType: Text.NativeRendering
            }
        }
    }
}
