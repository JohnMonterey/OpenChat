pragma Singleton

import QtQuick

QtObject {
    // Segoe UI is the Aero skin's native face, but it only ships on Windows.
    // Ask the font database once rather than letting every Text miss and pay for
    // the alias-population walk, and fall back to the platform's own UI font.
    readonly property string uiFont: Qt.fontFamilies().indexOf("Segoe UI") >= 0
                                         ? "Segoe UI"
                                         : Application.font.family

    readonly property int sidebarWidth: 268
    readonly property int conversationHeaderHeight: 111
    // The in-call header replaces the conversation header and needs room for two
    // callers side by side plus the call controls beneath them.
    readonly property int callHeaderHeight: 212
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

    // Affirmative / destructive actions on contact requests. Each carries the
    // presence bead's Aero treatment: a three-stop vertical gradient under a gloss
    // highlight, bounded by a darker rim. Softened off the pure hues so the pair
    // sits with the sidebar rather than shouting over it.
    readonly property color acceptTop: "#a9dd7b"
    readonly property color acceptMid: "#7cc848"
    readonly property color acceptBottom: "#61ac2e"
    readonly property color acceptBorder: "#58992b"
    readonly property color declineTop: "#efa79d"
    readonly property color declineMid: "#db7264"
    readonly property color declineBottom: "#c65a4c"
    readonly property color declineBorder: "#b04c40"

    // In-call surface. The speaking ring is the one saturated green in the
    // interface, so an active talker reads instantly against the Aero blues; the
    // glow is the same hue at low alpha, spread outside the ring.
    readonly property color callBackdropTop: "#eef6fb"
    readonly property color callBackdropBottom: "#dfeef7"
    readonly property color speakingRing: "#3fbf54"
    readonly property color speakingGlow: "#5fd47a"
    readonly property color idleRing: "#c3d3de"
    readonly property color endCallTop: "#ef9a8f"
    readonly property color endCallMid: "#d9604f"
    readonly property color endCallBottom: "#bd4737"
    readonly property color endCallBorder: "#a83e2f"
}
