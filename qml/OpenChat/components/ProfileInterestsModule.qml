import QtQuick
import OpenChat

// "<First>'s Interests" (SPEC §5.5): General, Music, Movies, Television,
// Books and Heroes, only the filled rows. Hidden from viewers when empty.
ProfileBox {
    id: module
    objectName: "profileInterestsBox"
    property var view: null

    render: view ? view.render : null
    pad: 8
    title: view ? view.possessive(view.ownerFirstName) + " Interests" : ""

    ProfileInfoTable {
        width: parent.width
        render: module.render
        rows: module.view ? module.view.page.interestRows : []
    }
}
