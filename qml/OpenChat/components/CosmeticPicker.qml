import QtQuick
import QtQuick.Controls.Basic
import OpenChat
import OpenChat.Native

// Settings → Cosmetics: one kind of collectible as a grid of tiles, None
// first, then every item of the kind from Common to Exotic. Picking a tile
// equips it at once: AppearanceSettings remembers it, and the sidebar header
// (or, for bubbles, your own messages) wears it. Each tile draws its item with
// the component that wears it, under the same tier bar as the daily case.
// Only what the account has unboxed can be picked; the rest stay in the grid,
// dimmed under a padlock, so the collection shows what is still to find.
Item {
    id: picker
    objectName: "cosmeticPicker_" + category

    // "frame", "flair", "bead", "scene" or "bubble".
    property string category: ""

    readonly property string equippedId: category === "frame" ? AppearanceSettings.avatarFrame
        : category === "flair" ? AppearanceSettings.nameFlair
        : category === "bead" ? AppearanceSettings.presenceBead
        : category === "scene" ? AppearanceSettings.profileScene
        : category === "bubble" ? AppearanceSettings.bubbleSkin : ""
    readonly property var owned: AppearanceSettings.ownedCosmetics

    // The catalogue keeps its own order within a tier; the engine's sort is
    // not stable, so that order breaks ties explicitly.
    readonly property var choices: {
        const items = Array.from(Cosmetics.items(category));
        const position = new Map(items.map((item, index) => [item.id, index]));
        items.sort((a, b) => a.rarityRank - b.rarityRank || position.get(a.id) - position.get(b.id));
        return [{ id: "", name: "None" }].concat(items);
    }
    readonly property int ownedCount: choices.filter(choice => choice.id && owned.indexOf(choice.id) >= 0).length

    readonly property int tileSpacing: 10
    readonly property int minimumTileWidth: 112
    readonly property int columns: Math.max(1, Math.floor((width + tileSpacing) / (minimumTileWidth + tileSpacing)))
    readonly property int tileWidth: Math.floor((width - (columns - 1) * tileSpacing) / columns)

    implicitHeight: column.height + 18

    function equip(id) {
        switch (category) {
        case "frame": AppearanceSettings.avatarFrame = id; break;
        case "flair": AppearanceSettings.nameFlair = id; break;
        case "bead": AppearanceSettings.presenceBead = id; break;
        case "scene": AppearanceSettings.profileScene = id; break;
        case "bubble": AppearanceSettings.bubbleSkin = id; break;
        }
    }

    // Arrows move between tiles, locked ones included so each can be read
    // out; a row holds `columns` of them.
    function focusTile(index) {
        if (index < 0 || index >= tiles.count)
            return;
        tiles.itemAt(index).forceActiveFocus(Qt.TabFocusReason);
    }

    Column {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: 8
        spacing: 14

        Item {
            width: parent.width
            height: Math.max(caption.height, count.height)

            Text {
                id: caption
                width: parent.width - count.implicitWidth - 16
                wrapMode: Text.WordWrap
                text: picker.category === "frame" ? "Rings your picture at the top of the sidebar and on your call tile."
                    : picker.category === "flair" ? "Styles your name at the top of the sidebar."
                    : picker.category === "bead" ? "Dresses the dot that shows your status."
                    : picker.category === "scene" ? "Paints the backdrop behind your name and picture."
                    : "Skins the messages you send."
                color: Theme.textSecondary
                font.family: Theme.uiFont
                font.pixelSize: 13
                renderType: Text.NativeRendering
            }
            Text {
                id: count
                objectName: "cosmeticPickerCount"
                anchors.right: parent.right
                text: picker.ownedCount + " of " + (picker.choices.length - 1) + " unboxed"
                color: Theme.textSecondary
                font.family: Theme.uiFont
                font.pixelSize: 12
                renderType: Text.NativeRendering
            }
        }

        Grid {
            columns: picker.columns
            spacing: picker.tileSpacing

            Repeater {
                id: tiles
                model: picker.choices

                Rectangle {
                    id: tile
                    required property var modelData
                    required property int index
                    readonly property bool none: !modelData.id
                    readonly property string itemId: none ? "" : modelData.id
                    readonly property bool locked: !none && picker.owned.indexOf(itemId) < 0
                    readonly property bool selected: picker.equippedId === itemId
                    readonly property bool hovered: tileMouse.containsMouse
                    readonly property bool lit: hovered && !locked
                    readonly property color tierColor: none ? Theme.rule : modelData.rarityColor
                    readonly property color tierInk: none ? Theme.textSecondary
                        : Theme.darkMode ? Qt.lighter(tierColor, 1.3) : Qt.darker(tierColor, 1.3)
                    objectName: "cosmeticChoice_" + (none ? picker.category + ".none" : itemId)
                    width: picker.tileWidth
                    height: 112
                    radius: 6
                    color: selected ? Theme.navSelected : lit ? Theme.buttonHover : Theme.buttonBackground
                    border.width: selected ? 2 : 1
                    border.color: selected || activeFocus ? Theme.focusBorder
                                                          : lit ? Theme.buttonBorder : Theme.inputBorder

                    // Tab lands on the equipped tile; arrows walk the rest.
                    activeFocusOnTab: selected
                    Accessible.role: Accessible.RadioButton
                    Accessible.name: none ? "None"
                        : modelData.name + ", " + modelData.rarityName + (modelData.animated ? ", animated" : "")
                          + (locked ? ", locked" : "")
                    Accessible.description: none ? "No " + Cosmetics.categoryName(picker.category)
                        : locked ? "Not unboxed yet. " + modelData.description : modelData.description
                    Accessible.checkable: !locked
                    Accessible.checked: selected
                    Accessible.onPressAction: activate()
                    Keys.onReturnPressed: activate()
                    Keys.onEnterPressed: activate()
                    Keys.onSpacePressed: activate()
                    Keys.onLeftPressed: picker.focusTile(index - 1)
                    Keys.onRightPressed: picker.focusTile(index + 1)
                    Keys.onUpPressed: picker.focusTile(index - picker.columns)
                    Keys.onDownPressed: picker.focusTile(index + picker.columns)

                    function activate() {
                        if (!locked)
                            picker.equip(itemId);
                    }

                    ToolTip.visible: hovered && !none
                    ToolTip.delay: 700
                    ToolTip.text: none ? ""
                        : locked ? modelData.description + "\nNot unboxed yet: it can drop from the daily case."
                        : modelData.description

                    // Everything that shows the item, dimmed while it is locked.
                    Item {
                        objectName: "cosmeticChoiceArt"
                        anchors.fill: parent
                        opacity: tile.locked ? 0.38 : 1

                        // The tier's glow, rising from the bottom edge.
                        Rectangle {
                            visible: !tile.none
                            anchors.fill: parent
                            anchors.margins: tile.border.width
                            radius: tile.radius - 1
                            gradient: Gradient {
                                GradientStop { position: 0.45; color: "transparent" }
                                GradientStop {
                                    position: 1
                                    color: Qt.rgba(tile.tierColor.r, tile.tierColor.g, tile.tierColor.b,
                                                   tile.selected ? 0.26 : 0.16)
                                }
                            }
                        }
                        Rectangle { x: 7; y: 1; width: parent.width - 14; height: 1; color: Theme.glossStrong }

                        CosmeticPreview {
                            x: 6
                            y: 10
                            width: parent.width - 12
                            height: 56
                            item: tile.modelData
                            stockCategory: picker.category
                            // Pointing at an animated item shows it moving.
                            animate: tile.hovered || tile.activeFocus
                        }
                        Text {
                            objectName: "cosmeticChoiceName"
                            x: 6
                            y: 70
                            width: parent.width - 12
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideRight
                            text: tile.modelData.name
                            color: Theme.textPrimary
                            font.family: Theme.uiFont
                            font.pixelSize: 12
                            // Narrow tiles shrink a long name before eliding it.
                            fontSizeMode: Text.HorizontalFit
                            minimumPixelSize: 9
                        }
                        Text {
                            objectName: "cosmeticChoiceRarity"
                            x: 6
                            y: 87
                            width: parent.width - 12
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideRight
                            text: tile.none ? "Default" : tile.modelData.rarityName
                            color: tile.tierInk
                            font.family: Theme.uiFont
                            font.pixelSize: 10
                            font.bold: true
                            font.capitalization: Font.AllUppercase
                            font.letterSpacing: 0.6
                        }
                        Rectangle {
                            visible: !tile.none
                            x: tile.border.width + 3
                            y: parent.height - tile.border.width - 5
                            width: parent.width - 2 * x
                            height: 3
                            radius: 1.5
                            color: tile.tierColor
                        }

                        // Previews stand still until pointed at; a play mark
                        // in the corner says which items move once worn.
                        Rectangle {
                            objectName: "cosmeticChoiceAnimated"
                            visible: !tile.none && tile.modelData.animated === true
                            x: 6
                            y: 6
                            width: 18
                            height: 18
                            radius: 9
                            color: Theme.fieldBackground
                            border.color: tile.tierColor
                            Canvas {
                                anchors.fill: parent
                                property color ink: tile.tierInk
                                onInkChanged: requestPaint()
                                onPaint: {
                                    const context = getContext("2d");
                                    context.reset();
                                    context.fillStyle = ink;
                                    context.beginPath();
                                    context.moveTo(7, 5);
                                    context.lineTo(13, 9);
                                    context.lineTo(7, 13);
                                    context.closePath();
                                    context.fill();
                                }
                            }
                        }
                    }

                    // Keyboard focus, inside the tile so the page's clip never cuts it.
                    Rectangle {
                        visible: tile.activeFocus
                        anchors.fill: parent
                        anchors.margins: tile.border.width + 2
                        radius: tile.radius - 2
                        color: "transparent"
                        border.color: Theme.focusBorder
                        opacity: 0.7
                    }

                    // Not unboxed yet: a padlock where the check would be.
                    Rectangle {
                        objectName: "cosmeticChoiceLock"
                        visible: tile.locked
                        x: parent.width - width - 6
                        y: 6
                        width: 18
                        height: 18
                        radius: 9
                        color: Theme.fieldBackground
                        border.color: Theme.inputBorder
                        Canvas {
                            anchors.fill: parent
                            property color ink: Theme.textSecondary
                            onInkChanged: requestPaint()
                            onPaint: {
                                const context = getContext("2d");
                                context.reset();
                                context.strokeStyle = ink;
                                context.lineWidth = 1.4;
                                context.beginPath();
                                context.moveTo(6.6, 9);
                                context.lineTo(6.6, 7.2);
                                context.arc(9, 7.2, 2.4, Math.PI, 0, false);
                                context.lineTo(11.4, 9);
                                context.stroke();
                                context.fillStyle = ink;
                                context.fillRect(5.2, 8.6, 7.6, 5.4);
                            }
                        }
                    }

                    // The equipped tile's check, an Aero bead in the corner.
                    Rectangle {
                        objectName: "cosmeticChoiceCheck"
                        visible: tile.selected
                        x: parent.width - width - 6
                        y: 6
                        width: 18
                        height: 18
                        radius: 9
                        border.color: Theme.switchBottom
                        gradient: Gradient {
                            GradientStop { position: 0; color: Theme.switchTop }
                            GradientStop { position: 1; color: Theme.switchBottom }
                        }
                        Text {
                            anchors.centerIn: parent
                            text: "✓"
                            color: "#ffffff"
                            font.pixelSize: 11
                            font.bold: true
                        }
                    }

                    MouseArea {
                        id: tileMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: tile.locked ? Qt.ArrowCursor : Qt.PointingHandCursor
                        onClicked: tile.activate()
                    }
                }
            }
        }
    }
}
