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
    onShimChanged: {
        if (!shim) return
        const s = WebEngine.script()
        s.name = "screenplus-shim"
        s.sourceUrl = shim
        s.injectionPoint = WebEngineScript.DocumentCreation
        s.worldId = WebEngineScript.MainWorld
        userScripts.collection = [ s ]
    }
    onScriptChanged: if (script && !loading) runJavaScript(script)
    onLoadingChanged: (info) => {
        if (info.status === WebEngineView.LoadSucceededStatus && script) runJavaScript(script)
    }
}
