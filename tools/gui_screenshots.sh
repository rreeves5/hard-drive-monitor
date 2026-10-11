#!/usr/bin/env bash
# Regenerate docs/gui-*.png: runs build-gui/hdmon-gui on the synthetic fixtures under Xvfb + openbox + stalonetray.
# Needs: xvfb, openbox, stalonetray, xdotool, imagemagick (import).
set -euo pipefail
cd "$(dirname "$0")/.."
BIN="${1:-build-gui/hdmon-gui}"
export DISPLAY=:97
Xvfb :97 -screen 0 1280x800x24 >/dev/null 2>&1 & XV=$!
trap 'kill $XV 2>/dev/null || true' EXIT
sleep 1
openbox >/dev/null 2>&1 &
sleep 1
stalonetray --geometry 4x1+1100+0 -i 24 --background '#333333' >/dev/null 2>&1 &
sleep 1
H="$(mktemp -d)"
run() { HOME="$H" XDG_CONFIG_HOME="$H/.config" "$BIN" --from-json tests/fixtures "$@" >/dev/null 2>&1 & echo $!; }
shot() { import -frame -window "$(xdotool search --sync --name "$1" | head -1)" "$2"; }
mkdir -p docs

P=$(run --show); sleep 3
W=$(xdotool search --name "Hard Drive Monitor Beta" | head -1)
shot "Hard Drive Monitor Beta" docs/gui-main.png
xdotool windowactivate --sync "$W"; xdotool key Down Down Down Down Down; sleep 0.5   # select sdb (CRITICAL)
xdotool key Return; sleep 1.5
shot "^Details" docs/gui-details.png
xdotool key Escape; sleep 0.5
xdotool mousemove 1112 12 click 3; sleep 1.5
import -window root -crop 300x200+1000+0 docs/gui-tray-menu.png
xdotool key Escape; kill $P; sleep 1

P=$(run --open options); sleep 3
shot "^Options$" docs/gui-options.png
kill $P; sleep 1
P=$(run --open about); sleep 3
shot "^About Hard Drive Monitor$" docs/gui-about.png
kill $P
rm -rf "$H"
ls -la docs/gui-*.png
