#!/usr/bin/env bash
# Hard Drive Monitor installer for Linux and macOS.
# Installs the `hdmon` command and makes sure it is on PATH for new shells.
#
#   ./install.sh                install for the current user + schedule the hourly check
#   ./install.sh --no-service   do not schedule anything (run `hdmon install-service` later)
#   ./install.sh --system       also symlink hdmon into /usr/local/bin (uses sudo)
#   ./install.sh --sudoers      add a sudoers rule so hdmon may run smartctl as root (asks for sudo)
#   ./install.sh --no-sudoers   never ask about the sudoers rule
#
# Order of preference: pipx -> pip install --user -> a private venv
# (~/.local/share/hard-drive-monitor/venv, used when pip refuses --user installs,
# e.g. "externally-managed-environment" on Debian 12 / Ubuntu 23.04+).
set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PKG="hard-drive-monitor"
BEGIN="# >>> hard-drive-monitor PATH (added by install.sh) >>>"
END="# <<< hard-drive-monitor PATH <<<"
STATE_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/hard-drive-monitor"
[ "$(uname -s)" = "Darwin" ] && STATE_DIR="$HOME/Library/Application Support/hard-drive-monitor"
SYSTEM=0
SERVICE=1
SUDOERS=ask

for arg in "$@"; do
  case "$arg" in
    --system) SYSTEM=1 ;;
    --no-service) SERVICE=0 ;;
    --sudoers) SUDOERS=yes ;;
    --no-sudoers) SUDOERS=no ;;
    -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

say() { printf '==> %s\n' "$*"; }

PY="${PYTHON:-}"
if [ -z "$PY" ]; then
  for c in python3 python; do
    if command -v "$c" >/dev/null 2>&1; then PY="$(command -v "$c")"; break; fi
  done
fi
[ -n "$PY" ] || { echo "Python 3.8+ is required (Linux: sudo apt install python3 | macOS: brew install python)" >&2; exit 1; }
"$PY" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)' \
  || { echo "Python 3.8+ is required, found: $("$PY" --version 2>&1)" >&2; exit 1; }

METHOD=""
BIN=""
if command -v pipx >/dev/null 2>&1; then
  say "Installing with pipx"
  pipx install --force "$SRC"
  BIN="$(pipx environment --value PIPX_BIN_DIR 2>/dev/null || true)"
  [ -n "$BIN" ] || BIN="$HOME/.local/bin"
  pipx ensurepath >/dev/null 2>&1 || true
  METHOD="pipx"
else
  say "pipx not found; trying: $PY -m pip install --user"
  if "$PY" -m pip install --user --upgrade "$SRC" 2>/tmp/hdmon-pip.$$; then
    BIN="$("$PY" -m site --user-base)/bin"
    METHOD="pip-user"
  else
    if grep -qi "externally-managed" /tmp/hdmon-pip.$$ 2>/dev/null || ! "$PY" -m pip --version >/dev/null 2>&1; then
      say "pip --user is not allowed here; using a private venv in $STATE_DIR/venv"
      mkdir -p "$STATE_DIR"
      "$PY" -m venv "$STATE_DIR/venv" || { echo "python3 -m venv failed (Debian/Ubuntu: sudo apt install python3-venv)" >&2; exit 1; }
      "$STATE_DIR/venv/bin/python" -m pip install --quiet --upgrade "$SRC"
      BIN="$HOME/.local/bin"
      mkdir -p "$BIN"
      ln -sf "$STATE_DIR/venv/bin/hdmon" "$BIN/hdmon"
      METHOD="venv"
    else
      cat /tmp/hdmon-pip.$$ >&2; rm -f /tmp/hdmon-pip.$$; exit 1
    fi
  fi
  rm -f /tmp/hdmon-pip.$$
fi

[ -x "$BIN/hdmon" ] || { echo "install finished but $BIN/hdmon was not found" >&2; exit 1; }
mkdir -p "$STATE_DIR"
printf 'method=%s\nbin=%s\n' "$METHOD" "$BIN" > "$STATE_DIR/install-state"

# PATH for new shells: an idempotent block in the usual startup files.
add_block() {
  local f="$1"
  if [ -f "$f" ] && grep -qF "$BEGIN" "$f"; then return 0; fi
  {
    printf '\n%s\n' "$BEGIN"
    printf 'case ":$PATH:" in *":%s:"*) ;; *) export PATH="%s:$PATH" ;; esac\n' "$BIN" "$BIN"
    printf '%s\n' "$END"
  } >> "$f"
  say "Added $BIN to PATH in $f"
}
add_block "$HOME/.profile"
add_block "$HOME/.bashrc"
[ -f "$HOME/.bash_profile" ] && add_block "$HOME/.bash_profile"   # bash login shells skip .profile if this exists
if [ -f "$HOME/.zshrc" ] || [ "$(uname -s)" = "Darwin" ]; then add_block "$HOME/.zshrc"; fi

