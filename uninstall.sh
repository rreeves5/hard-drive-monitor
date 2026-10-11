#!/usr/bin/env bash
# Removes Hard Drive Monitor (hdmon), its scheduled background check, the sudoers rule and
# the PATH lines install.sh added.
# History/config are kept; pass --purge to delete them too.
set -uo pipefail

BEGIN="# >>> hard-drive-monitor PATH (added by install.sh) >>>"
END="# <<< hard-drive-monitor PATH <<<"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}/hard-drive-monitor"
[ "$(uname -s)" = "Darwin" ] && DATA="$HOME/Library/Application Support/hard-drive-monitor"
CONF="${XDG_CONFIG_HOME:-$HOME/.config}/hard-drive-monitor"
PURGE=0
[ "${1:-}" = "--purge" ] && PURGE=1
say() { printf '==> %s\n' "$*"; }

METHOD=""; BIN=""; LINK=""
if [ -f "$DATA/install-state" ]; then
  METHOD="$(sed -n 's/^method=//p' "$DATA/install-state")"
  BIN="$(sed -n 's/^bin=//p' "$DATA/install-state")"
  LINK="$(sed -n 's/^system_link=//p' "$DATA/install-state")"
fi

SUDOERS_FILE="$(sed -n 's/^sudoers=//p' "$DATA/install-state" 2>/dev/null)"
HD=""; [ -n "$BIN" ] && [ -x "$BIN/hdmon" ] && HD="$BIN/hdmon"
[ -z "$HD" ] && HD="$(command -v hdmon 2>/dev/null || true)"
if [ -n "$HD" ]; then
  say "Removing the scheduled background check"; "$HD" uninstall-service || true
fi

if command -v pipx >/dev/null 2>&1 && pipx list --short 2>/dev/null | grep -q '^hard-drive-monitor '; then
  say "pipx uninstall hard-drive-monitor"; pipx uninstall hard-drive-monitor
fi
for py in python3 python; do
  if command -v "$py" >/dev/null 2>&1 && "$py" -m pip show hard-drive-monitor >/dev/null 2>&1; then
    say "$py -m pip uninstall hard-drive-monitor"
    "$py" -m pip uninstall -y hard-drive-monitor >/dev/null 2>&1 \
      || "$py" -m pip uninstall -y --break-system-packages hard-drive-monitor >/dev/null 2>&1 || true
  fi
done
if [ -d "$DATA/venv" ]; then
  say "Removing private venv $DATA/venv"; rm -rf "$DATA/venv"
fi
if [ -L "$HOME/.local/bin/hdmon" ] && [ ! -e "$HOME/.local/bin/hdmon" ]; then rm -f "$HOME/.local/bin/hdmon"; fi
if [ -n "$LINK" ] && [ -L "$LINK" ]; then
  say "Removing $LINK (sudo)"; sudo rm -f "$LINK"
fi

for f in "$HOME/.profile" "$HOME/.bashrc" "$HOME/.bash_profile" "$HOME/.zshrc"; do
  if [ -f "$f" ] && grep -qF "$BEGIN" "$f"; then
    tmp="$(mktemp)"
    awk -v b="$BEGIN" -v e="$END" '$0==b{skip=1; next} skip&&$0==e{skip=0; next} !skip' "$f" > "$tmp" && cat "$tmp" > "$f"
    rm -f "$tmp"
    say "Removed PATH block from $f"
  fi
done
if [ -n "$SUDOERS_FILE" ] && [ -f "$SUDOERS_FILE" ]; then
  say "Removing sudoers rule $SUDOERS_FILE (sudo)"; sudo rm -f "$SUDOERS_FILE"
fi
rm -f "$DATA/install-state"

if [ "$PURGE" = 1 ]; then
  say "Deleting history and config: $DATA $CONF"; rm -rf "$DATA" "$CONF"
else
  echo "Kept history ($DATA) and config ($CONF). Use --purge to delete them."
fi
echo "Open a new terminal for the PATH change to take effect."
