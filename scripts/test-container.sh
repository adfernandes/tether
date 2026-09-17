#!/usr/bin/env bash
# Only disposable Docker volumes and a private bus: never use the host's BlueZ/Avahi.
set -euo pipefail
image=${1:?Usage: test-container.sh RUNTIME_IMAGE TEST_IMAGE}
test_image=${2:?Supply the Dockerfile test-stage image for the encrypted-store fixture}
prefix="tether-smoke-$$-${RANDOM}"
app="$prefix-app"
bus="$prefix-bus"
volumes=("$prefix-data" "$prefix-downloads" "$prefix-bus" "$prefix-fault")
cleanup() {
    local status=$?
    if (( status != 0 )); then
        docker logs --tail 100 "$app" >&2 || true
        docker logs --tail 30 "$bus" >&2 || true
    fi
    docker rm -f "$app" "$bus" "$prefix-init" >/dev/null 2>&1 || true
    for volume in "${volumes[@]}"; do docker volume rm "$volume" >/dev/null 2>&1 || true; done
}
trap cleanup EXIT
for volume in "${volumes[@]}"; do docker volume create "$volume" >/dev/null; done

# Root is used only to initialize these disposable test volumes, never to run Tether.
docker run --rm --name "$prefix-init" --network none --user 0:0 \
    -v "$prefix-data:/data" -v "$prefix-downloads:/downloads" -v "$prefix-bus:/bus" \
    --entrypoint /bin/sh "$image" -ec 'chown 1000:1000 /data /downloads /bus; chmod 700 /data /downloads /bus'
# An external OBEX executable that never claims its bus name exercises readiness timeout.
# This override exists only in a disposable test volume, never in the runtime image.
docker run --rm --network none --user 0:0 -v "$prefix-fault:/fault" \
    --entrypoint /bin/sh "$image" -ec '
        cp -L /usr/local/libexec/tether-* /fault/
        printf "#!/bin/sh\nexec sleep 30\n" > /fault/tether-obexd
        chmod 755 /fault/tether-obexd
    '
docker run -d --name "$bus" --network none --user 1000:1000 --read-only \
    --cap-drop ALL --security-opt no-new-privileges:true -e XDG_RUNTIME_DIR=/bus -v "$prefix-bus:/bus" \
    --entrypoint /usr/bin/dbus-daemon "$image" \
    --session --nofork --nopidfile --address=unix:path=/bus/system_bus_socket >/dev/null

