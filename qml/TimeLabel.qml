import QtQuick 2.15
import QtQuick.Layouts 1.15

Text {
    id: root

    property real seconds: 0
    property bool live: false

    implicitWidth: live ? 46 : 56
    implicitHeight: 22
    Layout.preferredWidth: implicitWidth
    Layout.minimumWidth: implicitWidth
    Layout.maximumWidth: implicitWidth
    Layout.preferredHeight: implicitHeight
    horizontalAlignment: Text.AlignHCenter
    verticalAlignment: Text.AlignVCenter
    text: live ? "LIVE" : formatTime(seconds)
    color: live ? "#FFFFFF" : "#DDDDDD"
    font.family: "monospace"
    font.pixelSize: live ? 11 : 12
    font.bold: live

    Rectangle {
        anchors.fill: parent
        visible: root.live
        z: -1
        radius: 3
        color: "#D32F2F"
    }

    function pad2(value) {
        return value < 10 ? "0" + value : "" + value
    }

    function formatTime(value) {
        if (!isFinite(value) || value < 0)
            value = 0

        var total = Math.floor(value)
        var h = Math.floor(total / 3600)
        var m = Math.floor((total % 3600) / 60)
        var s = total % 60

        if (h > 0)
            return h + ":" + pad2(m) + ":" + pad2(s)
        return pad2(m) + ":" + pad2(s)
    }
}
