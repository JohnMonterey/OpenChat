import QtQuick
import OpenChat

// The wide column's one box on a contact who has no page yet (SPEC §5.9): an
// old client, or someone who never saved one. While the app is waiting for
// their page (a background request, or right after accepting them) it says so
// with a small spinner; opening the page itself never asks for it.
ProfileBox {
    id: module
    objectName: "profileNoPageBox"
    property var view: null
    readonly property bool loading: view !== null && view.profiles.pageLoading
    readonly property string first: view ? view.ownerFirstName : ""

    render: view ? view.render : null
    pad: 12

    Row {
        width: parent.width
        spacing: 8
        ProfileSpinner {
            objectName: "profileNoPageSpinner"
            visible: module.loading
            anchors.verticalCenter: parent.verticalCenter
            ink: module.render ? module.render.labelColor : "#1f4e79"
            running: module.view !== null && module.view.animate
        }
        Text {
            objectName: "profileNoPageText"
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - (module.loading ? 20 : 0)
            wrapMode: Text.Wrap
            text: module.loading ? "Getting " + module.view.possessive(module.first) + " page…"
                                 : module.first + " hasn't shared a profile page yet."
            textFormat: Text.PlainText
            color: module.render ? module.render.bodyColor : "black"
            font.family: module.render && module.render.bodyFamily.length > 0 ? module.render.bodyFamily : Theme.uiFont
            font.pixelSize: module.render ? module.render.bodyPixelSize + 1 : 14
            renderType: Text.NativeRendering
        }
    }
}
