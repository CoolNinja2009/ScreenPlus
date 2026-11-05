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
    readonly property bool held: configuration.Paused || (configuration.PauseOnFullscreen && fullscreenHere)
    readonly property bool graded: configuration.Brightness !== 0 || configuration.Contrast !== 0 || configuration.Saturation !== 0

    Rectangle {
        anchors.fill: parent
        color: "black"
    }

    Loader {
        id: view
        anchors.fill: parent
        source: root.kind === "video" ? "VideoView.qml"
              : root.kind === "image" ? "ImageView.qml"
              : root.kind === "html" ? "WebView.qml" : ""
        onLoaded: {
            if (root.kind === "html") {     /* the shim first: it has to be in place when the page loads */
                item.shim = Qt.binding(() => root.configuration.ShimUrl)
                item.script = Qt.binding(() => root.configuration.Script)
            }
            item.paused = Qt.binding(() => root.held)
            item.muted = Qt.binding(() => root.configuration.Muted)
            item.source = Qt.binding(() => root.src)
        }
        layer.enabled: root.graded
        layer.effect: MultiEffect {
            brightness: root.configuration.Brightness / 100
            contrast: root.configuration.Contrast / 100
            saturation: root.configuration.Saturation / 100
        }
    }

    Loader {
        id: watch
        source: "FullscreenWatch.qml"
    }
}
