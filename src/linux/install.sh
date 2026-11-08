#!/bin/sh
# Installs this Screen+ folder for the current user: ~/.local/share/screenplus-app, and `screenplus` on PATH.
# The folder carries SDL3, gtk4-layer-shell and FFmpeg itself (built on Ubuntu 24.04, so any glibc 2.39+ distro). What
# it still takes from the system is checked here: libraries the programs can't find (ldd), Plasma's Qt Multimedia and
# Qt WebEngine, and the commands it runs. Anything missing is named in the distribution's own packages and, when run
# from a terminal, installed with sudo after asking (apt, dnf, zypper, pacman).
#   --yes    install missing packages without asking     --check  only report what is missing (exit 1 if anything is)
#   --no-deps  skip the check (update.sh)                SCREENPLUS_DESKTOP=kde|hyprland|both  check for that desktop
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
app="${XDG_DATA_HOME:-$HOME/.local/share}/screenplus-app"
bin="$HOME/.local/bin"
yes= check= deps=1
for a in "$@"; do
    case $a in
        -y|--yes) yes=1 ;;
        --check) check=1 ;;
        --no-deps) deps= ;;
        *) echo "Usage: install.sh [--yes | --check | --no-deps]"; exit 2 ;;
    esac
done

# ---- What this system is
os=$( (. /etc/os-release 2>/dev/null; echo "${ID:-} ${ID_LIKE:-}") || true)
pm=
for c in apt-get dnf zypper pacman; do command -v $c >/dev/null 2>&1 && { pm=$c; break; }; done

kde= hypr=
case "${SCREENPLUS_DESKTOP:-}" in
    kde) kde=1 ;; hyprland) hypr=1 ;; both) kde=1; hypr=1 ;;
    *)
        case "${XDG_CURRENT_DESKTOP:-}" in *KDE*) kde=1 ;; *[Hh]yprland*) hypr=1 ;; esac
        if [ -z "$kde$hypr" ]; then      # not in the session (SSH, a TTY): whatever is installed
            command -v plasmashell >/dev/null 2>&1 && kde=1
            command -v Hyprland >/dev/null 2>&1 && hypr=1
        fi ;;
esac

