import QtQuick
import QtWebEngine

/* A paused page really stops: Qt only freezes a page that isn't visible, so its last frame stands in as a picture. */
Item {
    id: view
    property string source
    property string shim       /* file:// URL of the Lively / Wallpaper Engine API shim */
    property string script     /* hands the page its property values */
    property bool paused
    property bool muted: true

    onPausedChanged: {
        if (paused) {
            web.grabToImage(r => {
                if (!view.paused) return
                still.source = r.url
                still.visible = true
                web.visible = false
                web.lifecycleState = WebEngineView.LifecycleState.Frozen
            })
        } else {
            web.lifecycleState = WebEngineView.LifecycleState.Active
            web.visible = true
            still.visible = false
            still.source = ""
        }
    }
    onSourceChanged: web.url = source      /* a binding on url stops following once the page has loaded */
    onShimChanged: {
        web.addScripts()
        if (web.url.toString()) web.reload()     /* a new frame limit only reaches the page this way */
    }
    onScriptChanged: if (script && !web.loading) web.runJavaScript(script)

    /* A Customise recording's property values (Recording.qml) */
    function run(js) { if (!web.loading) web.runJavaScript(js) }

    /* From PointerWatch: type is move, down, up or leave */
    function pointer(type, x, y, button, buttons) {
        web.runJavaScript("window.__spPointer&&__spPointer('" + type + "'," + x + "," + y + "," + button + "," + buttons + ")")
    }

    WebEngineView {
        id: web
        anchors.fill: parent
        audioMuted: view.muted
        backgroundColor: "black"
        settings.playbackRequiresUserGesture: false
        settings.localContentCanAccessFileUrls: true
        settings.localContentCanAccessRemoteUrls: true
        settings.showScrollBars: false

        function pageScript(name, url) {
            const s = WebEngine.script()
            s.name = name
            s.sourceUrl = url
            s.injectionPoint = WebEngineScript.DocumentCreation
            s.worldId = WebEngineScript.MainWorld
            return s
        }
        function addScripts() {
            const list = [ pageScript("screenplus-pointer", Qt.resolvedUrl("pointer.js")) ]
            if (view.shim) list.unshift(pageScript("screenplus-shim", view.shim))
            userScripts.collection = list
        }
        Component.onCompleted: addScripts()
        onLoadingChanged: (info) => {
            if (info.status === WebEngineView.LoadSucceededStatus && view.script) runJavaScript(view.script)
            if (info.status === WebEngineView.LoadFailedStatus) retry.start()
        }
    }

    Image {
        id: still
        anchors.fill: parent
        visible: false
        cache: false
    }

    /* At sign-in the page can be up before the service that hosts it */
    Timer {
        id: retry
        interval: 250
        onTriggered: web.reload()
    }
}
