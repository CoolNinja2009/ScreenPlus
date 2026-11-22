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
    /* Upscaling Sharp (2): sharpen.frag deblurs the picture as Qt shows it, at any size.
     * Smooth has nothing to change here: Qt only scales bilinearly. */
    readonly property bool sharp: configuration.Upscale === 2
    /* Customise layout: the picture's rect in the display's (or the span box's) fractions, null: it covers. The
     * border is black, a colour, or the picture blurred over the whole box (GPU: MultiEffect on a live copy). */
    readonly property var lay: configuration.Layout ? configuration.Layout.split(",").map(Number) : null
    readonly property var cover: configuration.LayoutCover ? configuration.LayoutCover.split(",").map(Number) : null
    readonly property real boxX: configuration.SpanW > 0 ? -configuration.SpanX : 0
    readonly property real boxY: configuration.SpanW > 0 ? -configuration.SpanY : 0
    readonly property real boxW: configuration.SpanW > 0 ? configuration.SpanW : width
    readonly property real boxH: configuration.SpanW > 0 ? configuration.SpanH : height

    /* A Customise recording's value while one plays, else the stored one. */
    function lookValue(key, stored) {
        const v = recording.look[key]
        return v === undefined ? stored : v
    }

    Recording {
        id: recording
        source: root.configuration.Recording
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
        color: root.lay && root.configuration.LayoutFill === 4 ? root.configuration.LayoutColor : "black"
    }

    ShaderEffectSource {
        visible: root.lay !== null && root.cover !== null && root.configuration.LayoutFill === 1 && root.shownKind !== "html"
        sourceItem: visible ? view : null
        live: true
        x: root.cover ? root.boxX + root.cover[0] * root.boxW : 0
        y: root.cover ? root.boxY + root.cover[1] * root.boxH : 0
        width: root.cover ? root.cover[2] * root.boxW : 0
        height: root.cover ? root.cover[3] * root.boxH : 0
        layer.enabled: visible
        layer.effect: MultiEffect {
            blurEnabled: true
            blur: 1.0
            blurMax: 64
            brightness: -0.25
        }
    }

    Item {
        anchors.fill: parent
        layer.enabled: root.sharp
        layer.effect: ShaderEffect {
            property vector2d texel: Qt.vector2d(1 / Math.max(width, 1), 1 / Math.max(height, 1))
            fragmentShader: Qt.resolvedUrl("sharpen.frag.qsb")
        }

        Loader {
            id: view
            x: root.boxX + (root.lay ? root.lay[0] * root.boxW : 0)     /* span: this display's part of the whole */
            y: root.boxY + (root.lay ? root.lay[1] * root.boxH : 0)
            width: root.lay ? root.lay[2] * root.boxW : root.boxW
            height: root.lay ? root.lay[3] * root.boxH : root.boxH
            onLoaded: {
                if (root.shownKind === "html") {    /* the shim first: it has to be in place when the page loads */
                    item.shim = Qt.binding(() => root.configuration.ShimUrl)
                    item.script = Qt.binding(() => root.configuration.Script)
                }
                if (item.stretch !== undefined) item.stretch = Qt.binding(() => root.lay !== null)
                item.paused = Qt.binding(() => root.held)
                item.muted = Qt.binding(() => root.configuration.Muted)
                if (item.volume !== undefined) item.volume = Qt.binding(() => root.configuration.Volume / 100)
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