# ---- What is missing: "kind:name" words (lib:<soname>, qml:<module>, cmd:<command>)
missing= notes=
need() { missing="$missing $1"; }
libs() {       # what a program loads; some systems have no ldd script, only the loader
    if command -v ldd >/dev/null 2>&1; then ldd "$1" 2>/dev/null || true
    else /lib64/ld-linux-x86-64.so.2 --list "$1" 2>/dev/null || true; fi
}
note() { notes="$notes
  - $1"; }
check_system() {
    missing= notes=
    glibc=$(getconf GNU_LIBC_VERSION 2>/dev/null | awk '{print $2}')
    if [ -z "$glibc" ]; then
        note "This system has no glibc (musl?): Screen+'s programs need glibc 2.39 or newer."
    elif [ "$(printf '%s\n' "$glibc" | awk -F. '{print ($1 > 2 || ($1 == 2 && $2 >= 39))}')" != 1 ]; then
        note "glibc $glibc is too old: Screen+ needs 2.39 or newer (Ubuntu 24.04, Debian 13, Fedora 40 or later)."
    fi
    if [ -z "$kde$hypr" ]; then note "Neither KDE Plasma nor Hyprland found: Screen+ for Linux runs on those two."; fi
    if [ -n "$kde" ]; then
        ps=$(command -v plasmashell 2>/dev/null || true)     # Plasma 5 is built on Qt 5
        if [ -n "$ps" ] && libs "$ps" | grep -q libQt5Core; then note "This is Plasma 5: Screen+ needs Plasma 6."; fi
    fi
    progs="screenplus screenplus-ui.so screenplus-depth.so"
    [ -n "$hypr" ] && progs="$progs screenplus-engine"
    for p in $progs; do
        [ -f "$here/$p" ] || continue
        for l in $(libs "$here/$p" | awk '/not found/ {print $1}'); do
            case " $missing " in *" lib:$l "*) ;; *) need "lib:$l" ;; esac
        done
    done
    if [ -n "$kde" ]; then
        qml=$(qtpaths6 --query QT_INSTALL_QML 2>/dev/null || qmake6 -query QT_INSTALL_QML 2>/dev/null || true)
        for m in QtMultimedia QtWebEngine; do
            found=
            for d in $qml /usr/lib/qt6/qml /usr/lib64/qt6/qml /usr/lib/*-linux-gnu/qt6/qml; do
                [ -f "$d/$m/qmldir" ] && { found=1; break; }
            done
            [ -n "$found" ] || need "qml:$m"
        done
    fi
    for c in ffmpeg curl; do command -v $c >/dev/null 2>&1 || need "cmd:$c"; done
}

# The package that gives one missing piece, per package manager. dnf and zypper find a library or a command by what
# it provides, so only apt and pacman need names.
package() {
    kind=${1%%:*} name=${1#*:}
    case $pm:$kind in
        dnf:lib|zypper:lib) echo "$name()(64bit)" ;;
        dnf:cmd|zypper:cmd) echo "/usr/bin/$name" ;;
        dnf:qml) case $name in QtMultimedia) echo qt6-qtmultimedia ;; *) echo qt6-qtwebengine ;; esac ;;
        zypper:qml) case $name in QtMultimedia) echo qt6-multimedia-imports ;; *) echo qt6-webengine-imports ;; esac ;;
        pacman:qml) case $name in QtMultimedia) echo qt6-multimedia ;; *) echo qt6-webengine ;; esac ;;
        apt-get:qml)
            # KDE neon builds its own Qt, named qt6-*, which replaces Ubuntu's qml6-module-*
            case " $os " in
                *" neon "*) case $name in QtMultimedia) echo qt6-multimedia ;; *) echo qt6-webengine ;; esac ;;
                *) case $name in QtMultimedia) echo qml6-module-qtmultimedia ;; *) echo qml6-module-qtwebengine ;; esac ;;
            esac ;;
        *:cmd) echo "$name" ;;      # ffmpeg and curl have their own name everywhere
        apt-get:lib)
            case $name in
                libgtk-4.so.1) echo libgtk-4-1 ;;
                libwebkitgtk-6.0.so.4|libjavascriptcoregtk-6.0.so.1) echo libwebkitgtk-6.0-4 ;;
                libmpv.so.2) echo libmpv2 ;;
                libepoxy.so.0) echo libepoxy0 ;;
                libcairo.so.2) echo libcairo2 ;;
                libpango-1.0.so.0) echo libpango-1.0-0 ;;
                libpangocairo-1.0.so.0) echo libpangocairo-1.0-0 ;;
                libfontconfig.so.1) echo libfontconfig1 ;;
                libdav1d.so.7) echo libdav1d7 ;;
                libz.so.1) echo zlib1g ;;
                libwayland-client.so.0) echo libwayland-client0 ;;
                libglib-2.0.so.0|libgobject-2.0.so.0|libgio-2.0.so.0) echo libglib2.0-0t64 ;;
            esac ;;
        pacman:lib)
            case $name in
                libgtk-4.so.1) echo gtk4 ;;
                libwebkitgtk-6.0.so.4|libjavascriptcoregtk-6.0.so.1) echo webkitgtk-6.0 ;;
                libmpv.so.2) echo mpv ;;
                libepoxy.so.0) echo libepoxy ;;
                libcairo.so.2) echo cairo ;;
                libpango-1.0.so.0|libpangocairo-1.0.so.0) echo pango ;;
                libfontconfig.so.1) echo fontconfig ;;
                libdav1d.so.7) echo dav1d ;;
                libz.so.1) echo zlib ;;
                libwayland-client.so.0) echo wayland ;;
                libglib-2.0.so.0|libgobject-2.0.so.0|libgio-2.0.so.0) echo glib2 ;;
            esac ;;
    esac
}

report() {
    if [ -n "$notes" ]; then echo "Note:$notes"; fi
    if [ -z "$missing" ]; then return 0; fi
    pkgs= qpkgs= unknown=
    for m in $missing; do
        p=$(package "$m")
        if [ -n "$p" ]; then case " $pkgs " in *" $p "*) ;; *) pkgs="$pkgs $p"; qpkgs="$qpkgs '$p'" ;; esac
        else unknown="$unknown ${m#*:}"; fi
    done
    echo "Screen+ needs these from the system:$(for m in $missing; do printf ' %s' "${m#*:}"; done)"
    if [ -n "$unknown" ]; then echo "Install the packages that provide:$unknown"; fi
    return 1
}

install_cmd() {
    case $pm in
        apt-get) echo "apt-get update && apt-get install -y$qpkgs" ;;
        dnf) echo "dnf install -y$qpkgs" ;;
        zypper) echo "zypper --non-interactive install$qpkgs" ;;
        pacman) echo "pacman -S --needed --noconfirm$qpkgs" ;;
    esac
}

if [ -n "$deps" ]; then
    check_system
    if report; then
        [ -n "$check" ] && { echo "Everything Screen+ needs is here."; exit 0; }
    else
        [ -n "$check" ] && exit 1
        cmd=$(install_cmd)
        if [ -n "$pkgs" ] && [ -n "$cmd" ]; then
            go=$yes
            if [ -z "$go" ] && [ -t 0 ]; then
                printf 'Install%s now? [Y/n] ' "$pkgs"
                read -r ans || ans=n
                case $ans in [Nn]*) ;; *) go=1 ;; esac
            fi
            if [ -n "$go" ]; then
                if [ "$(id -u)" = 0 ]; then sh -c "$cmd"
                elif command -v sudo >/dev/null 2>&1; then sudo sh -c "$cmd"
                elif command -v pkexec >/dev/null 2>&1; then pkexec sh -c "$cmd"
                else echo "No sudo: run as root: $cmd"; fi || echo "The package install failed; Screen+ is installed anyway."
                check_system
                report || true
            else
                echo "To install them later: sudo sh -c \"$cmd\""
            fi
        fi
    fi
fi

# ---- Install
[ "$here" = "$app" ] && { echo "Already installed here."; exit 0; }
rm -rf "$app"
mkdir -p "$app" "$bin"
cp -r "$here"/. "$app"/
ln -sf "$app/screenplus" "$bin/screenplus"

cat > "$app/update.sh" <<'EOF'
#!/bin/sh
# Auto update: called with the verified release tarball. Waits for Screen+ to close, installs it, starts it again.
set -u
exec >/dev/null 2>&1
tmp=$(mktemp -d)
tar -xzf "$1" -C "$tmp" --strip-components 1 || exit 1
"$HOME/.local/bin/screenplus" stop
sleep 2
"$tmp/install.sh" --no-deps
rm -rf "$tmp"
"$HOME/.local/bin/screenplus" start
EOF
chmod +x "$app/update.sh"

apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
mkdir -p "$apps"
rm -f "$apps/screenplus.desktop"     # the name before it matched the window's app id
icons="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/256x256/apps"
mkdir -p "$icons"
cp "$here/screenplus.png" "$icons/org.screenplus.ScreenPlus.png"
cat > "$apps/org.screenplus.ScreenPlus.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Screen+
Comment=Live wallpapers
Exec="$app/screenplus" show
Icon=org.screenplus.ScreenPlus
Categories=Utility;
EOF

echo "Installed. Start it with: screenplus start   Open the window with: screenplus show"
case ":$PATH:" in
    *":$bin:"*) ;;
    *)
        for rc in "$HOME/.bashrc" "$HOME/.zshrc"; do
            [ -f "$rc" ] || continue
            grep -q '.local/bin' "$rc" || printf '\nexport PATH="$HOME/.local/bin:$PATH"\n' >> "$rc"
        done
        echo "Open a new terminal to use the screenplus command." ;;
esac
echo "Also used when present: yt-dlp (links), bsdtar (sample updates), pw-record (audio for pages), notify-send, kdialog or zenity."
