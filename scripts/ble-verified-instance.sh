#!/bin/sh
# Print an instance token only when the actual running bluetoothd is identical
# to the caller's known directed-notification build. No adapter mutations.
set -eu
if [ "$#" != 1 ] || [ ! -f "$1" ]; then
    echo 'Usage: sudo sh ble-verified-instance.sh /path/to/verified/fixed/bluetoothd' >&2
    exit 2
fi
dbus() { busctl --system call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus "$@"; }
guid=$(dbus GetId | cut -d '"' -f 2)
owner=$(dbus GetNameOwner s org.bluez | cut -d '"' -f 2)
pid=$(dbus GetConnectionUnixProcessID s "$owner" | cut -d ' ' -f 2)
cmp -s -- "$1" "/proc/$pid/exe" || { echo 'Running bluetoothd does not match the verified executable, or /proc/PID/exe is unreadable' >&2; exit 1; }
test "$(dbus GetNameOwner s org.bluez)" = "s \"$owner\""
test "$(dbus GetId)" = "s \"$guid\""
test "$(id -u)" = 0 || { echo 'Run as root to record the verified instance in /run' >&2; exit 1; }
stamp=$(mktemp /run/deskflow-bridge-bluez-directed.XXXXXX)
trap 'rm -f -- "$stamp"' EXIT
printf '%s/%s\n' "$guid" "$owner" > "$stamp"
chmod 0644 "$stamp"
mv -T -- "$stamp" /run/deskflow-bridge-bluez-directed
printf '%s/%s\n' "$guid" "$owner"
