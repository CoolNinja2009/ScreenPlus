import QtQuick
import QtMultimedia

Item {
    id: view
    property string source
    property bool paused
    property bool muted: true
    property real volume: 1

    /* source is set after the Loader has made this item: only ever ask once the player has a file */
    function sync() {
        if (!player.source.toString()) return
        if (paused) player.pause()
        else player.play()
    }
    onPausedChanged: sync()

    MediaPlayer {
        id: player
        source: view.source
        loops: MediaPlayer.Infinite
        videoOutput: output
        audioOutput: AudioOutput { muted: view.muted; volume: view.volume * view.volume }
        onSourceChanged: view.sync()
        onMediaStatusChanged: if (mediaStatus === MediaPlayer.LoadedMedia) view.sync()
        onErrorOccurred: (error, text) => console.warn("Screen+ video:", text)
    }

    VideoOutput {
        id: output
        anchors.fill: parent
        fillMode: VideoOutput.PreserveAspectCrop
    }
}
