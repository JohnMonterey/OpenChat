import QtQuick
import OpenChat

// A field's label with its right-aligned "n / max" counter (SPEC §14.3):
// both 12 px textSecondaryStrong, the counter turning warningText in the last
// 10% of the limit so the owner sees the end coming before typing stops.
Item {
    id: label

    property string text: ""
    property int count: 0
    property int limit: 0 // 0: no counter
    readonly property bool nearLimit: limit > 0 && count >= Math.ceil(limit * 0.9)

    implicitWidth: 200
    implicitHeight: 18
    height: implicitHeight

    Text {
        width: parent.width - (counter.visible ? counter.implicitWidth + 8 : 0)
        elide: Text.ElideRight
        text: label.text
        color: Theme.textSecondaryStrong
        font.family: Theme.uiFont
        font.pixelSize: 12
        renderType: Text.NativeRendering
    }
    Text {
        id: counter
        objectName: "profileFieldCounter"
        visible: label.limit > 0
        anchors.right: parent.right
        text: label.count + " / " + label.limit
        color: label.nearLimit ? Theme.warningText : Theme.textSecondaryStrong
        font.family: Theme.uiFont
        font.pixelSize: 12
        renderType: Text.NativeRendering
    }
}
