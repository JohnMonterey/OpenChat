import QtQuick
import OpenChat

// The wide column's first box (SPEC §5.6), MySpace's "extended network"
// line made a true statement: "<First> is in your contacts." on a contact's
// page (and in the editor preview, which shows what contacts see), "This is
// your profile. Your contacts see it just like this." on your own. Never on a
// stub, and never a "contacts since" date nobody knows.
ProfileBox {
    id: module
    objectName: "profileBannerBox"
    property var view: null
    readonly property bool own: view !== null && view.profiles.isOwnProfile && !view.preview
    readonly property string sentence: own ? "This is your profile. Your contacts see it just like this."
                                           : (view ? view.ownerFirstName : "") + " is in your contacts."

    render: view ? view.render : null
    pad: 12
    Accessible.name: sentence

    Row {
        width: parent.width
        spacing: 9
        ProfileGlyph {
            anchors.verticalCenter: parent.verticalCenter
            width: 18
            height: 18
            kind: "person"
            ink: module.render ? module.render.labelColor : "#1f4e79"
        }
        Text {
            objectName: "profileBannerText"
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 27
            wrapMode: Text.Wrap
            text: module.sentence
            textFormat: Text.PlainText
            color: module.render ? module.render.bodyColor : "black"
            style: module.render && module.render.textHalo ? Text.Outline : Text.Normal
            styleColor: module.render ? module.render.haloColor : "transparent"
            font.family: module.render && module.render.headingFamily.length > 0 ? module.render.headingFamily
                                                                                  : Theme.uiFont
            font.pixelSize: Math.round((module.own ? 14 : 15) * (module.render ? module.render.headingFactor : 1))
            // The interface face has a real DemiBold; the bundled heading
            // faces are already their heavier cut.
            font.weight: module.render && module.render.headingFamily.length > 0 ? Font.Normal : Font.DemiBold
            renderType: Text.NativeRendering
        }
    }
}
