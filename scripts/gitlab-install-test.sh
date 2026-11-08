#!/bin/sh
# Runs inside the GitLab CI install-test jobs: the release tarball on a bare image of one distribution. install.sh --yes
# must find and install everything Screen+ needs for both desktops with that distribution's package manager, then
# --check must find nothing missing and the program must run. `neon` is Ubuntu 24.04 with KDE neon's archive.
# Usage: scripts/gitlab-install-test.sh <distro>; VERSION comes from the version job.
set -eu
distro=$1
case $distro in
    arch) pacman -Syu --noconfirm --needed tar gzip gawk grep ;;
    fedora) dnf install -y tar gzip gawk grep findutils ;;
    opensuse) zypper --non-interactive install tar gzip gawk grep findutils ;;
    debian|ubuntu|neon)
        export DEBIAN_FRONTEND=noninteractive
        apt-get update
        apt-get install -y --no-install-recommends ca-certificates gawk
        if [ "$distro" = neon ]; then
            apt-get install -y --no-install-recommends curl gnupg
            curl -fsSL https://archive.neon.kde.org/public.key | gpg --dearmor -o /usr/share/keyrings/neon.gpg
            echo "deb [signed-by=/usr/share/keyrings/neon.gpg] https://archive.neon.kde.org/user noble main" > /etc/apt/sources.list.d/neon.list
            sed -i 's/^ID=.*/ID=neon/; s/^ID_LIKE=.*/ID_LIKE="ubuntu debian"/' /etc/os-release 2>/dev/null ||
                { cp --remove-destination "$(readlink -f /etc/os-release)" /etc/os-release; sed -i 's/^ID=.*/ID=neon/; s/^ID_LIKE=.*/ID_LIKE="ubuntu debian"/' /etc/os-release; }
            apt-get update
        fi ;;
    *) echo "Unknown distro: $distro"; exit 2 ;;
esac

dir=$(mktemp -d)
tar -xzf "release/ScreenPlus-$VERSION-linux-x86_64.tar.gz" -C "$dir" --strip-components 1
export SCREENPLUS_DESKTOP=both
if "$dir/install.sh" --check; then echo "Nothing was missing on a bare $distro?"; fi
"$dir/install.sh" --yes
"$HOME/.local/share/screenplus-app/install.sh" --check
"$HOME/.local/bin/screenplus" version | grep -F "$VERSION"
"$HOME/.local/bin/screenplus" help > /dev/null
