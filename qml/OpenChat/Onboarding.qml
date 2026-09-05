import QtQuick
import OpenChat
import OpenChat.Native

// First-run onboarding surface. A standalone full-window Item (not wired into
// Main.qml yet) that switches between the Create and Recovery views on the
// controller's step. Aero-styled to match the rest of the app: the same content
// gradient, input frames, and button treatment used by the composer and sidebar.
Item {
    id: onboarding
    objectName: "onboardingRoot"
    required property var controller

    implicitWidth: 860
    implicitHeight: 680

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.contentBackground }
            GradientStop { position: 1; color: Theme.contentBottom }
        }
    }

    // A reusable Aero text frame: rounded white field with the shared inset
    // highlight and outline used by the composer, carrying a single-line input
    // and a muted placeholder.
    component OnboardingField : Item {
        id: field
        property alias fieldName: fieldInput.objectName
        property alias text: fieldInput.text
        property string placeholder: ""
        property string prefix: ""
        height: 38

        Rectangle {
            anchors.fill: parent
            radius: 5
            color: "#ffffff"
        }

        Text {
            id: prefixLabel
            visible: field.prefix.length > 0
            anchors.left: parent.left
            anchors.leftMargin: 11
            anchors.verticalCenter: parent.verticalCenter
            text: field.prefix
            color: Theme.textSecondary
            font.family: Theme.uiFont
            font.pixelSize: 16
            renderType: Text.NativeRendering
        }

        TextInput {
            id: fieldInput
            anchors.left: prefixLabel.visible ? prefixLabel.right : parent.left
            anchors.leftMargin: prefixLabel.visible ? 1 : 12
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 16
            clip: true
            selectByMouse: true

            Text {
                anchors.fill: parent
                visible: !fieldInput.text && !fieldInput.activeFocus
                text: field.placeholder
                color: "#98a7ba"
                font: fieldInput.font
                verticalAlignment: Text.AlignVCenter
                renderType: Text.NativeRendering
            }
        }

        // Aero inset: shadow along the inner top/left, a faint highlight along
        // the bottom, then the shared outline drawn last.
        Rectangle { x: 5; y: 1; width: parent.width - 10; height: 1; color: "#24526878" }
        Rectangle { x: 1; y: 5; width: 1; height: parent.height - 10; color: "#18526878" }
        Rectangle { x: 5; y: parent.height - 2; width: parent.width - 10; height: 1; color: "#70ffffff" }
        Rectangle {
            anchors.fill: parent
            z: 10
            radius: 5
            color: "transparent"
            border.width: 1
            border.color: Theme.inputBorder
        }
    }

    // A primary Aero button matching the composer's send button treatment.
    component OnboardingButton : Item {
        id: button
        property string label: ""
        signal clicked
        height: 44

        Rectangle {
            anchors.fill: parent
            radius: 4
            color: button.enabled ? (buttonMouse.containsMouse ? "#f8fbfd" : "#f5f8fa")
                                   : "#eef2f5"
            border.width: 1
            border.color: button.enabled ? "#aebdca" : "#c5d0d9"
        }
        Text {
            anchors.centerIn: parent
            text: button.label
            color: button.enabled ? "#3f5570" : "#8b99aa"
            font.family: Theme.uiFont
            font.pixelSize: 16
            renderType: Text.NativeRendering
        }
        MouseArea {
            id: buttonMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: button.clicked()
        }
    }

    // Create view: profile fields gathered on first run.
    Item {
        id: createView
        objectName: "onboardingCreateView"
        anchors.fill: parent
        visible: onboarding.controller.step === OnboardingController.Step.Create

        Column {
            anchors.centerIn: parent
            width: 420
            spacing: 0

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 12

                Image {
                    width: 46
                    height: 46
                    anchors.verticalCenter: parent.verticalCenter
                    source: Qt.resolvedUrl("../../assets/icons/openchat.svg")
                    sourceSize: Qt.size(width * 2, height * 2)
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "OpenChat"
                    color: Theme.textPrimary
                    font.family: Theme.uiFont
                    font.pixelSize: 30
                    renderType: Text.NativeRendering
                }
            }

            Item { width: 1; height: 26 }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Create your profile"
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 23
                renderType: Text.NativeRendering
            }

            Item { width: 1; height: 6 }

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Set up how you'll appear to your contacts."
                color: Theme.textSecondary
                font.family: Theme.uiFont
                font.pixelSize: 14
                renderType: Text.NativeRendering
            }

            Item { width: 1; height: 30 }

            Text {
                width: parent.width
                text: "Display name"
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 14
                renderType: Text.NativeRendering
            }

            Item { width: 1; height: 7 }

            OnboardingField {
                width: parent.width
                fieldName: "displayNameField"
                placeholder: "Your name"
                text: onboarding.controller.displayName
                onTextChanged: {
                    if (text !== onboarding.controller.displayName)
                        onboarding.controller.setDisplayName(text);
                }
            }

            Item { width: 1; height: 18 }

            Text {
                width: parent.width
                text: "Handle"
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 14
                renderType: Text.NativeRendering
            }

            Item { width: 1; height: 7 }

            OnboardingField {
                width: parent.width
                fieldName: "handleField"
                prefix: "@"
                placeholder: "handle"
                text: onboarding.controller.handle
                onTextChanged: {
                    if (text !== onboarding.controller.handle)
                        onboarding.controller.setHandle(text);
                }
            }

            Item { width: 1; height: 7 }

            Text {
                width: parent.width
                text: "Others can add you by @handle."
                color: Theme.textSecondary
                font.family: Theme.uiFont
                font.pixelSize: 13
                renderType: Text.NativeRendering
            }

            Item { width: 1; height: 22 }

            Text {
                objectName: "onboardingCreateError"
                width: parent.width
                visible: onboarding.controller.errorText.length > 0
                text: onboarding.controller.errorText
                color: "#c0392b"
                wrapMode: Text.WordWrap
                font.family: Theme.uiFont
                font.pixelSize: 13
                renderType: Text.NativeRendering
            }

            Item {
                width: 1
                height: onboarding.controller.errorText.length > 0 ? 14 : 0
            }

            OnboardingButton {
                objectName: "createButton"
                width: parent.width
                // While a bootstrap is in flight the button reads "Creating…" and
                // is disabled, so the account creation cannot be re-triggered.
                label: onboarding.controller.creating ? "Creating…" : "Create"
                enabled: onboarding.controller.canCreate && !onboarding.controller.creating
                onClicked: onboarding.controller.createProfile()
            }
        }
    }

    // Recovery view: the one-time recovery code, shown after a successful create.
    Item {
        id: recoveryView
        objectName: "onboardingRecoveryView"
        anchors.fill: parent
        visible: onboarding.controller.step === OnboardingController.Step.Recovery

        Column {
            anchors.centerIn: parent
            width: 460
            spacing: 0

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Save your recovery code"
                color: Theme.textPrimary
                font.family: Theme.uiFont
                font.pixelSize: 23
                renderType: Text.NativeRendering
            }

            Item { width: 1; height: 8 }

            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: "This is shown once. Store it somewhere safe — it's the only way to "
                      + "recover your account."
                color: Theme.textSecondary
                wrapMode: Text.WordWrap
                font.family: Theme.uiFont
                font.pixelSize: 14
                renderType: Text.NativeRendering
            }

            Item { width: 1; height: 26 }

            // Prominent monospaced code box.
            Item {
                width: parent.width
                height: 66

                Rectangle {
                    anchors.fill: parent
                    radius: 6
                    color: "#eef6fb"
                    border.width: 1
                    border.color: Theme.inputBorder
                }
                Rectangle {
                    x: 6; y: 1; width: parent.width - 12; height: 1; color: "#24526878"
                }
                Rectangle {
                    x: 6; y: parent.height - 2; width: parent.width - 12; height: 1
                    color: "#70ffffff"
                }

                Text {
                    id: recoveryCodeText
                    objectName: "recoveryCodeText"
                    anchors.centerIn: parent
                    width: parent.width - 24
                    horizontalAlignment: Text.AlignHCenter
                    text: onboarding.controller.recoveryCode
                    color: "#2b3b53"
                    font.family: "Courier New"
                    font.pixelSize: 19
                    font.letterSpacing: 1
                    wrapMode: Text.WrapAnywhere
                    renderType: Text.NativeRendering
                }
            }

            Item { width: 1; height: 26 }

            OnboardingButton {
                objectName: "savedButton"
                width: parent.width
                label: "I've saved it"
                onClicked: onboarding.controller.confirmRecoverySaved()
            }
        }
    }
}
