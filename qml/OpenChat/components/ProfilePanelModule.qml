import QtQuick
import OpenChat
import OpenChat.Native

// One of the owner's own panels (docs/profile-panels.md): a box like every
// other module, titled with the owner's words and icon, holding their blocks
// in order. A panel with its own colours wears its own render (the page's
// knobs with its colours, through the same readability pass); otherwise the
// page's, in the column family it sits in. Viewers never see an empty
// panel, nor an empty block; the editor's preview shows both as places to
// fill in.
ProfileBox {
    id: module
    objectName: "profilePanelBox_" + panelId
    property var view: null
    property int panelId: 0
    readonly property var page: view ? view.page : null
    readonly property bool preview: view ? view.preview : false

    // Re-read only when this panel (or the set of blocks) changed, so typing
    // in one block never rebuilds another.
    property var info: ({})
    property var blockIds: []
    property var panelRender: null
    function refresh() {
        if (!module.page || module.panelId <= 0) {
            module.info = {};
            module.panelRender = null;
            return;
        }
        module.info = module.page.panel(module.panelId);
        module.panelRender = module.page.panelRender(module.panelId);
        const ids = module.info.blockIds || [];
        if (ids.length !== module.blockIds.length || ids.some((id, i) => id !== module.blockIds[i]))
            module.blockIds = ids;
    }
    onPanelIdChanged: refresh()
    onPageChanged: refresh()
    Component.onCompleted: refresh()
    Connections {
        target: module.page
        function onPanelChanged(id) {
            if (id === module.panelId)
                module.refresh();
        }
        function onPanelsChanged() { module.refresh(); }
        function onListsChanged() { module.refresh(); } // its column or content
    }

    readonly property bool hasPreviewContent: module.info.hasContent === true
    readonly property bool present: module.info.id !== undefined
    readonly property Item headerItemForEditor: module.headerItem

    render: module.panelRender
    alt: module.info.ownColours !== true && module.info.column === Profile.WideColumn
    title: module.info.showTitle === false ? "" : (module.info.shownTitle || "")
    glyph: module.info.glyph || ""

    Column {
        width: parent.width
        spacing: 12

        Repeater {
            model: module.blockIds
            delegate: ProfilePanelBlock {
                required property var modelData
                width: parent ? parent.width : 0
                view: module.view
                panel: module
                blockId: modelData
            }
        }
    }
}
