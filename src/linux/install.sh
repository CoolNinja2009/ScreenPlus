#!/bin/sh
# Installs this Screen+ folder for the current user: ~/.local/share/screenplus-app, and `screenplus` on PATH.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
app="${XDG_DATA_HOME:-$HOME/.local/share}/screenplus-app"
bin="$HOME/.local/bin"

[ "$here" = "$app" ] && { echo "Already installed here."; exit 0; }
rm -rf "$app"
mkdir -p "$app" "$bin"
cp -r "$here"/. "$app"/
ln -sf "$app/screenplus" "$bin/screenplus"

echo "Installed. Start it with: screenplus start"
case ":$PATH:" in
    *":$bin:"*) ;;
    *) echo "Add $bin to your PATH to use the screenplus command." ;;
esac
case "${XDG_CURRENT_DESKTOP:-}" in
    *Hyprland*|*hyprland*)
        echo "Hyprland needs: gtk4 gtk4-layer-shell mpv (libmpv) webkitgtk-6.0" ;;
    *KDE*)
        echo "KDE needs Qt Multimedia, and Qt WebEngine for web wallpapers (Fedora: qt6-qtmultimedia qt6-qtwebengine)" ;;
esac
