import QtQuick

AnimatedImage {
    property bool muted
    property bool stretch       /* a Customise layout: the item is the picture's rect */
    fillMode: stretch ? Image.Stretch : Image.PreserveAspectCrop
    asynchronous: true
    cache: false
}
