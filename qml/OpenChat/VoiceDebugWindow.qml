import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

Window {
    id: debugWindow
    objectName: "voiceDebugWindow"
    title: "[VOICE DEBUG OVERLAY] OpenChat Latency, Jitter & Transport Diagnostics"
    width: 980
    height: 760
    minimumWidth: 840
    minimumHeight: 640
    color: "#080b11"

    required property var debugController

    FontLoader {
        id: monoFont
    }

    readonly property string monoFamily: "Consolas, 'JetBrains Mono', 'DejaVu Sans Mono', 'Courier New', monospace"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 10

        // =========================================================================
        // TOP HEADER / COCKPIT STATUS BAR
        // =========================================================================
        Rectangle {
            Layout.fillWidth: true
            height: 54
            color: "#0f1622"
            radius: 6
            border.color: "#1e2d42"
            border.width: 1

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                spacing: 10

                // Title label
                RowLayout {
                    spacing: 6
                    Rectangle {
                        width: 8
                        height: 8
                        radius: 4
                        color: debugController.isSpikeActive ? "#ef4444" : "#22c55e"
                        SequentialAnimation on opacity {
                            loops: Animation.Infinite
                            running: debugController.isSpikeActive
                            NumberAnimation { from: 1.0; to: 0.2; duration: 250 }
                            NumberAnimation { from: 0.2; to: 1.0; duration: 250 }
                        }
                    }
                    Text {
                        text: "VOICE TELEMETRY HUD"
                        font.family: monoFamily
                        font.bold: true
                        font.pixelSize: 13
                        color: "#38bdf8"
                    }
                }

                Rectangle { width: 1; height: 24; color: "#1e2d42" }

                // Carrier pill
                Rectangle {
                    height: 26
                    implicitWidth: carrierText.implicitWidth + 18
                    radius: 4
                    color: debugController.peerUdpState === "Active" ? "#064e3b" : "#451a03"
                    border.color: debugController.peerUdpState === "Active" ? "#10b981" : "#f59e0b"
                    border.width: 1

                    Text {
                        id: carrierText
                        anchors.centerIn: parent
                        text: "CARRIER: " + debugController.carrierMode.toUpperCase()
                        font.family: monoFamily
                        font.bold: true
                        font.pixelSize: 11
                        color: debugController.peerUdpState === "Active" ? "#34d399" : "#fbbf24"
                    }
                }

                // Call status pill
                Rectangle {
                    height: 26
                    implicitWidth: callStatusText.implicitWidth + 18
                    radius: 4
                    color: debugController.hasCall ? "#172554" : "#1f2937"
                    border.color: debugController.hasCall ? "#3b82f6" : "#4b5563"
                    border.width: 1

                    Text {
                        id: callStatusText
                        anchors.centerIn: parent
                        text: "CALL: " + debugController.callState.toUpperCase() + " (" + debugController.callDuration + ")"
                        font.family: monoFamily
                        font.bold: true
                        font.pixelSize: 11
                        color: debugController.hasCall ? "#60a5fa" : "#9ca3af"
                    }
                }

                // Relay endpoint pill
                Rectangle {
                    height: 26
                    implicitWidth: endpointText.implicitWidth + 16
                    radius: 4
                    color: "#111827"
                    border.color: "#374151"
                    border.width: 1

                    Text {
                        id: endpointText
                        anchors.centerIn: parent
                        text: "RELAY: " + debugController.relayEndpoint + " | LOCAL :" + debugController.localPort
                        font.family: monoFamily
                        font.pixelSize: 10
                        color: "#9ca3af"
                    }
                }

                Item { Layout.fillWidth: true }

                // Transport Mode Selector
                RowLayout {
                    spacing: 4
                    Text {
                        text: "MODE:"
                        font.family: monoFamily
                        font.pixelSize: 10
                        color: "#6b7280"
                    }

                    Repeater {
                        model: ["Auto", "Udp", "Tcp"]
                        Rectangle {
                            height: 24
                            implicitWidth: modeLabel.implicitWidth + 12
                            radius: 3
                            color: debugController.transportSetting.toLowerCase() === modelData.toLowerCase()
                                   ? "#2563eb" : "#1e293b"
                            border.color: debugController.transportSetting.toLowerCase() === modelData.toLowerCase()
                                          ? "#60a5fa" : "#334155"

                            Text {
                                id: modeLabel
                                anchors.centerIn: parent
                                text: modelData.toUpperCase()
                                font.family: monoFamily
                                font.bold: true
                                font.pixelSize: 10
                                color: debugController.transportSetting.toLowerCase() === modelData.toLowerCase()
                                       ? "#ffffff" : "#94a3b8"
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: debugController.setTransportMode(modelData)
                            }
                        }
                    }
                }

                Rectangle { width: 1; height: 24; color: "#1e2d42" }

                // Actions buttons
                RowLayout {
                    spacing: 6

                    // Ping Now
                    Rectangle {
                        height: 24
                        implicitWidth: pingLabel.implicitWidth + 14
                        radius: 3
                        color: "#065f46"
                        border.color: "#10b981"
                        Text {
                            id: pingLabel
                            anchors.centerIn: parent
                            text: "PING NOW"
                            font.family: monoFamily
                            font.bold: true
                            font.pixelSize: 10
                            color: "#a7f3d0"
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: debugController.pingNow()
                        }
                    }

                    // Simulate Spike
                    Rectangle {
                        height: 24
                        implicitWidth: spikeLabel.implicitWidth + 14
                        radius: 3
                        color: "#7f1d1d"
                        border.color: "#ef4444"
                        Text {
                            id: spikeLabel
                            anchors.centerIn: parent
                            text: "TEST SPIKE"
                            font.family: monoFamily
                            font.bold: true
                            font.pixelSize: 10
                            color: "#fecaca"
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: debugController.simulateSpike(185.0)
                        }
                    }

                    // Clear logs
                    Rectangle {
                        height: 24
                        implicitWidth: clearLabel.implicitWidth + 12
                        radius: 3
                        color: "#1e293b"
                        border.color: "#334155"
                        Text {
                            id: clearLabel
                            anchors.centerIn: parent
                            text: "CLEAR"
                            font.family: monoFamily
                            font.pixelSize: 10
                            color: "#94a3b8"
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: debugController.clearLogs()
                        }
                    }
                }
            }
        }

        // =========================================================================
        // LAG SPIKE PINPOINTER & REALTIME LATENCY WAVEFORM
        // =========================================================================
        Rectangle {
            Layout.fillWidth: true
            height: 180
            color: "#0d131c"
            radius: 6
            border.color: debugController.isSpikeActive ? "#ef4444" : "#1e2d42"
            border.width: debugController.isSpikeActive ? 2 : 1

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 8

                // Header & Primary Metrics Bar
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 16

                    Text {
                        text: "LATENCY & LAG SPIKE PINPOINTER"
                        font.family: monoFamily
                        font.bold: true
                        font.pixelSize: 11
                        color: "#94a3b8"
                    }

                    Item { Layout.fillWidth: true }

                    // Current RTT Big Metric
                    RowLayout {
                        spacing: 4
                        Text {
                            text: "CURRENT:"
                            font.family: monoFamily
                            font.pixelSize: 10
                            color: "#64748b"
                        }
                        Text {
                            text: debugController.currentRtt.toFixed(1) + " ms"
                            font.family: monoFamily
                            font.bold: true
                            font.pixelSize: 15
                            color: debugController.isSpikeActive ? "#ef4444"
                                   : debugController.currentRtt > 80.0 ? "#f59e0b" : "#38bdf8"
                        }
                        Text {
                            visible: debugController.isSpikeActive
                            text: "[⚠️ LAG SPIKE!]"
                            font.family: monoFamily
                            font.bold: true
                            font.pixelSize: 11
                            color: "#ef4444"
                        }
                    }

                    // Average EMA
                    RowLayout {
                        spacing: 4
                        Text {
                            text: "AVG (EMA):"
                            font.family: monoFamily
                            font.pixelSize: 10
                            color: "#64748b"
                        }
                        Text {
                            text: debugController.avgRtt.toFixed(1) + " ms"
                            font.family: monoFamily
                            font.bold: true
                            font.pixelSize: 12
                            color: "#e2e8f0"
                        }
                    }

                    // Min / Max
                    RowLayout {
                        spacing: 4
                        Text {
                            text: "MIN/MAX:"
                            font.family: monoFamily
                            font.pixelSize: 10
                            color: "#64748b"
                        }
                        Text {
                            text: debugController.minRtt.toFixed(1) + " / " + debugController.maxRtt.toFixed(1) + " ms"
                            font.family: monoFamily
                            font.bold: true
                            font.pixelSize: 12
                            color: "#e2e8f0"
                        }
                    }

                    // Spikes Count Badge
                    Rectangle {
                        height: 20
                        implicitWidth: spikeCountText.implicitWidth + 12
                        radius: 3
                        color: debugController.lagSpikeCount > 0 ? "#7f1d1d" : "#064e3b"
                        border.color: debugController.lagSpikeCount > 0 ? "#ef4444" : "#10b981"
                        Text {
                            id: spikeCountText
                            anchors.centerIn: parent
                            text: "SPIKES DETECTED: " + debugController.lagSpikeCount
                            font.family: monoFamily
                            font.bold: true
                            font.pixelSize: 10
                            color: debugController.lagSpikeCount > 0 ? "#fca5a5" : "#6ee7b7"
                        }
                    }

                    // Jitter Spread
                    RowLayout {
                        spacing: 4
                        Text {
                            text: "JITTER:"
                            font.family: monoFamily
                            font.pixelSize: 10
                            color: "#64748b"
                        }
                        Text {
                            text: debugController.jitterMs + " ms"
                            font.family: monoFamily
                            font.bold: true
                            font.pixelSize: 12
                            color: debugController.jitterMs > 20 ? "#f59e0b" : "#38bdf8"
                        }
                    }
                }

                // Waveform Chart Area
                Rectangle {
                    id: chartContainer
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    color: "#070a0f"
                    radius: 4
                    clip: true
                    border.color: "#16202e"
                    border.width: 1

                    // Grid lines (50ms, 100ms, 150ms)
                    Repeater {
                        model: [50, 100, 150]
                        Item {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            property real maxScale: Math.max(160.0, debugController.maxRtt * 1.1)
                            y: chartContainer.height - (modelData / maxScale) * chartContainer.height

                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                height: 1
                                color: modelData === 100 ? "#3b1e2b" : "#152030"
                            }
                            Text {
                                text: modelData + "ms"
                                font.family: monoFamily
                                font.pixelSize: 8
                                color: modelData === 100 ? "#f43f5e" : "#475569"
                                anchors.right: parent.right
                                anchors.rightMargin: 4
                                anchors.bottom: parent.top
                            }
                        }
                    }

                    // Avg RTT Guideline
                    Rectangle {
                        property real maxScale: Math.max(160.0, debugController.maxRtt * 1.1)
                        visible: debugController.avgRtt > 0
                        anchors.left: parent.left
                        anchors.right: parent.right
                        height: 1
                        y: chartContainer.height - (debugController.avgRtt / maxScale) * chartContainer.height
                        color: "#0284c7"
                        opacity: 0.8
                    }

                    // Waveform Bars
                    Row {
                        id: barsRow
                        anchors.fill: parent
                        anchors.margins: 4
                        spacing: 2

                        readonly property real maxScale: Math.max(160.0, debugController.maxRtt * 1.1)
                        readonly property var history: debugController.latencyHistory
                        readonly property int count: history.length
                        readonly property real barW: count > 0
                            ? Math.max(3.0, (barsRow.width - (count - 1) * barsRow.spacing) / count)
                            : 6.0

                        Repeater {
                            model: debugController.latencyHistory
                            Rectangle {
                                width: barsRow.barW
                                property real rttVal: modelData.rtt || 0.0
                                property bool isSpike: modelData.isSpike || (rttVal > 100.0)
                                height: Math.max(4.0, (rttVal / barsRow.maxScale) * (barsRow.height - 4))
                                anchors.bottom: parent.bottom
                                radius: 1

                                color: isSpike ? "#ef4444"
                                       : (rttVal > 80.0 ? "#f59e0b" : "#0ea5e9")

                                // Spike glow or marker
                                Rectangle {
                                    visible: isSpike
                                    width: parent.width + 2
                                    height: 4
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    anchors.bottom: parent.top
                                    anchors.bottomMargin: 1
                                    color: "#f87171"
                                }

                                ToolTip.visible: barMouseArea.containsMouse
                                ToolTip.text: "RTT: " + rttVal.toFixed(1) + " ms\nEMA: "
                                              + (modelData.ema ? modelData.ema.toFixed(1) : "--")
                                              + " ms\nTime: " + (modelData.time || "")
                                              + (isSpike ? "\n⚠️ LAG SPIKE" : "")

                                MouseArea {
                                    id: barMouseArea
                                    anchors.fill: parent
                                    hoverEnabled: true
                                }
                            }
                        }
                    }
                }
            }
        }

        // =========================================================================
        // 4 COMPREHENSIVE TELEMETRY CARDS (GRID)
        // =========================================================================
        GridLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 250
            columns: 4
            rowSpacing: 8
            columnSpacing: 8

            // ---------------------------------------------------------------------
            // CARD 1: CARRIER & UDP PROTOCOL
            // ---------------------------------------------------------------------
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "#0d131c"
                radius: 6
                border.color: "#1e2d42"
                border.width: 1

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 6

                    Text {
                        text: "1. CARRIER & TRANSPORT"
                        font.family: monoFamily
                        font.bold: true
                        font.pixelSize: 11
                        color: "#38bdf8"
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: "#16202e" }

                    RowLayout {
                        Text { text: "Active Route:"; font.family: monoFamily; font.pixelSize: 10; color: "#64748b" }
                        Item { Layout.fillWidth: true }
                        Text { text: debugController.carrierMode; font.family: monoFamily; font.bold: true; font.pixelSize: 10; color: "#f1f5f9" }
                    }

                    RowLayout {
                        Text { text: "Peer UDP State:"; font.family: monoFamily; font.pixelSize: 10; color: "#64748b" }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: debugController.peerUdpState
                            font.family: monoFamily
                            font.bold: true
                            font.pixelSize: 10
                            color: debugController.peerUdpState === "Active" ? "#22c55e" : "#eab308"
                        }
                    }

                    // Silence threshold progress
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        RowLayout {
                            Text { text: "Silence / Timeout:"; font.family: monoFamily; font.pixelSize: 10; color: "#64748b" }
                            Item { Layout.fillWidth: true }
                            Text {
                                text: debugController.silenceMs + " / 3000 ms"
                                font.family: monoFamily
                                font.pixelSize: 10
                                color: debugController.silenceMs > 2000 ? "#ef4444"
                                       : debugController.silenceMs > 1000 ? "#f59e0b" : "#94a3b8"
                            }
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            height: 5
                            radius: 2
                            color: "#1e293b"
                            Rectangle {
                                width: parent.width * Math.min(1.0, debugController.silencePercent)
                                height: parent.height
                                radius: 2
                                color: debugController.silencePercent > 0.7 ? "#ef4444"
                                       : debugController.silencePercent > 0.4 ? "#f59e0b" : "#0284c7"
                            }
                        }
                    }

                    RowLayout {
                        Text { text: "Heartbeat Pings:"; font.family: monoFamily; font.pixelSize: 10; color: "#64748b" }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: debugController.pingsSent + " tx / " + debugController.pongsReceived + " rx"
                            font.family: monoFamily
                            font.pixelSize: 10
                            color: "#e2e8f0"
                        }
                    }

                    RowLayout {
                        Text { text: "Packets Sent / Recv:"; font.family: monoFamily; font.pixelSize: 10; color: "#64748b" }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: debugController.framesSent + " / " + debugController.packetsReceived
                            font.family: monoFamily
                            font.pixelSize: 10
                            color: "#e2e8f0"
                        }
                    }

                    RowLayout {
                        Text { text: "Bitrate (Tx / Rx):"; font.family: monoFamily; font.pixelSize: 10; color: "#64748b" }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: debugController.sendBitrateKbps.toFixed(1) + " / " + debugController.recvBitrateKbps.toFixed(1) + " kbps"
                            font.family: monoFamily
                            font.bold: true
                            font.pixelSize: 10
                            color: "#38bdf8"
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            // ---------------------------------------------------------------------
            // CARD 2: JITTER BUFFER & UNDERRUNS
            // ---------------------------------------------------------------------
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "#0d131c"
                radius: 6
                border.color: debugController.bufferStarved > 0 ? "#7f1d1d" : "#1e2d42"
                border.width: 1

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 6

                    Text {
                        text: "2. JITTER BUFFER & UNDERRUN"
                        font.family: monoFamily
                        font.bold: true
                        font.pixelSize: 11
                        color: "#a855f7"
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: "#16202e" }

                    RowLayout {
                        Text { text: "Arrival Jitter:"; font.family: monoFamily; font.pixelSize: 10; color: "#64748b" }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: debugController.jitterMs + " ms"
                            font.family: monoFamily
                            font.bold: true
                            font.pixelSize: 10
                            color: debugController.jitterMs > 25 ? "#ef4444" : "#c084fc"
                        }
                    }

                    RowLayout {
                        Text { text: "Cushion Target:"; font.family: monoFamily; font.pixelSize: 10; color: "#64748b" }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: debugController.targetDepth + " frames (" + debugController.targetDepthMs + " ms)"
                            font.family: monoFamily
                            font.bold: true
                            font.pixelSize: 10
                            color: "#f1f5f9"
                        }
                    }

                    RowLayout {
                        Text { text: "Peak Queue Depth:"; font.family: monoFamily; font.pixelSize: 10; color: "#64748b" }
                        Item { Layout.fillWidth: true }
                        Text { text: debugController.peakDepth + " frames"; font.family: monoFamily; font.pixelSize: 10; color: "#94a3b8" }
                    }

                    // Highlight buffer starvation in red (this is audio dropout!)
                    RowLayout {
                        Text { text: "Starved (Underruns):"; font.family: monoFamily; font.pixelSize: 10; color: "#64748b" }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: debugController.bufferStarved + " times"
                            font.family: monoFamily
                            font.bold: true
                            font.pixelSize: 10
                            color: debugController.bufferStarved > 0 ? "#ef4444" : "#22c55e"
                        }
                    }

                    RowLayout {
                        Text { text: "Late / Dropped:"; font.family: monoFamily; font.pixelSize: 10; color: "#64748b" }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: debugController.bufferLate + " late / " + debugController.bufferDropped + " dropped"
                            font.family: monoFamily
                            font.pixelSize: 10
                            color: debugController.bufferLate > 0 ? "#f59e0b" : "#94a3b8"
                        }
                    }

                    RowLayout {
                        Text { text: "Duplicates / Overflow:"; font.family: monoFamily; font.pixelSize: 10; color: "#64748b" }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: debugController.bufferDuplicates + " dup / " + debugController.bufferOverflows + " over"
                            font.family: monoFamily
                            font.pixelSize: 10
                            color: "#94a3b8"
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            // ---------------------------------------------------------------------
            // CARD 3: CODEC, PLC & PACKET LOSS
            // ---------------------------------------------------------------------
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "#0d131c"
                radius: 6
                border.color: "#1e2d42"
                border.width: 1

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 6

                    Text {
                        text: "3. AUDIO CODEC & PLC"
                        font.family: monoFamily
                        font.bold: true
                        font.pixelSize: 11
                        color: "#22c55e"
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: "#16202e" }

                    RowLayout {
                        Text { text: "Audio Codec:"; font.family: monoFamily; font.pixelSize: 10; color: "#64748b" }
                        Item { Layout.fillWidth: true }
                        Text { text: debugController.codecName; font.family: monoFamily; font.bold: true; font.pixelSize: 10; color: "#4ade80" }
                    }

                    RowLayout {
                        Text { text: "Frames Captured:"; font.family: monoFamily; font.pixelSize: 10; color: "#64748b" }
                        Item { Layout.fillWidth: true }
                        Text { text: debugController.framesCaptured + " frames"; font.family: monoFamily; font.pixelSize: 10; color: "#e2e8f0" }
                    }

                    RowLayout {
                        Text { text: "Frames Played:"; font.family: monoFamily; font.pixelSize: 10; color: "#64748b" }
                        Item { Layout.fillWidth: true }
                        Text { text: debugController.framesPlayed + " frames"; font.family: monoFamily; font.pixelSize: 10; color: "#e2e8f0" }
                    }

                    // Opus PLC (Packet Loss Concealment) frames
                    RowLayout {
                        Text { text: "PLC Concealed:"; font.family: monoFamily; font.pixelSize: 10; color: "#64748b" }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: debugController.framesConcealed + " frames"
                            font.family: monoFamily
                            font.bold: true
                            font.pixelSize: 10
                            color: debugController.framesConcealed > 0 ? "#f59e0b" : "#4ade80"
                        }
                    }

                    RowLayout {
                        Text { text: "Silent Playback:"; font.family: monoFamily; font.pixelSize: 10; color: "#64748b" }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: debugController.framesSilent + " frames"
                            font.family: monoFamily
                            font.pixelSize: 10
                            color: debugController.framesSilent > 0 ? "#ef4444" : "#94a3b8"
                        }
                    }

                    RowLayout {
                        Text { text: "Loss Concealment %:"; font.family: monoFamily; font.pixelSize: 10; color: "#64748b" }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: debugController.lossRatePercent.toFixed(2) + " %"
                            font.family: monoFamily
                            font.bold: true
                            font.pixelSize: 10
                            color: debugController.lossRatePercent > 2.0 ? "#ef4444" : "#94a3b8"
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            // ---------------------------------------------------------------------
            // CARD 4: AUDIO HARDWARE, GATE & METERS
            // ---------------------------------------------------------------------
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "#0d131c"
                radius: 6
                border.color: "#1e2d42"
                border.width: 1

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 6

                    Text {
                        text: "4. HARDWARE & NOISE GATE"
                        font.family: monoFamily
                        font.bold: true
                        font.pixelSize: 11
                        color: "#f59e0b"
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: "#16202e" }

                    RowLayout {
                        Text { text: "Noise Gate:"; font.family: monoFamily; font.pixelSize: 10; color: "#64748b" }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: debugController.micGateOpen ? "OPEN (Passed)" : "CLOSED (Muted)"
                            font.family: monoFamily
                            font.bold: true
                            font.pixelSize: 10
                            color: debugController.micGateOpen ? "#22c55e" : "#94a3b8"
                        }
                    }

                    // Local level bar
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        RowLayout {
                            Text { text: "Mic Level (Local):"; font.family: monoFamily; font.pixelSize: 10; color: "#64748b" }
                            Item { Layout.fillWidth: true }
                            Text {
                                text: (debugController.localAudioLevel * 100).toFixed(0) + " %"
                                font.family: monoFamily
                                font.pixelSize: 9
                                color: debugController.localSpeaking ? "#22c55e" : "#94a3b8"
                            }
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            height: 6
                            radius: 2
                            color: "#1e293b"
                            Rectangle {
                                width: parent.width * Math.min(1.0, debugController.localAudioLevel)
                                height: parent.height
                                radius: 2
                                color: debugController.localAudioLevel > 0.8 ? "#ef4444" : "#22c55e"
                            }
                        }
                    }

                    // Remote level bar
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        RowLayout {
                            Text { text: "Remote Peer Level:"; font.family: monoFamily; font.pixelSize: 10; color: "#64748b" }
                            Item { Layout.fillWidth: true }
                            Text {
                                text: (debugController.remoteAudioLevel * 100).toFixed(0) + " %"
                                font.family: monoFamily
                                font.pixelSize: 9
                                color: debugController.remoteSpeaking ? "#38bdf8" : "#94a3b8"
                            }
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            height: 6
                            radius: 2
                            color: "#1e293b"
                            Rectangle {
                                width: parent.width * Math.min(1.0, debugController.remoteAudioLevel)
                                height: parent.height
                                radius: 2
                                color: debugController.remoteAudioLevel > 0.8 ? "#ef4444" : "#38bdf8"
                            }
                        }
                    }

                    RowLayout {
                        Text { text: "Mic Gain / Gate dB:"; font.family: monoFamily; font.pixelSize: 10; color: "#64748b" }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: debugController.micGain.toFixed(1) + "x / " + debugController.gateThresholdDb.toFixed(0) + " dB"
                            font.family: monoFamily
                            font.pixelSize: 10
                            color: "#94a3b8"
                        }
                    }

                    RowLayout {
                        Text { text: "Mute Status:"; font.family: monoFamily; font.pixelSize: 10; color: "#64748b" }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: debugController.muted ? "MUTED" : "ACTIVE"
                            font.family: monoFamily
                            font.bold: true
                            font.pixelSize: 10
                            color: debugController.muted ? "#ef4444" : "#22c55e"
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }
        }

        // =========================================================================
        // DIAGNOSTIC EVENT LOG CONSOLE
        // =========================================================================
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: "#05080c"
            radius: 6
            border.color: "#1e2d42"
            border.width: 1

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 8
                spacing: 4

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: "VERBOSE DIAGNOSTIC LOG (TIMESTAMPED EVENTS)"
                        font.family: monoFamily
                        font.bold: true
                        font.pixelSize: 10
                        color: "#64748b"
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        text: debugController.eventLogs.length + " events"
                        font.family: monoFamily
                        font.pixelSize: 9
                        color: "#475569"
                    }
                }

                ListView {
                    id: logListView
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: debugController.eventLogs
                    boundsBehavior: Flickable.StopAtBounds

                    ScrollBar.vertical: ScrollBar {
                        policy: ScrollBar.AsNeeded
                    }

                    delegate: Text {
                        width: logListView.width
                        text: modelData
                        font.family: monoFamily
                        font.pixelSize: 10
                        wrapMode: Text.WrapAnywhere
                        color: modelData.indexOf("[SPIKE]") !== -1 ? "#f87171"
                               : modelData.indexOf("[FAILOVER]") !== -1 ? "#f59e0b"
                               : modelData.indexOf("[ERROR]") !== -1 ? "#ef4444"
                               : modelData.indexOf("[CARRIER]") !== -1 ? "#38bdf8"
                               : modelData.indexOf("[PLC]") !== -1 ? "#c084fc"
                               : modelData.indexOf("[PING]") !== -1 ? "#34d399" : "#94a3b8"
                    }
                }
            }
        }
    }
}
