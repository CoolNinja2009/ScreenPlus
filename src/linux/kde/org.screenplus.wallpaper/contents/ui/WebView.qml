import QtQuick
import QtWebEngine

WebEngineView {
    id: web
    property string source
    property string shim       /* file:// URL of the Lively / Wallpaper Engine API shim */
    property string script     /* hands the page its property values */
    property bool paused
    property bool muted: true

    audioMuted: muted
    backgroundColor: "black"
    lifecycleState: paused ? WebEngineView.LifecycleState.Frozen : WebEngineView.LifecycleState.Active
    settings.playbackRequiresUserGesture: false
    settings.localContentCanAccessFileUrls: true
    settings.localContentCanAccessRemoteUrls: true
    settings.showScrollBars: false

    onSourceChanged: url = source      /* a binding on url stops following once the page has loaded */
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
        if (shim) list.unshift(pageScript("screenplus-shim", shim))
        userScripts.collection = list
    }
    Component.onCompleted: addScripts()
    onShimChanged: addScripts()

    /* From PointerWatch: type is move, down, up or leave */
    function pointer(type, x, y, button, buttons) {
        runJavaScript("window.__spPointer&&__spPointer('" + type + "'," + x + "," + y + "," + button + "," + buttons + ")")
    }
    onScriptChanged: if (script && !loading) runJavaScript(script)
    onLoadingChanged: (info) => {
        if (info.status === WebEngineView.LoadSucceededStatus && script) runJavaScript(script)
    }
}
