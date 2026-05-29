import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

Item {
    id: root

    signal fullscreenClicked()
    signal rendererBadgeClicked()

    property int idleState: 0
    property int openingState: 1
    property int readyState: 2
    property int playingState: 3
    property int pausedState: 4
    property int seekingState: 5
    property int stoppedState: 6
    property int errorState: 7

    property int playbackState: controller ? controller.state() : idleState
    property real currentPosition: controller ? controller.positionSec() : 0
    property real duration: controller ? controller.durationSec() : -1
    property bool live: duration < 0 || (controller && controller.isRealtime())
    property bool draggingProgress: progressSlider.pressed
    property bool controlsEnabled: playbackState !== idleState && playbackState !== openingState
    property bool muted: controller ? controller.isMuted() : false
    property real volume: controller ? controller.volume() : 1.0

    function wakeUp() {
        visiblePart.opacity = 1.0
        if (!draggingProgress)
            hideTimer.restart()
    }

    function syncSlider(pos, dur) {
        duration = dur
        currentPosition = pos
        if (!draggingProgress)
            progressSlider.value = Math.max(progressSlider.from, Math.min(progressSlider.to, pos))
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
        onPositionChanged: root.wakeUp()
    }

    Toast {
        id: toast
        z: 10
    }

    Item {
        id: visiblePart
        anchors.fill: parent
        opacity: 1.0

        Behavior on opacity {
            NumberAnimation { duration: 250; easing.type: Easing.OutCubic }
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 88
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#00000000" }
                GradientStop { position: 1.0; color: "#CC000000" }
            }
        }

        ColumnLayout {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 16
            anchors.rightMargin: 16
            anchors.bottomMargin: 4
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 24
                spacing: 10
                opacity: controlsEnabled ? 1.0 : 0.45

                TimeLabel {
                    seconds: root.currentPosition
                    live: false
                }

                Slider {
                    id: progressSlider
                    Layout.fillWidth: true
                    from: 0
                    to: root.live ? 1 : Math.max(1, root.duration)
                    value: root.live ? 1 : Math.max(0, Math.min(to, root.currentPosition))
                    enabled: controlsEnabled && !root.live

                    onPressedChanged: {
                        root.wakeUp()
                        if (!pressed && enabled && controller)
                            controller.requestSeek(value)
                    }

                    background: Rectangle {
                        x: progressSlider.leftPadding
                        y: progressSlider.topPadding + progressSlider.availableHeight / 2 - height / 2
                        width: progressSlider.availableWidth
                        height: 4
                        radius: 2
                        color: "#444444"

                        Rectangle {
                            width: progressSlider.visualPosition * parent.width
                            height: parent.height
                            radius: 2
                            color: "#4FC3F7"
                        }
                    }

                    handle: Rectangle {
                        x: progressSlider.leftPadding + progressSlider.visualPosition * (progressSlider.availableWidth - width)
                        y: progressSlider.topPadding + progressSlider.availableHeight / 2 - height / 2
                        width: progressSlider.hovered || progressSlider.pressed ? 14 : 12
                        height: width
                        radius: width / 2
                        color: "#FFFFFF"
                        Behavior on width { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }
                    }
                }

                TimeLabel {
                    seconds: root.duration
                    live: root.live
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 56
                spacing: 8
                opacity: controlsEnabled ? 1.0 : 0.45

                RowLayout {
                    Layout.preferredWidth: 200
                    Layout.alignment: Qt.AlignVCenter
                    spacing: 8

                    IconButton {
                        source: root.muted ? "qrc:/resources/icons/volume_muted.svg" : "qrc:/resources/icons/volume.svg"
                        enabled: controlsEnabled
                        onClicked: {
                            if (controller)
                                controller.setMuted(!root.muted)
                            root.wakeUp()
                        }
                    }

                    Slider {
                        id: volumeSlider
                        Layout.preferredWidth: 100
                        from: 0
                        to: 1
                        value: root.volume
                        enabled: controlsEnabled
                        onMoved: {
                            if (controller)
                                controller.setVolume(value)
                            root.wakeUp()
                        }
                        onPressedChanged: {
                            if (!pressed && controller)
                                controller.setVolume(value)
                        }

                        background: Rectangle {
                            x: volumeSlider.leftPadding
                            y: volumeSlider.topPadding + volumeSlider.availableHeight / 2 - height / 2
                            width: volumeSlider.availableWidth
                            height: 3
                            radius: 1.5
                            color: "#444444"
                            Rectangle {
                                width: volumeSlider.visualPosition * parent.width
                                height: parent.height
                                radius: 1.5
                                color: "#4FC3F7"
                            }
                        }

                        handle: Rectangle {
                            x: volumeSlider.leftPadding + volumeSlider.visualPosition * (volumeSlider.availableWidth - width)
                            y: volumeSlider.topPadding + volumeSlider.availableHeight / 2 - height / 2
                            width: volumeSlider.hovered || volumeSlider.pressed ? 12 : 10
                            height: width
                            radius: width / 2
                            color: "#FFFFFF"
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    spacing: 12

                    Item { Layout.fillWidth: true }

                    IconButton {
                        source: "qrc:/resources/icons/prev_frame.svg"
                        enabled: playbackState === pausedState
                    }

                    IconButton {
                        implicitWidth: 48
                        implicitHeight: 48
                        iconSize: 24
                        normalColor: "#20FFFFFF"
                        hoverColor: "#35FFFFFF"
                        pressedColor: "#50FFFFFF"
                        source: playbackState === playingState
                                ? "qrc:/resources/icons/pause.svg"
                                : "qrc:/resources/icons/play.svg"
                        enabled: controlsEnabled && playbackState !== seekingState
                        onClicked: {
                            if (controller)
                                controller.togglePause()
                            root.wakeUp()
                        }
                    }

                    IconButton {
                        source: "qrc:/resources/icons/next_frame.svg"
                        enabled: playbackState === pausedState
                    }

                    Item { Layout.fillWidth: true }
                }

                RowLayout {
                    Layout.preferredWidth: 200
                    Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                    spacing: 10

                    Rectangle {
                        Layout.preferredHeight: 24
                        radius: 3
                        color: "#99000000"
                        border.color: "#22FFFFFF"
                        implicitWidth: modeText.implicitWidth + 12

                        Text {
                            id: modeText
                            anchors.centerIn: parent
                            text: "SW"
                            color: "#DDDDDD"
                            font.pixelSize: 11
                            font.bold: true
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.rendererBadgeClicked()
                        }
                    }

                    IconButton {
                        source: "qrc:/resources/icons/fullscreen.svg"
                        enabled: true
                        onClicked: root.fullscreenClicked()
                    }
                }
            }
        }
    }

    Timer {
        id: hideTimer
        interval: 2500
        repeat: false
        onTriggered: {
            if (!root.draggingProgress)
                visiblePart.opacity = 0
        }
    }

    Connections {
        target: controller

        function onStateChanged(state) {
            root.playbackState = state
            root.wakeUp()
        }

        function onPositionChanged(pos, dur) {
            root.syncSlider(pos, dur)
        }

        function onMediaLoaded() {
            root.duration = controller.durationSec()
            root.currentPosition = controller.positionSec()
            root.wakeUp()
        }

        function onErrorOccurred(errCode, msg) {
            toast.showMessage(msg)
            root.wakeUp()
        }

        function onVolumeChanged(value) {
            root.volume = value
            volumeSlider.value = value
        }

        function onMutedChanged(value) {
            root.muted = value
        }
    }

    Component.onCompleted: {
        root.wakeUp()
        if (controller) {
            root.playbackState = controller.state()
            root.duration = controller.durationSec()
            root.currentPosition = controller.positionSec()
            root.volume = controller.volume()
            root.muted = controller.isMuted()
        }
    }
}
