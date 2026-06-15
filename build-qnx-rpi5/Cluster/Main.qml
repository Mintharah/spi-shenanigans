import QtQuick
import QtQuick.Shapes
import QtQuick.Effects
import QtQuick.Controls.Material
import QtQuick.Layouts

Window {
    id: root
    width: 1280
    height: 720
    visible: true
    title: "Mercedes EQ Style Cluster"
    color: "#01040a"

    Material.theme: Material.Dark

    readonly property color eqCyan: "#00d4ff"
    readonly property color horizonGlow: "#122a45"

    // ── Bound directly to backend — no NumberAnimations ──────────────────
    property real speedVal: Vehicle.speed
    property real powerVal: Vehicle.power

    // --- BACKGROUND ---
    Item {
        anchors.fill: parent
        z: -1
        Rectangle {
            anchors.fill: parent
            color: "#01040a"
        }
        Rectangle {
            anchors.centerIn: parent
            width: parent.width
            height: parent.height * 0.4
            opacity: 0.4
            gradient: Gradient {
                orientation: Gradient.Vertical
                GradientStop {
                    position: 0.0
                    color: "transparent"
                }
                GradientStop {
                    position: 0.5
                    color: root.horizonGlow
                }
                GradientStop {
                    position: 1.0
                    color: "transparent"
                }
            }
            layer.enabled: true
            layer.effect: MultiEffect {
                blurEnabled: true
                blur: 0.8
            }
        }
    }

    // --- DIAL COMPONENT ---
    component EQDial: Item {
        id: dial
        width: 425
        height: 425
        property string label: ""
        property real currentValue: 0
        property real maxValue: 220
        property real stopAt: 147
        property bool clockwise: true

        readonly property real baseStartAngle: clockwise ? -210 : 30
        readonly property real totalSweep: clockwise ? 240 : -240

        // 1. Digital Display
        Column {
            anchors.centerIn: parent
            anchors.verticalCenterOffset: 15
            spacing: -15
            z: 1

            Text {
                text: Math.round(dial.currentValue)
                color: "white"
                font.pixelSize: 100
                font.weight: Font.ExtraLight
                font.family: "Century Gothic"
                anchors.horizontalCenter: parent.horizontalCenter
                layer.enabled: true
                layer.effect: MultiEffect {
                    shadowEnabled: true
                    shadowColor: root.eqCyan
                    shadowBlur: 0.2
                    brightness: 0.2
                }
            }
            Text {
                text: dial.label
                color: root.eqCyan
                font.pixelSize: 14
                font.weight: Font.Bold
                font.family: "Century Gothic"
                font.letterSpacing: 4
                opacity: 0.8
                anchors.horizontalCenter: parent.horizontalCenter
            }
        }

        // 2. Ticks and Labels
        Shape {
            anchors.fill: parent
            opacity: 0.4
            ShapePath {
                strokeColor: "#ffffff"
                strokeWidth: 1
                fillColor: "transparent"
                PathAngleArc {
                    centerX: 212.5
                    centerY: 212.5
                    radiusX: 200
                    radiusY: 200
                    startAngle: dial.baseStartAngle
                    sweepAngle: (dial.stopAt / dial.maxValue) * dial.totalSweep
                }
            }
        }

        Repeater {
            model: 31
            delegate: Item {
                anchors.fill: parent
                property real progress: index / 30
                property real tickVal: progress * dial.maxValue
                property real angleDeg: dial.baseStartAngle + (progress * dial.totalSweep)
                property real angleRad: angleDeg * Math.PI / 180
                visible: tickVal <= dial.stopAt

                Rectangle {
                    width: index % 5 === 0 ? 3 : 1.5
                    height: index % 5 === 0 ? 14 : 7
                    color: tickVal <= dial.currentValue ? root.eqCyan : "#44ffffff"
                    x: 212.5 + 190 * Math.cos(angleRad)
                    y: 212.5 + 190 * Math.sin(angleRad)
                    rotation: angleDeg + 90
                }

                Text {
                    visible: index % 5 === 0
                    text: Math.round(tickVal)
                    color: "white"
                    font.pixelSize: 18
                    font.family: "Century Gothic"
                    opacity: tickVal <= dial.currentValue ? 1.0 : 0.4
                    x: 212.5 + 155 * Math.cos(angleRad) - width / 2
                    y: 212.5 + 155 * Math.sin(angleRad) - height / 2
                }
            }
        }

        // 3. Dynamic Glow Arc
        Shape {
            anchors.fill: parent
            z: 2
            layer.enabled: true
            layer.samples: 8
            layer.effect: MultiEffect {
                blurEnabled: true
                blur: 0.3
                brightness: 0.4
            }
            ShapePath {
                strokeColor: root.eqCyan
                strokeWidth: 5
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                PathAngleArc {
                    centerX: 212.5
                    centerY: 212.5
                    radiusX: 200
                    radiusY: 200
                    startAngle: dial.baseStartAngle
                    sweepAngle: (Math.min(dial.currentValue, dial.stopAt) / dial.maxValue) * dial.totalSweep
                }
            }
        }

        // 4. Needle
        Rectangle {
            z: 10
            width: 3
            height: 165
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.verticalCenter
            transformOrigin: Item.Bottom
            rotation: dial.baseStartAngle + 90 + (Math.min(dial.currentValue, dial.stopAt) / dial.maxValue) * dial.totalSweep
            antialiasing: true
            gradient: Gradient {
                GradientStop {
                    position: 0.0
                    color: "transparent"
                }
                GradientStop {
                    position: 0.1
                    color: root.eqCyan
                }
                GradientStop {
                    position: 0.3
                    color: "#ffffff"
                }
                GradientStop {
                    position: 0.5
                    color: "transparent"
                }
            }
        }
    }

    // --- MAIN CENTER CONTENT ---
    Item {
        anchors.centerIn: parent
        width: 1200
        height: 425

        // ── Left gauge: speed with smoothed animation ─────────────────────
        EQDial {
            id: leftGauge
            anchors.left: parent.left
            label: "KM/H"
            maxValue: 220
            stopAt: 147
            clockwise: true

            property real _target: Vehicle.speed
            currentValue: _target
            Behavior on _target {
                SmoothedAnimation {
                    velocity: 40
                }
            }
        }

        // ── Center info panel ─────────────────────────────────────────────
        Item {
            id: infoSquare
            width: 500
            height: 320
            anchors.centerIn: parent
            z: 5
            // Clock
            Text {
                id: clockText
                anchors.top: parent.top
                anchors.topMargin: 20
                anchors.horizontalCenter: parent.horizontalCenter
                color: "white"
                font.pixelSize: 28
                font.family: "Century Gothic"
                font.weight: Font.DemiBold
                layer.enabled: true
                layer.effect: MultiEffect {
                    shadowEnabled: true
                    shadowColor: root.eqCyan
                    shadowBlur: 0.3
                }
                function updateTime() {
                    clockText.text = Qt.formatTime(new Date(), "hh:mm");
                }
                Component.onCompleted: updateTime()  // ← call immediately on startup
            }

            Timer {
                interval: 1000
                running: true
                repeat: true
                onTriggered: clockText.updateTime()
            }

            // Car image — only blinks red on criticalAlert
            Image {
                id: myImage
                anchors.horizontalCenter: parent.horizontalCenter
                height: 380
                width: 620
                y: -45
                source: "qrc:/images/images/car.png"
                fillMode: Image.PreserveAspectFit

                MultiEffect {
                    id: redOverlay
                    source: myImage
                    anchors.fill: myImage
                    colorization: Vehicle.criticalAlert ? 1.0 : 0.0
                    colorizationColor: "#FF3131"
                    opacity: 1.0

                    SequentialAnimation on opacity {
                        running: Vehicle.criticalAlert
                        loops: Animation.Infinite
                        NumberAnimation {
                            to: 0.2
                            duration: 150
                        }
                        NumberAnimation {
                            to: 1.0
                            duration: 150
                        }
                        onStopped: redOverlay.opacity = 1.0
                    }
                }
            }

            // Warning icons row
            RowLayout {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 5
                y: 200
                Item {
                    width: 25
                }
                // speedWarningImage — Speed warning
                Image {
                    id: speedWarningImage
                    source: "qrc:/images/images/speed.svg"
                    Layout.preferredWidth: 90
                    Layout.preferredHeight: 110

                    MultiEffect {
                        source: speedWarningImage
                        anchors.fill: speedWarningImage
                        colorization: 1.0
                        colorizationColor: Vehicle.speedWarning ? "#FF3131" : "#888888"
                        opacity: Vehicle.speedWarning ? 1.0 : 0.3

                        SequentialAnimation on opacity {
                            running: Vehicle.speedWarning
                            loops: Animation.Infinite
                            NumberAnimation {
                                to: 0
                                duration: Vehicle.criticalAlert ? 150 : 500
                            }
                            NumberAnimation {
                                to: 1
                                duration: Vehicle.criticalAlert ? 150 : 500
                            }
                        }
                    }
                }

                // tempWarningImage — Temp warning
                Image {
                    id: tempWarningImage
                    source: "qrc:/images/images/temp.svg"
                    Layout.preferredWidth: 100
                    Layout.preferredHeight: 120

                    MultiEffect {
                        source: tempWarningImage
                        anchors.fill: tempWarningImage
                        colorization: 1.0
                        colorizationColor: Vehicle.tempWarning ? "#FF3131" : "#888888"
                        opacity: Vehicle.tempWarning ? 1.0 : 0.3

                        SequentialAnimation on opacity {
                            running: Vehicle.tempWarning
                            loops: Animation.Infinite
                            NumberAnimation {
                                to: 0
                                duration: Vehicle.criticalAlert ? 150 : 500
                            }
                            NumberAnimation {
                                to: 1
                                duration: Vehicle.criticalAlert ? 150 : 500
                            }
                        }
                    }
                }

                // vibrationWarningImage — Vibration warning
                Image {
                    id: vibrationWarningImage
                    source: "qrc:/images/images/vibration.svg"
                    Layout.preferredWidth: 120
                    Layout.preferredHeight: 120

                    MultiEffect {
                        source: vibrationWarningImage
                        anchors.fill: vibrationWarningImage
                        colorization: 1.0
                        colorizationColor: Vehicle.vibWarning ? "#FF3131" : "#888888"
                        opacity: Vehicle.vibWarning ? 1.0 : 0.3

                        SequentialAnimation on opacity {
                            running: Vehicle.vibWarning
                            loops: Animation.Infinite
                            NumberAnimation {
                                to: 0
                                duration: Vehicle.criticalAlert ? 150 : 500
                            }
                            NumberAnimation {
                                to: 1
                                duration: Vehicle.criticalAlert ? 150 : 500
                            }
                        }
                    }
                }

                // voltageWarningImage — Voltage warning
                Image {
                    id: voltageWarningImage
                    source: "qrc:/images/images/voltage.svg"
                    Layout.preferredWidth: 120
                    Layout.preferredHeight: 120

                    MultiEffect {
                        source: voltageWarningImage
                        anchors.fill: voltageWarningImage
                        colorization: 1.0
                        colorizationColor: Vehicle.voltageWarning ? "#FF3131" : "#888888"
                        opacity: Vehicle.voltageWarning ? 1.0 : 0.3

                        SequentialAnimation on opacity {
                            running: Vehicle.voltageWarning
                            loops: Animation.Infinite
                            NumberAnimation {
                                to: 0
                                duration: Vehicle.criticalAlert ? 150 : 500
                            }
                            NumberAnimation {
                                to: 1
                                duration: Vehicle.criticalAlert ? 150 : 500
                            }
                        }
                    }
                }
            }

            // Border glow
            Rectangle {
                anchors.fill: parent
                color: "transparent"
                border.color: root.eqCyan
                border.width: 1.5
                radius: 15
                opacity: 0.15
                layer.enabled: true
                layer.effect: MultiEffect {
                    blurEnabled: true
                    blur: 0.5
                    brightness: 0.5
                }
            }
        }

        // ── Right gauge: power % with correct scale ───────────────────────
        EQDial {
            id: rightGauge
            anchors.right: parent.right
            label: "POWER %"
            maxValue: 220
            stopAt: 147
            clockwise: false

            property real _target: Vehicle.power
            currentValue: _target
            Behavior on _target {
                SmoothedAnimation {
                    velocity: 30
                }
            }
        }
    }

    // --- BOTTOM STATS SECTION ---
    Item {
        id: footerContainer
        width: 880
        height: 125
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 50
        anchors.horizontalCenter: parent.horizontalCenter

        Rectangle {
            anchors.fill: parent
            color: Qt.rgba(0.08, 0.08, 0.12, 0.5)
            radius: 14
            border.color: Qt.rgba(1, 1, 1, 0.12)
            layer.enabled: true
            layer.effect: MultiEffect {
                blurEnabled: true
                blur: 0.5
                brightness: 0.1
            }
        }

        Column {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 0

            Item {
                width: parent.width
                height: 35
                Text {
                    text: "ACCUMULATED TOTAL"
                    color: "white"
                    font.pixelSize: 12
                    font.letterSpacing: 2
                    font.weight: Font.Bold
                    font.family: "Century Gothic"
                    opacity: 0.8
                    anchors.left: parent.left
                    anchors.leftMargin: 15
                    anchors.verticalCenter: parent.verticalCenter
                }
                Rectangle {
                    anchors.bottom: parent.bottom
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: parent.width - 30
                    height: 1
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop {
                            position: 0.0
                            color: "transparent"
                        }
                        GradientStop {
                            position: 0.5
                            color: Qt.rgba(1, 1, 1, 0.2)
                        }
                        GradientStop {
                            position: 1.0
                            color: "transparent"
                        }
                    }
                }
            }

            Row {
                width: parent.width
                height: 75
                Repeater {
                    model: [
                        {
                            v: "03:21",
                            u: "h"
                        },
                        {
                            v: "116.3",
                            u: "km"
                        },
                        {
                            v: "35",
                            u: "km/h"
                        },
                        {
                            v: "18.4",
                            u: "kWh/100"
                        }
                    ]
                    delegate: Item {
                        width: 880 / 4
                        height: 75
                        Rectangle {
                            visible: index > 0
                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            width: 1
                            height: 35
                            gradient: Gradient {
                                GradientStop {
                                    position: 0.0
                                    color: "transparent"
                                }
                                GradientStop {
                                    position: 0.5
                                    color: Qt.rgba(1, 1, 1, 0.15)
                                }
                                GradientStop {
                                    position: 1.0
                                    color: "transparent"
                                }
                            }
                        }
                        Column {
                            anchors.centerIn: parent
                            spacing: 2
                            Text {
                                text: modelData.v
                                color: "white"
                                font.pixelSize: 32
                                font.family: "Century Gothic"
                                anchors.horizontalCenter: parent.horizontalCenter
                            }
                            Text {
                                text: modelData.u
                                color: root.eqCyan
                                font.pixelSize: 11
                                font.weight: Font.Bold
                                font.family: "Century Gothic"
                                anchors.horizontalCenter: parent.horizontalCenter
                            }
                        }
                    }
                }
            }
        }
    }

    // --- STATUS FOOTER ---
    Item {
        width: 880
        height: 35
        anchors.top: footerContainer.bottom
        anchors.topMargin: 10
        anchors.horizontalCenter: parent.horizontalCenter

        Row {
            anchors.left: parent.left
            spacing: 18
            Text {
                text: "NORMAL"
                color: "white"
                font.pixelSize: 14
                font.weight: Font.Bold
                font.family: "Century Gothic"
            }
            Text {
                text: Math.round(Vehicle.battery) + "%"
                color: Vehicle.voltageWarning ? "#FF3131" : "white"
                font.pixelSize: 14
                font.family: "Century Gothic"
            }
            Text {
                text: "P"
                color: root.eqCyan
                font.pixelSize: 16
                font.weight: Font.Black
            }
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "ODO  657 km"
            color: "white"
            font.pixelSize: 13
            font.family: "Century Gothic"
            font.letterSpacing: 1.8
            opacity: 0.9
        }
        Text {
            anchors.right: parent.right
            text: Math.round(Vehicle.temp) + "°C"
            color: Vehicle.tempWarning ? "#FF3131" : "white"
            font.pixelSize: 13
            font.family: "Century Gothic"
            font.letterSpacing: 1.8
            opacity: 0.9
        }
    }
}
