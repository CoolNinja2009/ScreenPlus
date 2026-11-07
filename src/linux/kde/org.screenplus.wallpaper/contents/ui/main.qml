/* Screen+ on Plasma. The service writes this plugin's config (src/linux/desktop_kde.c) and everything below
 * follows it. Each view is its own file behind a Loader, so a missing Qt module (QtWebEngine, say) costs
 * that kind of wallpaper, not the plugin. */
import QtQuick
import QtQuick.Effects
import org.kde.plasma.plasmoid

WallpaperItem {
    id: root

    readonly property string kind: configuration.Kind
    readonly property string src: configuration.Source
    property bool fullscreenHere: watch.item ? watch.item.fullscreen : false
    property bool coveredHere: watch.item ? watch.item.covered : false
    property bool appHere: watch.item ? watch.item.ruleApp : false
    readonly property bool held: configuration.Paused || (configuration.PauseOnFullscreen && fullscreenHere)
                                 || (configuration.PauseWhenCovered && coveredHere) || appHere
    readonly property bool graded: configuration.Brightness !== 0 || configuration.Contrast !== 0 || configuration.Saturation !== 0
                                   || recording.hasLook

    /* A Customise recording's value while one plays, else the stored one. */
    function lookValue(key, stored) {
        const v = recording.look[key]
        return v === undefined ? stored : v
    }

    Recording {
        id: recording
        json: root.configuration.Recording
        running: !root.held
        onPropsScript: js => { if (root.shownKind === "html" && view.item && view.item.run) view.item.run(js) }
    }

    /* What the view shows. Every switch gets a new view: Source and Kind change one after the other, and a GIF put
     * into a view that was showing an image never starts. The timer lets both land first. */
    property string shownKind
    property string shownSrc

    function show() {
        view.source = ""
        shownKind = kind
        shownSrc = src
        view.source = kind === "video" ? "VideoView.qml"
                    : kind === "image" ? "ImageView.qml"
                    : kind === "html" ? "WebView.qml" : ""
    }
    onKindChanged: settle.restart()
    onSrcChanged: settle.restart()
    Component.onCompleted: show()

    Timer {
        id: settle
        interval: 0
        onTriggered: root.show()
    }

    Rectangle {
        anchors.fill: parent
        color: "black"
    }

    Loader {
        id: view
        x: root.configuration.SpanW > 0 ? -root.configuration.SpanX : 0     /* span: this display's part of the whole */
        y: root.configuration.SpanW > 0 ? -root.configuration.SpanY : 0
        width: root.configuration.SpanW > 0 ? root.configuration.SpanW : root.width
        height: root.configuration.SpanW > 0 ? root.configuration.SpanH : root.height
        onLoaded: {
            if (root.shownKind === "html") {    /* the shim first: it has to be in place when the page loads */
                item.shim = Qt.binding(() => root.configuration.ShimUrl)
                item.script = Qt.binding(() => root.configuration.Script)
            }
            item.paused = Qt.binding(() => root.held)
            item.muted = Qt.binding(() => root.configuration.Muted)
            item.source = root.shownSrc      /* not a binding: the old view lingers a moment after a switch */
            recording.reset()
        }
        layer.enabled: root.graded
        layer.effect: MultiEffect {
            brightness: root.lookValue("brightness", root.configuration.Brightness) / 100
            contrast: root.lookValue("contrast", root.configuration.Contrast) / 100
            saturation: root.lookValue("saturation", root.configuration.Saturation) / 100
        }
    }

    Loader {
        id: watch
        source: "FullscreenWatch.qml"
        onLoaded: item.rules = Qt.binding(() => root.configuration.PauseApps)
    }

    Loader {
        active: root.shownKind === "html" && root.configuration.Mouse && !root.held && view.status === Loader.Ready
        source: "PointerWatch.qml"
        onLoaded: item.target = Qt.binding(() => view.item)
    }
}