start() {
    docker rm -f "$app" >/dev/null 2>&1 || true
    docker run -d --name "$app" --network none --read-only --user 1000:1000 \
        --cap-drop ALL --security-opt no-new-privileges:true \
        --tmpfs /run/tether-runtime:uid=1000,gid=1000,mode=0700 \
        --tmpfs /tmp:mode=1777 \
        -v "$prefix-data:/data" -v "$prefix-downloads:/downloads" \
        -v "$prefix-bus:/host/run/dbus:ro" "$@" "$image" >/dev/null
}
ready() {
    local i
    for ((i=0; i<100; i++)); do
        [[ $(docker inspect -f '{{.State.Running}}' "$app") == true ]] || return 1
        if docker exec "$app" /usr/local/libexec/tether-container-healthcheck; then return 0; fi
        sleep 0.2
    done
    echo 'runtime never became healthy' >&2
    return 1
}
exited() {
    local i
    for ((i=0; i<80; i++)); do
        [[ $(docker inspect -f '{{.State.Running}}' "$app") == false ]] && return 0
        sleep 0.2
    done
    echo 'runtime did not terminate within its shutdown budget' >&2
    return 1
}
signal_child() {
    docker exec "$app" bash -ec '
        for comm in /proc/[0-9]*/comm; do
            read -r name 2>/dev/null < "$comm" || continue
            if [[ $name == "$1" ]]; then
                pid=${comm#/proc/}; pid=${pid%/comm}
                kill -"$2" "$pid"
                exit 0
            fi
        done
        exit 1
    ' _ "$1" "$2"
}
store_fixture() {
    docker run --rm --network none --read-only --user 1000:1000 \
        -e HOME=/data -e XDG_CONFIG_HOME=/data/.config -e XDG_DATA_HOME=/data/.local/share \
        -e DBUS_SESSION_BUS_ADDRESS=unix:path=/no-session-bus \
        -v "$prefix-data:/data" --entrypoint /build/test/tether_container_store_fixture "$test_image" "$1"
}

# Give the fixture bus a bounded startup window, without touching any host bus.
for ((i=0; i<50; i++)); do
    docker exec "$bus" test -S /bus/system_bus_socket && break
    sleep 0.1
done
start
ready
printf 'PASS: non-root runtime is healthy without a phone, BlueZ or Avahi\n'
docker exec "$app" bash -ec '
    [[ $(id -u) == 1000 ]]
    [[ $TETHER_NO_AUTOSTART == 1 && $TETHER_LOG_STDERR == 1 ]]
    test ! -e /usr/local/bin/tether-gtk
    test ! -e /usr/local/bin/tether-dialog
    test ! -e /usr/share/dbus-1/services/org.bluez.obex.service
    test -f /usr/local/share/locale/fr/LC_MESSAGES/tether.mo
    ! command -v g++
    ! command -v npm
    ! ldd /usr/local/bin/tether /usr/local/bin/tetherd | grep "not found"
    test ! -e /data/.local/state/tether/tetherd.log
    [[ $(stat -c %a /data) == 700 ]]
    [[ $(stat -c %a /run/tether-runtime) == 700 ]]
'
docker logs "$app" 2>&1 | grep 'tetherd version' >/dev/null
# No connected remote peer; this only creates synthetic local trust in this disposable home.
docker exec "$app" tether accept container-smoke-fingerprint >/dev/null
identity=$(docker exec "$app" sha256sum /data/.config/tether/cert.pem /data/.config/tether/key.pem /data/.config/tether/known_hosts.json)
docker exec "$app" sh -c 'umask 077; printf "persistent file\n" > /downloads/smoke.txt; touch /run/tether-runtime/staging-sentinel'
# Populate the journal while the daemon is stopped; never race a live journal writer.
docker stop -t 15 "$app" >/dev/null
[[ $(docker inspect -f '{{.State.ExitCode}}' "$app") == 0 ]]
store_fixture seed
start
ready
[[ $(docker exec "$app" sha256sum /data/.config/tether/cert.pem /data/.config/tether/key.pem /data/.config/tether/known_hosts.json) == "$identity" ]]
docker exec "$app" sh -ec 'test ! -e /run/tether-runtime/staging-sentinel; test "$(cat /downloads/smoke.txt)" = "persistent file"; test "$(stat -c %a /data/.config/tether/store.key)" = 600'
docker stop -t 15 "$app" >/dev/null
store_fixture check
printf 'PASS: identity, trust, nonempty encrypted history and files survive recreation\n'

# A stuck process fails passive health within budget, without spawning replacements.
start
ready
for name in tetherd dbus-daemon; do
    signal_child "$name" STOP
    before=$SECONDS
    if docker exec "$app" /usr/local/libexec/tether-container-healthcheck; then
        echo "health unexpectedly passed with $name stopped" >&2; exit 1
    fi
    (( SECONDS - before <= 6 ))
    [[ $(docker inspect -f '{{.State.Running}}' "$app") == true ]]
    signal_child "$name" CONT
    ready
done
printf 'PASS: stuck daemon/bus fail bounded health without restarting the runtime\n'

# Each critical child is owned by this test container. Killing it must stop its siblings.
for name in tetherd tether-obexd dbus-daemon; do
    start
    ready
    signal_child "$name" KILL
    exited
    [[ $(docker inspect -f '{{.State.ExitCode}}' "$app") != 0 ]]
    printf 'PASS: %s failure terminates the runtime\n' "$name"
done

start
ready
docker kill --signal INT "$app" >/dev/null
exited
[[ $(docker inspect -f '{{.State.ExitCode}}' "$app") == 0 ]]
# A child that stays alive without becoming ready must not keep the runtime starting forever.
before=$SECONDS
start -v "$prefix-fault:/usr/local/libexec:ro"
exited
(( SECONDS - before <= 15 ))
[[ $(docker inspect -f '{{.State.ExitCode}}' "$app") != 0 ]]
docker logs "$app" 2>&1 | grep 'obex-timeout' >/dev/null
printf 'PASS: SIGINT shuts down cleanly; OBEX startup timeout stops the runtime\n'

start --user 0:0
exited
[[ $(docker inspect -f '{{.State.ExitCode}}' "$app") != 0 ]]
docker logs "$app" 2>&1 | grep 'root-user' >/dev/null
start -e DBUS_SYSTEM_BUS_ADDRESS=unix:path=/wrong-bus
exited
[[ $(docker inspect -f '{{.State.ExitCode}}' "$app") != 0 ]]
docker logs "$app" 2>&1 | grep 'invalid-layout' >/dev/null
# An unavailable host bus is an actionable startup failure, not a privileged fallback.
docker stop "$bus" >/dev/null
start
exited
[[ $(docker inspect -f '{{.State.ExitCode}}' "$app") != 0 ]]
docker logs "$app" 2>&1 | grep -E 'host-bus-(missing|denied)' >/dev/null
# Misowned persistent data is refused before bus checks, not recursively chowned.
docker run --rm --network none --user 0:0 -v "$prefix-data:/data" --entrypoint /bin/sh "$image" -c 'chown 0:0 /data'
start
exited
[[ $(docker inspect -f '{{.State.ExitCode}}' "$app") != 0 ]]
docker logs "$app" 2>&1 | grep 'wrong-owner' >/dev/null
printf 'PASS: unsafe users, layout overrides and misowned volumes are refused\n'
