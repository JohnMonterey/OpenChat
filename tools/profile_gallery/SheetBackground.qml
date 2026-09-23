import QtQuick
import OpenChat

// The sidebar's own gradient, so every item is judged where it will live.
Rectangle {
    anchors.fill: parent
    gradient: Gradient {
        GradientStop { position: 0; color: Theme.sidebarTop }
        GradientStop { position: 1; color: Theme.sidebarBottom }
    }
}
