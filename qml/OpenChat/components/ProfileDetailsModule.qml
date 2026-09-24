import QtQuick
import OpenChat

// "<First>'s Details" (SPEC §5.5): Here for, Hometown, Zodiac sign,
// Occupation, Education and Languages, only the filled rows. Hidden from
// viewers when empty.
ProfileBox {
    id: module
    objectName: "profileDetailsBox"
    property var view: null

    render: view ? view.render : null
    pad: 8
    title: view ? view.possessive(view.ownerFirstName) + " Details" : ""

    ProfileInfoTable {
        width: parent.width
        render: module.render
        rows: module.view ? module.view.page.detailRows : []
    }
}
