pragma Singleton

import QtQuick
import OpenChat.Native

QtObject {
    readonly property bool darkMode: AppearanceSettings.darkMode
    function setDarkMode(enabled) { AppearanceSettings.darkMode = enabled; }

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

    readonly property color sidebarTop: darkMode ? "#223442" : "#f0f9fd"
    readonly property color sidebarBottom: darkMode ? "#1a2a37" : "#e5f2f9"
    readonly property color selectedTop: darkMode ? "#355871" : "#d4e8f3"
    readonly property color selectedBottom: darkMode ? "#253f52" : "#eaf5fa"
    readonly property color categoryText: darkMode ? "#9cc9eb" : "#35618f"
    readonly property color rule: darkMode ? "#3b5061" : "#bfced8"

    readonly property color contentBackground: darkMode ? "#18232e" : "#f8fbfd"
    readonly property color contentBottom: darkMode ? "#141e28" : "#f5f9fc"
    readonly property color textPrimary: darkMode ? "#e0eaf3" : "#2b3b53"
    readonly property color textSecondary: darkMode ? "#a0b3c5" : "#8190a6"
    readonly property color iconInk: darkMode ? "#afc8dc" : "#425570"
    readonly property color accentBlue: darkMode ? "#80bde6" : "#78acd3"
    readonly property color inputBorder: darkMode ? "#4b657a" : "#b8c7d5"

    // Affirmative / destructive actions on contact requests. Each carries the
    // presence bead's Aero treatment: a three-stop vertical gradient under a gloss
    // highlight, bounded by a darker rim. Softened off the pure hues so the pair
    // sits with the sidebar rather than shouting over it.
    readonly property color acceptTop: darkMode ? "#568544" : "#a9dd7b"
    readonly property color acceptMid: darkMode ? "#47763a" : "#7cc848"
    readonly property color acceptBottom: darkMode ? "#38632c" : "#61ac2e"
    readonly property color acceptBorder: darkMode ? "#749d62" : "#58992b"
    readonly property color declineTop: darkMode ? "#a55d53" : "#efa79d"
    readonly property color declineMid: darkMode ? "#924b42" : "#db7264"
    readonly property color declineBottom: darkMode ? "#7b3c34" : "#c65a4c"
    readonly property color declineBorder: darkMode ? "#b7786e" : "#b04c40"

    // In-call surface. The speaking ring is the one saturated green in the
    // interface, so an active talker reads instantly against the Aero blues; the
    // glow is the same hue at low alpha, spread outside the ring.
    readonly property color callBackdropTop: darkMode ? "#243846" : "#eef6fb"
    readonly property color callBackdropBottom: darkMode ? "#1a2e3c" : "#dfeef7"
    readonly property color speakingRing: "#3fbf54"
    readonly property color speakingGlow: "#5fd47a"
    readonly property color idleRing: darkMode ? "#486378" : "#c3d3de"
    readonly property color endCallTop: darkMode ? "#b56558" : "#ef9a8f"
    readonly property color endCallMid: darkMode ? "#9e4b40" : "#d9604f"
    readonly property color endCallBottom: darkMode ? "#813c33" : "#bd4737"
    readonly property color endCallBorder: darkMode ? "#c38679" : "#a83e2f"

    // Surfaces and details shared by light and dark Aero themes.
    readonly property color fieldBackground: darkMode ? "#111d28" : "#ffffff"
    readonly property color fieldAccessory: darkMode ? "#1b2c3a" : "#fbfdfe"
    readonly property color fieldDivider: darkMode ? "#3a5163" : "#d9e1e7"
    readonly property color placeholderText: darkMode ? "#91a6b8" : "#98a7ba"
    readonly property color insetTop: darkMode ? "#50000000" : "#24526878"
    readonly property color insetLeft: darkMode ? "#30000000" : "#18526878"
    readonly property color gloss: darkMode ? "#20ffffff" : "#70ffffff"
    readonly property color glossStrong: darkMode ? "#38ffffff" : "#ffffff"
    readonly property color buttonBackground: darkMode ? "#293e4f" : "#f5f8fa"
    readonly property color buttonHover: darkMode ? "#355168" : "#f8fbfd"
    readonly property color buttonDisabled: darkMode ? "#22313e" : "#eef2f5"
    readonly property color buttonBorder: darkMode ? "#526d81" : "#aebdca"
    readonly property color buttonDisabledBorder: darkMode ? "#3a4f60" : "#c5d0d9"
    readonly property color buttonText: darkMode ? "#e0eaf3" : "#3f5570"
    readonly property color buttonDisabledText: darkMode ? "#8092a3" : "#8b99aa"
    readonly property color sendText: darkMode ? "#d2e2ef" : "#596c83"
    readonly property color buttonTop: darkMode ? "#3b5367" : "#fafcfd"
    readonly property color buttonMid: darkMode ? "#2c4355" : "#eef3f7"
    readonly property color buttonBottom: darkMode ? "#233848" : "#e2eaf1"
    readonly property color focusBorder: darkMode ? "#8fc9ef" : "#4386b8"
    readonly property color cameraAccent: darkMode ? "#9cd5fa" : "#287bad"
    readonly property color headerTop: darkMode ? "#263b4a" : "#fbfdff"
    readonly property color headerBottom: darkMode ? "#1e303f" : "#edf5fb"
    readonly property color composerBackground: darkMode ? "#1e2e3b" : "#eef4f8"
    readonly property color panelBackground: darkMode ? "#233847" : "#eef6fb"
    readonly property color sectionHighlight: darkMode ? "#0affffff" : "#27ffffff"
    readonly property color categoryChevron: darkMode ? "#9dbed6" : "#557ca1"
    readonly property color avatarBorder: darkMode ? "#658299" : "#8fa8b8"
    readonly property color badgeBackground: darkMode ? "#2b4052" : "#f4f8fb"
    readonly property color errorText: darkMode ? "#ffa99e" : "#c0392b"
    readonly property color retryText: darkMode ? "#ffa99e" : "#c62828"
    readonly property color successText: darkMode ? "#95d6ac" : "#2e7d4f"
    readonly property color successFill: darkMode ? "#286846" : "#2e7d4f"
    readonly property color warningBackground: darkMode ? "#463d25" : "#fdf3d8"
    readonly property color warningBorder: darkMode ? "#796a3d" : "#e4d5a3"
    readonly property color noticeBorder: darkMode ? "#796a3d" : "#e2c98a"
    readonly property color warningText: darkMode ? "#f0d994" : "#7a6828"
    readonly property color noticeText: darkMode ? "#f0d994" : "#6b5a2b"
    readonly property color softRule: darkMode ? "#304453" : "#e4edf3"
    readonly property color dateRule: darkMode ? "#35495a" : "#d7e1e8"
    readonly property color timestampText: darkMode ? "#a0b7ca" : "#92a2b4"
    readonly property color outgoingTop: darkMode ? "#2e3e4d" : "#fdfefe"
    readonly property color outgoingBottom: darkMode ? "#243442" : "#f4f8fb"
    readonly property color outgoingBorder: darkMode ? "#536e82" : "#b9c7d4"
    readonly property color incomingTop: darkMode ? "#274961" : "#edf8ff"
    readonly property color incomingBottom: darkMode ? "#203b50" : "#e2f2fc"
    readonly property color incomingBorder: darkMode ? "#567f9d" : "#8dbbe0"
    readonly property color profileHover: darkMode ? "#18ffffff" : "#14000000"
    readonly property color searchBackground: darkMode ? "#172735" : "#f9fcfe"
    readonly property color searchIcon: darkMode ? "#a0b5c7" : "#8798aa"
    readonly property color iconHover: darkMode ? "#d0e6f5" : "#5f7386"
    readonly property color iconDisabled: darkMode ? "#62788b" : "#c3ccd5"
    readonly property color navSelected: darkMode ? "#2d485d" : "#e7f2f8"
    readonly property color requestButton: darkMode ? "#293e4f" : "#f3f7fa"
    readonly property color requestSuccess: darkMode ? "#8bd5a2" : "#5aa06a"
    readonly property color navText: darkMode ? "#a3bed3" : "#6f8eac"
    readonly property color dialogScrim: darkMode ? "#99060d14" : "#66223247"
    readonly property color tooltipBorder: darkMode ? "#668ba6" : "#9bbdd4"
    readonly property color tooltipTop: darkMode ? "#3b556b" : "#ffffff"
    readonly property color tooltipMid: darkMode ? "#2c4559" : "#f2faff"
    readonly property color tooltipBottom: darkMode ? "#223849" : "#dfedf8"
    readonly property color tooltipHighlight: darkMode ? "#38ffffff" : "#e6ffffff"
    readonly property color tooltipShadowStroke: darkMode ? "#30000000" : "#103f6482"
    readonly property color tooltipShadowFill: darkMode ? "#48000000" : "#153f6482"
    readonly property color selectionBackground: darkMode ? "#32678d" : "#b9ddf5"
    readonly property color selectionText: darkMode ? "#ffffff" : "#20354a"
    readonly property color switchTop: darkMode ? "#548fb6" : "#75b3da"
    readonly property color switchBottom: darkMode ? "#32688f" : "#498cb8"
    readonly property color switchKnobTop: darkMode ? "#e0edf6" : "#ffffff"
    readonly property color switchKnobBottom: darkMode ? "#b1c8d9" : "#e3edf4"
    readonly property color onAccentText: "#ffffff"
}
