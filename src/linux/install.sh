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
    *)
        for rc in "$HOME/.bashrc" "$HOME/.zshrc"; do
            [ -f "$rc" ] || continue
            grep -q '.local/bin' "$rc" || printf '\nexport PATH="$HOME/.local/bin:$PATH"\n' >> "$rc"
        done
        echo "Open a new terminal to use the screenplus command." ;;
esac

apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
mkdir -p "$apps"
cat > "$apps/screenplus.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Screen+
Comment=Live wallpapers
Exec="$app/screenplus" show
Icon=preferences-desktop-wallpaper
Categories=Utility;
EOF
case "${XDG_CURRENT_DESKTOP:-}" in
    *Hyprland*|*hyprland*)
        echo "Hyprland needs: gtk4 gtk4-layer-shell mpv (libmpv) webkitgtk-6.0" ;;
    *KDE*)
        echo "KDE needs Qt Multimedia, and Qt WebEngine for web wallpapers (Fedora: qt6-qtmultimedia qt6-qtwebengine)" ;;
esac
echo "The window needs SDL3, Cairo, Pango and ffmpeg. Open it with: screenplus show"
