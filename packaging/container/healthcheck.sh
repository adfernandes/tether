#!/usr/bin/env bash
# Bound the entire probe, not each read independently. Never activate a service.
# Variables must expand in the bounded child shell, not the outer wrapper.
# shellcheck disable=SC2016
exec timeout --kill-after=0.5s 4s /bin/bash -ec '
    [[ ${DBUS_SESSION_BUS_ADDRESS:-} == unix:path=/run/tether-runtime/bus ]]
    [[ $(gdbus call --session --timeout 1 --dest org.freedesktop.DBus \
        --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.NameHasOwner org.bluez.obex 2>/dev/null) == "(true,)" ]]
    TETHER_NO_AUTOSTART=1 /usr/local/bin/tether status >/dev/null 2>&1
'
