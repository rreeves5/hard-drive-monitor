#!/bin/sh
# Live check of hdmon's raw-socket D-Bus client against a private dbus-daemon and libdbus-based fake services
# (no desktop or systemd needed). Usage: tools/dbus-verify/verify.sh path/to/hdmon
# Needs: dbus-daemon, a C compiler, libdbus-1-dev. Output: the calls the fake services received, with arguments.
set -eu
HDMON=$(realpath "$1")
HERE=$(cd "$(dirname "$0")" && pwd)
W=$(mktemp -d)
cc -O1 -o "$W/fake" "$HERE/fake_services.c" $(pkg-config --cflags --libs dbus-1)
cat > "$W/bus.conf" <<C
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig><type>session</type><listen>unix:path=$W/bus</listen><auth>EXTERNAL</auth>
<policy context="default"><allow user="*"/><allow send_destination="*" eavesdrop="true"/><allow eavesdrop="true"/><allow own="*"/></policy></busconfig>
C
PID=$(dbus-daemon --config-file="$W/bus.conf" --fork --print-pid)
trap 'kill $PID 2>/dev/null; kill $FAKE 2>/dev/null; rm -rf "$W"' EXIT
export DBUS_SESSION_BUS_ADDRESS="unix:path=$W/bus" HOME="$W/home" XDG_CONFIG_HOME="$W/home/.config" XDG_RUNTIME_DIR="$W/rt"
"$W/fake" > "$W/calls.log" 2>&1 & FAKE=$!
sleep 0.5; grep -q READY "$W/calls.log" || { echo "fake services failed:"; cat "$W/calls.log"; exit 1; }
echo "== hdmon alert-test";        "$HDMON" alert-test 2>&1 | grep -i desktop || true
echo "== hdmon install-service";   "$HDMON" install-service 2>&1 | grep -i "d-bus\|systemctl" | head -3
echo "== hdmon uninstall-service"; "$HDMON" uninstall-service 2>&1 | grep -i "d-bus\|systemctl" | head -3
echo "== calls received by the fake services"; cat "$W/calls.log"
