/* Plasma's icon layer sits over the wallpaper and keeps the pointer, so the page never sees it. This sits over
 * everything in the desktop's window and only watches: its handlers never claim an event, so icons, rubber band
 * selection and the desktop menu work as before. What it sees goes to the page through WebView.pointer(). */
import QtQuick

Item {
    id: watch
    property Item target
    property point last
    property int buttons: 0

    anchors.fill: parent
    z: 1000000

    Component.onCompleted: {
        let top = parent
        while (top.parent) top = top.parent
        parent = top
    }

    function send(type, p, button) {
        if (!target) return
        const q = mapToItem(target, p.x, p.y)
        target.pointer(type, Math.round(q.x), Math.round(q.y), button, buttons)
    }
    function moved(p) {
        last = p
        if (!tick.running) tick.start()
    }

    Timer {             /* one move a frame at most */
        id: tick
        interval: 16
        onTriggered: watch.send("move", watch.last, 0)
    }

    HoverHandler {
        onPointChanged: watch.moved(point.position)
        onHoveredChanged: if (!hovered && watch.buttons === 0) {
            tick.stop()
            watch.send("leave", watch.last, 0)
        }
    }

    PointHandler {
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
        property int down: 0
        onPointChanged: if (active) watch.moved(point.position)
        onActiveChanged: {
            tick.stop()
            if (active) {
                const b = point.pressedButtons
                down = b & Qt.RightButton ? 2 : b & Qt.MiddleButton ? 1 : 0
                watch.buttons = b
                watch.last = point.position
                watch.send("down", watch.last, down)
                if (down === 2) {           /* the desktop's menu takes the release, so it is sent now */
                    watch.buttons = 0
                    watch.send("up", watch.last, down)
                }
            } else if (down !== 2) {
                watch.buttons = 0
                watch.send("up", watch.last, down)
            }
        }
    }
}
