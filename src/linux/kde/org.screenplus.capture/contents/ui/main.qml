/* Records a web wallpaper for the Library, like src/app/preview_capture.c on Windows. The service runs this through
 * plasmawindowed on Qt's offscreen platform (src/linux/preview.c): nothing shows, and a page that misbehaves can only
 * take this process down. Arguments after the applet id: output folder, page, shim, script. */
import QtQuick
import QtWebEngine
import org.kde.plasma.plasmoid

PlasmoidItem {
    id: root
    width: 960
    height: 540

    readonly property var args: {
        const a = Qt.application.arguments, i = a.indexOf("org.screenplus.capture")
        return a.slice(i + 1)
    }
    readonly property string outDir: args[0] || ""
    readonly property int frames: 40
    property int shot: 0

    function finish() {
        web.grabToImage(r => { r.saveToFile(root.outDir + "/done.png"); Qt.quit() }, Qt.size(480, 270))
    }
    function grab() {
        web.grabToImage(r => {
            r.saveToFile(root.outDir + "/" + String(root.shot).padStart(3, "0") + ".png")
            if (++root.shot >= root.frames) root.finish()
            else next.start()
        }, Qt.size(480, 270))
    }

    Timer { id: warmup; interval: 2500; onTriggered: root.grab() }
    Timer { id: next; interval: 100; onTriggered: root.grab() }

    WebEngineView {
        id: web
        width: 960
        height: 540
        zoomFactor: 0.5                 /* a 1920x1080 page, as on Windows */
        backgroundColor: "black"
        audioMuted: true
        settings.playbackRequiresUserGesture: false
        settings.localContentCanAccessFileUrls: true
        settings.localContentCanAccessRemoteUrls: true
        settings.showScrollBars: false
        onLoadingChanged: info => {
            if (info.status !== WebEngineView.LoadSucceededStatus) return
            if (root.args[3]) runJavaScript(root.args[3])
            if (!warmup.running && root.shot === 0) warmup.start()
        }
        Component.onCompleted: {
            if (root.args[2]) {
                const s = WebEngine.script()
                s.name = "screenplus-shim"
                s.sourceUrl = root.args[2]
                s.injectionPoint = WebEngineScript.DocumentCreation
                s.worldId = WebEngineScript.MainWorld
                userScripts.collection = [ s ]
            }
            url = root.args[1]
        }
    }
}
