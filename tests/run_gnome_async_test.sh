#!/usr/bin/env bash
set -euo pipefail
# dbus-run-session supplies a private bus; never contact the running daemon.
export DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS"
export GIO_USE_VFS=local
exec "$1" -m "$(dirname "$0")/test_gnome_async.js"
