import QtQuick
import OpenChat

// "<First>'s Blurbs" (SPEC §5.7), the wide column's header family: "About
// me:" and "Who I'd like to meet:" as sub-heads, each followed by the owner's
// words as plain text with their line breaks (a blank line is a paragraph
// gap). Long text is never clipped: the page scrolls. Either part hides when
// empty, and the box when both are.
ProfileBox {
    id: module
    objectName: "profileBlurbsBox"
    property var view: null
    readonly property var page: view ? view.page : null
    readonly property bool preview: view ? view.preview : false
    // For the editor's click targets.
    readonly property Item aboutItem: aboutPart
    readonly property Item meetItem: meetPart

    render: view ? view.render : null
    alt: true
    pad: 12
    title: view ? view.possessive(view.ownerFirstName) + " Blurbs" : ""

    component SubHead: Text {
        textFormat: Text.PlainText
        color: module.render ? module.render.blurbSubheadColor : "black"
        style: module.render && module.render.textHalo ? Text.Outline : Text.Normal
        styleColor: module.render ? module.render.haloColor : "transparent"
        font.family: module.render && module.render.headingFamily.length > 0 ? module.render.headingFamily
                                                                              : Theme.uiFont
        font.pixelSize: module.render ? module.render.subheadPixelSize : 14
        font.bold: module.render ? module.render.headingBold : true
        renderType: Text.NativeRendering
    }
    component Paragraphs: Text {
        width: parent ? parent.width : 0
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        color: module.render ? module.render.bodyColor : "black"
        style: module.render && module.render.textHalo ? Text.Outline : Text.Normal
        styleColor: module.render ? module.render.haloColor : "transparent"
        font.family: module.render && module.render.bodyFamily.length > 0 ? module.render.bodyFamily : Theme.uiFont
        font.pixelSize: module.render ? module.render.bodyPixelSize : 13
        lineHeight: 1.18
        renderType: Text.NativeRendering
    }

    // 16 px from one part's words to the next sub-head, as the mockups
    // draw it (4 + 8 + 4).
    Column {
        width: parent.width
        spacing: 16

        Column {
            id: aboutPart
            objectName: "profileAboutMe"
            visible: module.page !== null && module.page.aboutMe.length > 0
            width: parent.width
            spacing: 4
            SubHead { text: "About me:" }
            Paragraphs { text: module.page ? module.page.aboutMe : "" }
        }
        Column {
            id: meetPart
            objectName: "profileMeet"
            visible: module.page !== null && module.page.meet.length > 0
            width: parent.width
            spacing: 4
            SubHead { text: "Who I'd like to meet:" }
            Paragraphs { text: module.page ? module.page.meet : "" }
        }
    }
}
