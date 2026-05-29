import QtQuick 2.15

Item {
    id: root

    property alias text: message.text

    anchors.left: parent.left
    anchors.right: parent.right
    height: 64
    y: visibleState ? 0 : -height
    opacity: visibleState ? 1 : 0

    property bool visibleState: false

    Behavior on y { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
    Behavior on opacity { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }

    Rectangle {
        anchors.fill: parent
        color: "#B71C1C"
        opacity: 0.96
    }

    Text {
        id: message
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: 18
        anchors.rightMargin: 18
        color: "#FFFFFF"
        font.pixelSize: 14
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }

    Timer {
        id: hideTimer
        interval: 4000
        onTriggered: root.visibleState = false
    }

    function showMessage(value) {
        message.text = value
        visibleState = true
        hideTimer.restart()
    }
}
