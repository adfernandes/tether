#!/usr/bin/env bash
# A single lifecycle owner for the private session bus, OBEX and Tether.
set -euo pipefail
umask 077
children=()

fail() { printf 'tether-container: %s\n' "$*" >&2; exit 1; }
# Invoked by the EXIT trap, including exits from a signal/readiness failure.
# shellcheck disable=SC2329
cleanup() {
    local status=$? pid deadline alive
    trap - EXIT
    # Do not interrupt reaping if Docker/user sends another shutdown signal.
    trap '' TERM INT
    for pid in "${children[@]}"; do kill -TERM "$pid" 2>/dev/null || true; done
    deadline=$((SECONDS + 10))
    while (( SECONDS < deadline )); do
        alive=false
        for pid in "${children[@]}"; do
            if kill -0 "$pid" 2>/dev/null; then alive=true; fi
        done
        $alive || break
        sleep 0.1
    done
    for pid in "${children[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then kill -KILL "$pid" 2>/dev/null || true; fi
        wait "$pid" 2>/dev/null || true
    done
    exit "$status"
}
trap cleanup EXIT
trap 'exit 0' TERM INT

[[ $# == 1 && $1 == tetherd ]] || fail 'unsupported-command: use docker exec for CLI commands'
uid=$(id -u)
[[ $uid != 0 ]] || fail 'root-user: configure a non-root host UID/GID'
hostname=$(uname -n)
[[ $hostname =~ ^[[:alnum:]]([[:alnum:]-]{0,61}[[:alnum:]])?$ ]] || fail 'invalid-layout: choose a DNS-style hostname'

require_env() {
    [[ ${!1-} == "$2" ]] || fail "invalid-layout: $1 must be $2 (configure mounts instead)"
}
require_env HOME /data
require_env XDG_CONFIG_HOME /data/.config
require_env XDG_DATA_HOME /data/.local/share
require_env XDG_STATE_HOME /data/.local/state
require_env XDG_DOWNLOAD_DIR /downloads
require_env XDG_RUNTIME_DIR /run/tether-runtime
require_env XDG_CACHE_HOME /run/tether-runtime/cache
require_env DBUS_SYSTEM_BUS_ADDRESS unix:path=/host/run/dbus/system_bus_socket
require_env DBUS_SESSION_BUS_ADDRESS unix:path=/run/tether-runtime/bus
require_env TETHER_LOG_STDERR 1
require_env TETHER_NO_AUTOSTART 1
# Never accidentally attach to a desktop inherited through operator environment overrides.
unset DISPLAY WAYLAND_DISPLAY

for dir in /data /downloads /run/tether-runtime; do
    [[ -d $dir && ! -L $dir ]] || fail "invalid-layout: $dir must be a directory mount"
    [[ $(stat -c %u "$dir") == "$uid" ]] || fail "wrong-owner: prepare $dir for UID $uid on the host"
    [[ -w $dir && -x $dir ]] || fail "unwritable-volume: $dir must be writable by UID $uid"
done
for dir in /data /run/tether-runtime; do
    mode=$(stat -c %a "$dir")
    (( (8#$mode & 077) == 0 )) || fail "invalid-layout: $dir must not grant group/other access (use mode 0700)"
done
mkdir -p "$XDG_CONFIG_HOME/tether" "$XDG_DATA_HOME/tether" "$XDG_STATE_HOME/tether" "$XDG_CACHE_HOME"
[[ -S /host/run/dbus/system_bus_socket ]] || fail 'host-bus-missing: mount the host /run/dbus directory at /host/run/dbus'
if ! timeout -k 0.5s 3s gdbus call --system --timeout 2 --dest org.freedesktop.DBus \
    --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.ListNames >/dev/null 2>&1; then
    fail 'host-bus-denied: verify host UID and D-Bus policy; do not use privileged mode'
fi
for name in org.bluez org.freedesktop.Avahi; do
    if [[ $(timeout -k 0.5s 3s gdbus call --system --timeout 2 --dest org.freedesktop.DBus \
        --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.NameHasOwner "$name" 2>/dev/null) != '(true,)' ]]; then
        printf 'tether-container: %s unavailable; repair the host service and restart if needed\n' "$name" >&2
    fi
done

bus_ready() {
    timeout -k 0.2s 1s gdbus call --session --timeout 1 --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
        --method org.freedesktop.DBus.NameHasOwner "$1" 2>/dev/null | grep -qx '(true,)'
}
wait_ready() {
    local name=$1 label=$2 deadline=$((SECONDS + 10)) pid
    while (( SECONDS < deadline )); do
        for pid in "${children[@]}"; do
            kill -0 "$pid" 2>/dev/null || fail "child-exited: $label did not become ready"
        done
        if bus_ready "$name"; then return; fi
        sleep 0.1
    done
    fail "$label-timeout: private $label did not become ready"
}
# The runtime directory is ephemeral. Do not mount a host session bus here.
dbus-daemon --session --nofork --nopidfile --address="$DBUS_SESSION_BUS_ADDRESS" &
children+=("$!")
wait_ready org.freedesktop.DBus session-bus
# Ubuntu's foreground switch is --nodetach, verified in the image build.
/usr/local/libexec/tether-obexd --nodetach &
children+=("$!")
wait_ready org.bluez.obex obex
printf 'tether-container: session bus and OBEX ready; starting tetherd\n' >&2
tetherd &
children+=("$!")
status=0
wait -n "${children[@]}" || status=$?
fail "child-exited: a supervised process exited ($status); stopping its siblings"
