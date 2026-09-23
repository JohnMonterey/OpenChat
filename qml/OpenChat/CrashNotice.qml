import QtQuick
import QtQuick.Window
import OpenChat

// What a tester sees the moment OpenChat has crashed, or on the next launch
// after it closed without a trace: what happened in plain words, what OpenChat
// was doing at the time, and the full report to copy and send. It is its own
// window, independent of the chat and of onboarding, so it can be shown by a
// freshly relaunched process that has nothing else on screen.
Window {
    id: crashNotice
    objectName: "crashNotice"
    required property var report
    property bool showDetails: false
    readonly property string monospaceFamily: Qt.platform.os === "windows" ? "Consolas"
                                            : (Qt.platform.os === "osx" ? "Menlo" : "monospace")

    width: 660
    height: showDetails ? 620 : 420
    minimumWidth: 480
    minimumHeight: 360
    visible: true
    title: report.headline
    color: Theme.contentBackground

    Connections {
        target: crashNotice.report
        function onFinished() { crashNotice.close() }
    }

    Column {
        id: summary
        objectName: "crashSummary"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 22
        spacing: 12

        Text {
            objectName: "crashHeadline"
            width: parent.width
            text: crashNotice.report.headline
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 21
            font.bold: true
            wrapMode: Text.WordWrap
        }

        Text {
            objectName: "crashWhatHappened"
            width: parent.width
            text: crashNotice.report.whatHappened
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 14
            wrapMode: Text.WordWrap
        }

        // The three facts that make a report obvious, each only when known.
        Grid {
            width: parent.width
            columns: 2
            columnSpacing: 10
            rowSpacing: 6

            Text {
                visible: crashNotice.report.doing.length > 0
                text: "While"
                color: Theme.textSecondary
                font.family: Theme.uiFont
                font.pixelSize: 13
                font.bold: true
            }
            Text {
                objectName: "crashDoing"
                visible: crashNotice.report.doing.length > 0
                width: summary.width - 70
                text: crashNotice.report.doing
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 13
                wrapMode: Text.WordWrap
            }
            Text {
                visible: crashNotice.report.where.length > 0
                text: "Where"
                color: Theme.textSecondary
                font.family: Theme.uiFont
                font.pixelSize: 13
                font.bold: true
            }
            Text {
                objectName: "crashWhere"
                visible: crashNotice.report.where.length > 0
                width: summary.width - 70
                text: crashNotice.report.where
                color: Theme.textPrimary
                font.family: crashNotice.monospaceFamily
                font.pixelSize: 12
                wrapMode: Text.WrapAnywhere
            }
            Text {
                visible: crashNotice.report.hint.length > 0
                text: "Hint"
                color: Theme.textSecondary
                font.family: Theme.uiFont
                font.pixelSize: 13
                font.bold: true
            }
            Text {
                objectName: "crashHint"
                visible: crashNotice.report.hint.length > 0
                width: summary.width - 70
                text: crashNotice.report.hint
                color: Theme.declineBottom
                font.family: Theme.uiFont
                font.pixelSize: 13
                wrapMode: Text.WordWrap
            }
        }

        TextEdit {
            objectName: "crashReportPath"
            width: parent.width
            readOnly: true
            selectByMouse: true
            text: "The full report is saved at " + crashNotice.report.reportPath
                  + ". Please send it to whoever gave you this build of OpenChat."
            color: Theme.textSecondary
            font.family: Theme.uiFont
            font.pixelSize: 12
            wrapMode: TextEdit.Wrap
        }

        // Wraps rather than running off a narrow window.
        Flow {
            width: parent.width
            spacing: 8

            AeroButton {
                objectName: "crashCopyButton"
                width: 118
                height: 34
                fontPixelSize: 13
                label: crashNotice.report.copied ? "Copied" : "Copy report"
                onClicked: crashNotice.report.copyReport()
            }
            AeroButton {
                objectName: "crashFolderButton"
                width: 110
                height: 34
                fontPixelSize: 13
                label: "Open folder"
                onClicked: crashNotice.report.openReportFolder()
            }
            AeroButton {
                objectName: "crashDetailsButton"
                width: 118
                height: 34
                fontPixelSize: 13
                label: crashNotice.showDetails ? "Hide details" : "Show details"
                onClicked: crashNotice.showDetails = !crashNotice.showDetails
            }
            AeroButton {
                objectName: "crashRestartButton"
                visible: crashNotice.report.canRestart
                width: 140
                height: 34
                fontPixelSize: 13
                label: "Restart OpenChat"
                onClicked: crashNotice.report.restartOpenChat()
            }
            AeroButton {
                objectName: "crashCloseButton"
                width: 72
                height: 34
                fontPixelSize: 13
                label: "Close"
                onClicked: crashNotice.report.dismiss()
            }
        }
    }

    // The report itself, exactly as saved, for whoever wants the stack.
    Rectangle {
        id: details
        objectName: "crashDetails"
        visible: crashNotice.showDetails
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: summary.bottom
        anchors.bottom: parent.bottom
        anchors.margins: 22
        anchors.topMargin: 14
        color: Theme.fieldBackground
        border.width: 1
        border.color: Theme.inputBorder
        radius: 3
        clip: true

        Flickable {
            id: reportScroll
            anchors.fill: parent
            anchors.margins: 8
            contentWidth: reportText.paintedWidth
            contentHeight: reportText.paintedHeight
            boundsBehavior: Flickable.StopAtBounds

            TextEdit {
                id: reportText
                objectName: "crashReportText"
                readOnly: true
                selectByMouse: true
                text: crashNotice.report.reportText
                color: Theme.textPrimary
                font.family: crashNotice.monospaceFamily
                font.pixelSize: 11
            }
        }
    }

    onClosing: crashNotice.report.dismiss()
}
