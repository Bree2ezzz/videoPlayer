import QtQuick 2.15
import QtQuick.Layouts 1.15

Item {
    id: root

    property url source
    property bool checked: false
    property bool round: true
    property color normalColor: "transparent"
    property color hoverColor: "#22FFFFFF"
    property color pressedColor: "#35FFFFFF"
    property real iconSize: 22
    property real radiusValue: round ? width / 2 : 3

    signal clicked()

    implicitWidth: 36
    implicitHeight: 36
    Layout.preferredWidth: implicitWidth
    Layout.preferredHeight: implicitHeight
    opacity: enabled ? 1.0 : 0.35

    Rectangle {
        anchors.fill: parent
        radius: root.radiusValue
        color: !root.enabled ? "transparent"
              : mouse.pressed ? root.pressedColor
              : mouse.containsMouse ? root.hoverColor
              : root.normalColor
    }

    Image {
        anchors.centerIn: parent
        width: root.iconSize
        height: root.iconSize
        source: root.source
        fillMode: Image.PreserveAspectFit
        smooth: true
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        enabled: root.enabled
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: root.clicked()
    }
}
