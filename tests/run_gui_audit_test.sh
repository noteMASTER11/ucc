#!/usr/bin/env bash
set -euo pipefail
test_home=$(mktemp -d)
trap 'rm -rf "$test_home"' EXIT
export HOME="$test_home" XDG_CONFIG_HOME="$test_home/.config" QT_QPA_PLATFORM=offscreen GIO_USE_VFS=local
export DBUS_SYSTEM_BUS_ADDRESS="$DBUS_SESSION_BUS_ADDRESS"
"$@"