if [ "$SYSTEM" = 1 ]; then
  say "Linking /usr/local/bin/hdmon -> $BIN/hdmon (sudo)"
  sudo mkdir -p /usr/local/bin
  sudo ln -sf "$BIN/hdmon" /usr/local/bin/hdmon
  echo "system_link=/usr/local/bin/hdmon" >> "$STATE_DIR/install-state"
fi

say "Installed: $("$BIN/hdmon" --version)  ($METHOD, $BIN/hdmon)"

# --- SMART needs root: optional sudoers rule for smartctl only
SMARTCTL="$(command -v smartctl 2>/dev/null || true)"
for c in /usr/sbin/smartctl /usr/local/sbin/smartctl /opt/homebrew/bin/smartctl /usr/local/bin/smartctl; do
  [ -z "$SMARTCTL" ] && [ -x "$c" ] && SMARTCTL="$c"
done
if [ -z "$SMARTCTL" ]; then
  if [ "$(uname -s)" = "Darwin" ]; then hint="brew install smartmontools"; else hint="sudo apt install smartmontools  (dnf/pacman: smartmontools)"; fi
  say "smartmontools is not installed yet: $hint"
  say "Then re-run ./install.sh (or add the sudoers rule from README.md) so the background check can read SMART."
else
  RULE="$(id -un) ALL=(root) NOPASSWD: $SMARTCTL"
  SUDOERS_FILE=/etc/sudoers.d/hard-drive-monitor
  if [ "$SUDOERS" = ask ]; then
    if [ -f "$SUDOERS_FILE" ]; then
      SUDOERS=no; echo "sudoers=$SUDOERS_FILE" >> "$STATE_DIR/install-state"
      say "sudoers rule already present ($SUDOERS_FILE)"
    elif [ -t 0 ]; then
      echo
      echo "Reading SMART data needs root. The background check runs as you, so it can only read"
      echo "your drives if it may run smartctl via sudo without a password. Optional rule:"
      echo "    $RULE"
      echo "(it allows smartctl only, with any arguments; written to $SUDOERS_FILE)"
      printf "Add this sudoers rule now? [y/N] "
      read -r ans || ans=""
      case "$ans" in y|Y|yes|YES) SUDOERS=yes ;; *) SUDOERS=no ;; esac
    else SUDOERS=no; fi
  fi
  if [ "$SUDOERS" = yes ]; then
    if ! command -v visudo >/dev/null 2>&1 && [ ! -x /usr/sbin/visudo ]; then
      say "visudo not found, so I will not write an unvalidated sudoers file. Add this line yourself"
      say "with: sudo visudo -f $SUDOERS_FILE"
      echo "    $RULE"
    else
      VISUDO="$(command -v visudo 2>/dev/null || echo /usr/sbin/visudo)"
      tmp="$(mktemp)"; printf '%s\n' "$RULE" > "$tmp"
      if sudo "$VISUDO" -cf "$tmp" >/dev/null; then
        sudo install -m 0440 -o root -g "$(id -gn root 2>/dev/null || echo 0)" "$tmp" "$SUDOERS_FILE"
        echo "sudoers=$SUDOERS_FILE" >> "$STATE_DIR/install-state"
        say "Added $SUDOERS_FILE ($RULE)"
      else
        say "sudoers rule failed validation; not installed"
      fi
      rm -f "$tmp"
    fi
  elif [ ! -f "$SUDOERS_FILE" ]; then
    if [ "$(uname -s)" = "Darwin" ]; then
      say "No sudoers rule: the internal Apple SSD usually still works; other disks need sudo. Options:"
      echo "    ./install.sh --sudoers                 (smartctl-only sudoers rule)"
      echo "    hdmon install-service --system         (prints a root LaunchDaemon you can install)"
    else
      say "No sudoers rule: the background check will show drives as UNKNOWN until you add one:"
      echo "    ./install.sh --sudoers     (or see 'smartctl needs root' in README.md)"
    fi
  fi
fi

# --- schedule the background check (on by default)
if [ "$SERVICE" = 1 ]; then
  say "Scheduling the background check (hourly + at login/boot)"
  if "$BIN/hdmon" install-service; then
    echo "service=1" >> "$STATE_DIR/install-state"
  else
    say "Could not schedule the background check automatically (see message above)."
  fi
else
  say "Skipped scheduling (--no-service). Turn it on later with: hdmon install-service"
fi

cat <<MSG

Done. Open a NEW terminal (or run:  source ~/.profile ), then try:
  hdmon status
Turn the background check off any time with:  hdmon uninstall-service
Remove everything with:                        ./uninstall.sh
MSG
