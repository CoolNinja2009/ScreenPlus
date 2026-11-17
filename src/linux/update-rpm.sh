#!/bin/sh
# RPM updater: the service already verified this release asset's SHA-256.
# DNF keeps ownership and dependency records intact; Polkit asks the user to authorize the system change.
set -eu

state="${XDG_STATE_HOME:-$HOME/.local/state}/screenplus"
mkdir -p "$state"
exec >>"$state/update.log" 2>&1

rpm_file=${1:-}
case "$rpm_file" in
    /*.rpm) ;;
    *) echo 'update: expected an absolute RPM path'; exit 1 ;;
esac
[ -f "$rpm_file" ] || { echo 'update: verified RPM is missing'; exit 1; }
[ -x /usr/bin/pkexec ] && [ -x /usr/bin/dnf ] || {
    echo 'update: Polkit or DNF is unavailable'
    exit 1
}
identity=$(rpm -qp --qf '%{NAME} %{ARCH}' "$rpm_file") || exit 1
[ "$identity" = 'screenplus x86_64' ] || {
    echo "update: unexpected RPM identity: $identity"
    exit 1
}

echo "update: installing $rpm_file"
/usr/bin/pkexec /usr/bin/dnf install -y "$rpm_file" || {
    echo 'update: authorization or DNF installation failed'
    exit 1
}
/usr/bin/screenplus stop
/usr/bin/screenplus start
echo 'update: Screen+ restarted after RPM upgrade'
