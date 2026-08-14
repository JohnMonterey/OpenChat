pragma Singleton

import QtQuick

QtObject {
    readonly property string uiFont: "Segoe UI"

    readonly property int sidebarWidth: 268
    readonly property int conversationHeaderHeight: 111
    readonly property int composerHeight: 104

    readonly property color sidebarTop: "#f0f9fd"
    readonly property color sidebarBottom: "#e5f2f9"
    readonly property color selectedTop: "#d4e8f3"
    readonly property color selectedBottom: "#eaf5fa"
    readonly property color categoryText: "#35618f"
    readonly property color rule: "#bfced8"

    readonly property color contentBackground: "#f8fbfd"
    readonly property color contentBottom: "#f5f9fc"
    readonly property color textPrimary: "#2b3b53"
    readonly property color textSecondary: "#8190a6"
    readonly property color iconInk: "#425570"
    readonly property color accentBlue: "#78acd3"
    readonly property color inputBorder: "#b8c7d5"
}
