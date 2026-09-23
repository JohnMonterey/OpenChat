import QtQuick
import OpenChat
import OpenChat.Native

// First-run onboarding surface. A standalone full-window Item that switches
// between the Credentials and Recovery views on the controller's step. The
// Credentials view is both the sign-up and the log-in form (controller.mode):
// a username and a password, plus a confirmation and a strength guide when
// creating an account. Aero-styled to match the rest of the app: the same content
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
    // and a muted placeholder. `secret` turns it into a password field with a
    // Show/Hide toggle; `invalid` tints the outline; Enter emits accepted().
    component OnboardingField : Item {
        id: field
        property alias fieldName: fieldInput.objectName
        property alias text: fieldInput.text
        property alias input: fieldInput
        property string placeholder: ""
        property string prefix: ""
        property bool secret: false
        property bool invalid: false
        property bool revealed: false
        property int maximumLength: 256
        signal accepted
        signal focused
        height: 38

        // Never leave a password on screen once the field is cleared (submit and
        // mode switches clear it from the controller side).
        onTextChanged: if (text.length === 0) revealed = false

        Rectangle {
            anchors.fill: parent
            radius: 5
            color: Theme.fieldBackground
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
            anchors.right: revealToggle.visible ? revealToggle.left : parent.right
            anchors.rightMargin: revealToggle.visible ? 6 : 12
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.textPrimary
            font.family: Theme.uiFont
            font.pixelSize: 16
            clip: true
            activeFocusOnTab: true
            selectByMouse: true
            selectionColor: Theme.selectionBackground
            selectedTextColor: Theme.selectionText
            maximumLength: field.maximumLength
            echoMode: field.secret && !field.revealed ? TextInput.Password : TextInput.Normal
            passwordCharacter: "\u2022"
            passwordMaskDelay: 0
            onActiveFocusChanged: if (activeFocus) field.focused()
            // Usernames and passwords are never words: keep keyboards from
            // capitalising, predicting or learning them.
            inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                              | (field.secret ? Qt.ImhSensitiveData : Qt.ImhNone)
            Keys.onReturnPressed: field.accepted()
            Keys.onEnterPressed: field.accepted()

            Text {
                anchors.fill: parent
                visible: !fieldInput.text && !fieldInput.activeFocus
                text: field.placeholder
                color: Theme.placeholderText
                font: fieldInput.font
                verticalAlignment: Text.AlignVCenter
                renderType: Text.NativeRendering
            }
        }

        Text {
            id: revealToggle
            objectName: fieldInput.objectName + "Reveal"
            visible: field.secret && fieldInput.text.length > 0
            anchors.right: parent.right
            anchors.rightMargin: 11
            anchors.verticalCenter: parent.verticalCenter
            text: field.revealed ? "Hide" : "Show"
            color: revealMouse.containsMouse ? Theme.focusBorder : Theme.accentBlue
            font.family: Theme.uiFont
            font.pixelSize: 13
            renderType: Text.NativeRendering

            MouseArea {
                id: revealMouse
                anchors.fill: parent
                anchors.margins: -6
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    field.revealed = !field.revealed;
                    fieldInput.forceActiveFocus();
                }
            }
        }

        // Aero inset: shadow along the inner top/left, a faint highlight along
        // the bottom, then the shared outline drawn last.
        Rectangle { x: 5; y: 1; width: parent.width - 10; height: 1; color: Theme.insetTop }
        Rectangle { x: 1; y: 5; width: 1; height: parent.height - 10; color: Theme.insetLeft }
        Rectangle { x: 5; y: parent.height - 2; width: parent.width - 10; height: 1; color: Theme.gloss }
        Rectangle {
            anchors.fill: parent
            z: 10
            radius: 5
            color: "transparent"
            border.width: 1
            border.color: field.invalid ? Theme.errorText
                          : fieldInput.activeFocus ? Theme.focusBorder : Theme.inputBorder
        }
    }

    // The small line under a field: guidance while typing, tinted as an error.
    // Its height is left implicit (binding a wrapping Text's height to its own
    // implicitHeight is a binding loop); the Column skips it while invisible.
    component FieldHint : Text {
        visible: text.length > 0
        topPadding: 5
        color: Theme.errorText
        wrapMode: Text.WordWrap
        font.family: Theme.uiFont
        font.pixelSize: 12
        renderType: Text.NativeRendering
    }

    component FieldLabel : Text {
        color: Theme.textPrimary
        font.family: Theme.uiFont
        font.pixelSize: 14
        bottomPadding: 7
        renderType: Text.NativeRendering
    }

    // A primary Aero button matching the composer's send button treatment.
    component OnboardingButton : Item {
        id: button
        property string label: ""
        signal clicked
        height: 44
        activeFocusOnTab: true
        Keys.onReturnPressed: if (enabled) clicked()
        Keys.onEnterPressed: if (enabled) clicked()
        Keys.onSpacePressed: if (enabled) clicked()

        Rectangle {
            anchors.fill: parent
            radius: 4
            color: button.enabled ? (buttonMouse.containsMouse ? Theme.buttonHover : Theme.buttonBackground)
                                   : Theme.buttonDisabled
            border.width: 1
            border.color: !button.enabled ? Theme.buttonDisabledBorder
                          : button.activeFocus ? Theme.focusBorder : Theme.buttonBorder
        }
        Text {
            anchors.centerIn: parent
            text: button.label
            color: button.enabled ? Theme.buttonText : Theme.buttonDisabledText
            font.family: Theme.uiFont
            font.pixelSize: 16
            renderType: Text.NativeRendering
        }
        MouseArea {
            id: buttonMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            enabled: button.enabled
            onClicked: button.clicked()
        }
    }

    // Credentials view: sign up for a new account, or log in to an existing one.
    Item {
        id: credentialsView
        objectName: "onboardingCredentialsView"
        anchors.fill: parent
        visible: onboarding.controller.step === OnboardingController.Step.Credentials

        readonly property bool signingUp:
            onboarding.controller.mode === OnboardingController.Mode.SignUp

        onVisibleChanged: if (visible) handleField.input.forceActiveFocus()
        Component.onCompleted: if (visible) handleField.input.forceActiveFocus()

        // Scrolls just far enough that a newly focused control, plus the hint
        // under it, is on screen. Only matters when the form is taller than the
        // window; tabbing into a control below the fold would otherwise be blind.
        function reveal(item) {
            const top = item.mapToItem(form, 0, 0).y + form.y;
            const bottom = top + item.height + 34;
            if (top - 12 < formScroll.contentY)
                formScroll.contentY = Math.max(0, top - 12);
            else if (bottom > formScroll.contentY + formScroll.height)
                formScroll.contentY = Math.min(formScroll.contentHeight - formScroll.height,
                                               bottom - formScroll.height);
        }

        // Centred while it fits; scrolls instead of clipping at the minimum
        // window height, where the sign-up form with every hint showing is taller
        // than the window.
        Flickable {
            id: formScroll
            anchors.fill: parent
            contentWidth: width
            contentHeight: Math.max(height, form.height + 56)
            boundsBehavior: Flickable.StopAtBounds
            clip: true

            Column {
                id: form
                width: 420
                x: Math.round((formScroll.width - width) / 2)
                y: Math.round(Math.max(28, (formScroll.contentHeight - height) / 2))
                spacing: 0

                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 12

                    Image {
                        width: 46
                        height: 46
                        anchors.verticalCenter: parent.verticalCenter
                        source: Qt.resolvedUrl("../../assets/icons/openchat-256.png")
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

                Item { width: 1; height: 24 }

                Text {
                    objectName: "onboardingTitle"
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: credentialsView.signingUp ? "Create your account" : "Welcome back"
                    color: Theme.textPrimary
                    font.family: Theme.uiFont
                    font.pixelSize: 23
                    renderType: Text.NativeRendering
                }

                Item { width: 1; height: 6 }

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: credentialsView.signingUp
                          ? "Pick a username and a password to get started."
                          : "Log in with your username and password."
                    color: Theme.textSecondary
                    font.family: Theme.uiFont
                    font.pixelSize: 14
                    renderType: Text.NativeRendering
                }

                Item { width: 1; height: 22 }

                // Shown when startup had something to tell the user, such as
                // outdated local data having been erased.
                Rectangle {
                    objectName: "onboardingNotice"
                    visible: onboarding.controller.notice.length > 0
                    width: parent.width
                    height: visible ? noticeText.implicitHeight + 20 : 0
                    radius: 5
                    color: Theme.warningBackground
                    border.width: 1
                    border.color: Theme.warningBorder

                    Text {
                        id: noticeText
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.margins: 11
                        anchors.verticalCenter: parent.verticalCenter
                        text: onboarding.controller.notice
                        color: Theme.warningText
                        wrapMode: Text.WordWrap
                        font.family: Theme.uiFont
                        font.pixelSize: 13
                        renderType: Text.NativeRendering
                    }
                }

                Item { width: 1; height: onboarding.controller.notice.length > 0 ? 18 : 0 }

                FieldLabel { text: "Username" }

                OnboardingField {
                    id: handleField
                    width: parent.width
                    fieldName: "handleField"
                    prefix: "@"
                    placeholder: "username"
                    maximumLength: 40
                    enabled: !onboarding.controller.busy
                    invalid: onboarding.controller.handleHint.length > 0
                    text: onboarding.controller.handle
                    onTextChanged: {
                        if (text !== onboarding.controller.handle)
                            onboarding.controller.setHandle(text);
                    }
                    onAccepted: passwordField.input.forceActiveFocus()
                    onFocused: credentialsView.reveal(handleField)
                }

                FieldHint {
                    objectName: "handleHint"
                    width: parent.width
                    text: onboarding.controller.handleHint
                }

                Text {
                    visible: credentialsView.signingUp
                             && onboarding.controller.handleHint.length === 0
                    width: parent.width
                    topPadding: 5
                    text: "Others can add you by @username. It can't be changed later."
                    color: Theme.textSecondary
                    wrapMode: Text.WordWrap
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                    renderType: Text.NativeRendering
                }

                Item { width: 1; height: 16 }

                FieldLabel { text: "Password" }

                OnboardingField {
                    id: passwordField
                    width: parent.width
                    fieldName: "passwordField"
                    secret: true
                    placeholder: credentialsView.signingUp ? "At least 10 characters" : "Password"
                    enabled: !onboarding.controller.busy
                    invalid: onboarding.controller.passwordHint.length > 0
                    text: onboarding.controller.password
                    onTextChanged: {
                        if (text !== onboarding.controller.password)
                            onboarding.controller.setPassword(text);
                    }
                    onAccepted: {
                        if (credentialsView.signingUp)
                            passwordConfirmField.input.forceActiveFocus();
                        else
                            onboarding.controller.submit();
                    }
                    onFocused: credentialsView.reveal(passwordField)
                }

                // Strength guide: four segments and a word, for new passwords only.
                Item {
                    objectName: "passwordStrengthMeter"
                    visible: credentialsView.signingUp
                             && onboarding.controller.passwordStrength > 0
                    width: parent.width
                    height: visible ? 19 : 0

                    readonly property int level: onboarding.controller.passwordStrength
                    readonly property color levelColor: level <= 1 ? Theme.errorText
                                                        : level === 2 ? Theme.warningText
                                                        : Theme.successText

                    Row {
                        id: strengthBars
                        anchors.left: parent.left
                        anchors.bottom: parent.bottom
                        anchors.bottomMargin: 4
                        spacing: 4

                        Repeater {
                            model: 4
                            Rectangle {
                                required property int index
                                width: 54
                                height: 4
                                radius: 2
                                color: index < strengthBars.parent.level
                                       ? strengthBars.parent.levelColor : Theme.softRule
                            }
                        }
                    }
                    Text {
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        text: ["", "Too weak", "Fair", "Good", "Strong"][parent.level]
                        color: parent.levelColor
                        font.family: Theme.uiFont
                        font.pixelSize: 12
                        renderType: Text.NativeRendering
                    }
                }

                FieldHint {
                    objectName: "passwordHint"
                    width: parent.width
                    text: onboarding.controller.passwordHint
                }

                Item { width: 1; height: credentialsView.signingUp ? 16 : 0 }

                FieldLabel {
                    visible: credentialsView.signingUp
                    text: "Confirm password"
                }

                OnboardingField {
                    id: passwordConfirmField
                    visible: credentialsView.signingUp
                    height: visible ? 38 : 0
                    width: parent.width
                    fieldName: "passwordConfirmField"
                    secret: true
                    placeholder: "Type it again"
                    enabled: !onboarding.controller.busy
                    invalid: onboarding.controller.passwordConfirmHint.length > 0
                    text: onboarding.controller.passwordConfirm
                    onTextChanged: {
                        if (text !== onboarding.controller.passwordConfirm)
                            onboarding.controller.setPasswordConfirm(text);
                    }
                    onAccepted: onboarding.controller.submit()
                    onFocused: credentialsView.reveal(passwordConfirmField)
                }

                FieldHint {
                    objectName: "passwordConfirmHint"
                    width: parent.width
                    text: onboarding.controller.passwordConfirmHint
                }

                Item { width: 1; height: 20 }

                Text {
                    objectName: "onboardingError"
                    width: parent.width
                    visible: onboarding.controller.errorText.length > 0
                    bottomPadding: 14
                    text: onboarding.controller.errorText
                    color: Theme.errorText
                    wrapMode: Text.WordWrap
                    font.family: Theme.uiFont
                    font.pixelSize: 13
                    renderType: Text.NativeRendering
                }

                OnboardingButton {
                    id: submitButton
                    objectName: "submitButton"
                    width: parent.width
                    onActiveFocusChanged: if (activeFocus) credentialsView.reveal(submitButton)
                    // While the flow is in flight the button says so and is
                    // disabled, so it cannot be re-triggered.
                    label: credentialsView.signingUp
                           ? (onboarding.controller.busy ? "Creating your account…" : "Create account")
                           : (onboarding.controller.busy ? "Logging in…" : "Log in")
                    enabled: onboarding.controller.canSubmit
                    onClicked: onboarding.controller.submit()
                }

                Item { width: 1; height: 16 }

                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 5

                    Text {
                        text: credentialsView.signingUp ? "Already have an account?"
                                                        : "New to OpenChat?"
                        color: Theme.textSecondary
                        font.family: Theme.uiFont
                        font.pixelSize: 13
                        renderType: Text.NativeRendering
                    }
                    Text {
                        id: modeSwitch
                        objectName: "modeSwitch"
                        signal clicked
                        activeFocusOnTab: true
                        text: credentialsView.signingUp ? "Log in" : "Create an account"
                        color: onboarding.controller.busy ? Theme.buttonDisabledText
                               : (modeMouse.containsMouse || activeFocus) ? Theme.focusBorder
                               : Theme.accentBlue
                        font.family: Theme.uiFont
                        font.pixelSize: 13
                        font.underline: modeMouse.containsMouse || activeFocus
                        renderType: Text.NativeRendering
                        onClicked: {
                            onboarding.controller.setMode(
                                credentialsView.signingUp ? OnboardingController.Mode.LogIn
                                                          : OnboardingController.Mode.SignUp);
                            handleField.input.forceActiveFocus();
                        }
                        Keys.onReturnPressed: clicked()
                        Keys.onEnterPressed: clicked()
                        Keys.onSpacePressed: clicked()

                        MouseArea {
                            id: modeMouse
                            anchors.fill: parent
                            anchors.margins: -5
                            hoverEnabled: true
                            enabled: !onboarding.controller.busy
                            cursorShape: Qt.PointingHandCursor
                            onClicked: modeSwitch.clicked()
                        }
                    }
                }

                Item { width: 1; height: 18 }

                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: "Your password never leaves this device. Only a key derived "
                          + "from it is sent, encrypted, to sign you in."
                    color: Theme.textSecondary
                    wrapMode: Text.WordWrap
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                    renderType: Text.NativeRendering
                }
            }
        }

        // A slim position indicator, present only while the form overflows.
        Rectangle {
            visible: formScroll.contentHeight > formScroll.height + 1
            anchors.right: parent.right
            anchors.rightMargin: 4
            width: 5
            radius: 2.5
            color: Theme.inputBorder
            y: formScroll.visibleArea.yPosition * formScroll.height + 4
            height: Math.max(24, formScroll.visibleArea.heightRatio * formScroll.height - 8)
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
                    color: Theme.panelBackground
                    border.width: 1
                    border.color: Theme.inputBorder
                }
                Rectangle {
                    x: 6; y: 1; width: parent.width - 12; height: 1; color: Theme.insetTop
                }
                Rectangle {
                    x: 6; y: parent.height - 2; width: parent.width - 12; height: 1
                    color: Theme.gloss
                }

                Text {
                    id: recoveryCodeText
                    objectName: "recoveryCodeText"
                    anchors.centerIn: parent
                    width: parent.width - 24
                    horizontalAlignment: Text.AlignHCenter
                    text: onboarding.controller.recoveryCode
                    color: Theme.textPrimary
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
