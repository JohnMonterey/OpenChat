import QtQuick
import OpenChat

// One person on the call screen: their picture, their name, and the ring that
// says whether they are the one talking.
//
// The ring is always drawn, in a muted grey when quiet and in green when
// speaking, so a talker is signalled by the ring CHANGING COLOUR rather than by
// one appearing — nothing shifts position or size, which is what keeps a
// two-person call from flickering as the conversation goes back and forth. The
// outer glow is a second, softer ring that fades in behind it, giving the
// speaker some weight without moving the layout.
Item {
    id: participant

    required property string name
    required property string avatarKey
    property bool speaking: false
    // 0..1, the smoothed voice level. Only ever widens the glow, never the
    // layout, so a loud talker reads as brighter rather than bigger.
    property real level: 0.0
    property bool muted: false
    property int avatarSize: 74

    implicitWidth: 132
    implicitHeight: avatarSize + 40

    Item {
        id: avatarBlock
        width: participant.avatarSize
        height: participant.avatarSize
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top

        // The glow: outside the ring, keyed off the voice level so it breathes
        // with the speaker. Invisible when quiet, so it costs nothing to draw.
        Rectangle {
            objectName: "speakingGlow"
            anchors.centerIn: parent
            width: parent.width + 22
            height: parent.height + 22
            radius: 10
            color: "transparent"
            border.width: 6
            border.color: Theme.speakingGlow
            opacity: participant.speaking
                     ? 0.20 + 0.35 * Math.min(1.0, participant.level * 3.0)
                     : 0.0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: 120 } }
        }

        // The ring itself, drawn just outside the avatar's own 1px border.
        Rectangle {
            objectName: "speakingRing"
            anchors.centerIn: parent
            width: parent.width + 8
            height: parent.height + 8
            radius: 8
            color: "transparent"
            border.width: 3
            border.color: participant.speaking ? Theme.speakingRing : Theme.idleRing
            Behavior on border.color { ColorAnimation { duration: 120 } }
        }

        Avatar {
            anchors.fill: parent
            cornerRadius: 6
            avatarKey: participant.avatarKey
        }

        // A muted participant is marked on the picture, because a muted person
        // is silent for a reason and should not read as merely "not talking".
        Rectangle {
            objectName: "mutedBadge"
            visible: participant.muted
            width: 22
            height: 22
            radius: 11
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: -4
            color: "#f4f8fb"
            border.width: 1
            border.color: Theme.inputBorder

            Rectangle {
                anchors.centerIn: parent
                width: 13
                height: 2
                radius: 1
                rotation: -45
                color: Theme.declineBottom
            }
        }
    }

    Text {
        objectName: "participantName"
        anchors.top: avatarBlock.bottom
        anchors.topMargin: 10
        anchors.horizontalCenter: parent.horizontalCenter
        width: parent.width
        horizontalAlignment: Text.AlignHCenter
        elide: Text.ElideRight
        text: participant.name
        color: Theme.textPrimary
        font.family: Theme.uiFont
        font.pixelSize: 14
        renderType: Text.NativeRendering
    }
}
