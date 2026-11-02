#!/bin/sh
set -eu

if [ "$(id -u)" = "0" ]; then
    PUID="${PUID:-1000}"
    PGID="${PGID:-1000}"
    case "$PUID" in ''|*[!0-9]*) echo "PUID must be numeric" >&2; exit 1 ;; esac
    case "$PGID" in ''|*[!0-9]*) echo "PGID must be numeric" >&2; exit 1 ;; esac
    if [ "$PUID" -eq 0 ] || [ "$PGID" -eq 0 ]; then
        echo "MapleStore must run with a non-root UID and GID" >&2
        exit 1
    fi
    mkdir -p /data /config
    chown "$PUID:$PGID" /data /config
    exec su-exec "$PUID:$PGID" /usr/local/bin/maplestore "$@"
fi

exec /usr/local/bin/maplestore "$@"
