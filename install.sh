#!/usr/bin/env bash
# Hard Drive Monitor installer for Linux and macOS.
# Installs the `hdmon` command and makes sure it is on PATH for new shells.
#
#   ./install.sh                install for the current user + schedule the hourly check
#   ./install.sh --no-service   do not schedule anything (run `hdmon install-service` later)
#   ./install.sh --system       also symlink hdmon into /usr/local/bin (uses sudo)
#   ./install.sh --sudoers      add a sudoers rule so hdmon may run smartctl as root (asks for sudo)
#   ./install.sh --no-sudoers   never ask about the sudoers rule
#   ./install.sh --binary FILE  install this hdmon binary
#   ./install.sh --build        always build from source with CMake
#   ./install.sh --enable-auto-update  let the scheduled check look for new releases once a day
#                               (notify only). Off by default; same as: hdmon update --enable-auto-check
#
# Uses a prebuilt binary when one matches this machine (in order: --binary FILE, ./hdmon next to
# this script, dist/hdmon-<os>-<arch>, build/hdmon); otherwise builds from source with CMake
# (needs cmake 3.16+ and a C++17 compiler). The binary is copied to ~/.local/bin/hdmon.
set -euo pipefail

SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BEGIN="# >>> hard-drive-monitor PATH (added by install.sh) >>>"
END="# <<< hard-drive-monitor PATH <<<"
STATE_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/hard-drive-monitor"
[ "$(uname -s)" = "Darwin" ] && STATE_DIR="$HOME/Library/Application Support/hard-drive-monitor"
BIN="$HOME/.local/bin"
SYSTEM=0
SERVICE=1
SUDOERS=ask
BINARY=""
FORCE_BUILD=0
AUTO_UPDATE=0

while [ $# -gt 0 ]; do
  case "$1" in
    --system) SYSTEM=1 ;;
    --no-service) SERVICE=0 ;;
    --sudoers) SUDOERS=yes ;;
    --no-sudoers) SUDOERS=no ;;
    --binary) shift; BINARY="${1:-}" ;;
    --binary=*) BINARY="${1#--binary=}" ;;
    --build) FORCE_BUILD=1 ;;
    --enable-auto-update) AUTO_UPDATE=1 ;;
    -h|--help) sed -n '2,17p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

say() { printf '==> %s\n' "$*"; }

case "$(uname -s)" in
  Linux) OS=linux ;;
  Darwin) OS=macos ;;
  *) echo "install.sh supports Linux and macOS; on Windows use install.ps1" >&2; exit 1 ;;
esac
case "$(uname -m)" in
  x86_64|amd64) ARCH=x86_64 ;;
  arm64|aarch64) ARCH=arm64 ;;
  *) ARCH="$(uname -m)" ;;
esac

runs() { [ -f "$1" ] && "$1" --version >/dev/null 2>&1; }

build_from_source() {
  command -v cmake >/dev/null 2>&1 || { echo "No prebuilt hdmon for $OS/$ARCH and cmake is not installed." >&2
    echo "Install cmake + a C++17 compiler (Linux: sudo apt install cmake g++ | macOS: xcode-select --install; brew install cmake)" >&2
    exit 1; }
  local bdir; bdir="$(mktemp -d)"
  say "Building hdmon from source with CMake (in $bdir)"
  cmake -S "$SRC" -B "$bdir" -DCMAKE_BUILD_TYPE=Release -DHDMON_BUILD_TESTS=OFF >/dev/null
  cmake --build "$bdir" --config Release --parallel >/dev/null
  CANDIDATE="$bdir/hdmon"
  [ -x "$CANDIDATE" ] || CANDIDATE="$bdir/Release/hdmon"
  BUILD_DIR="$bdir"
}

CANDIDATE=""
BUILD_DIR=""
if [ "$FORCE_BUILD" = 0 ]; then
  for c in "$BINARY" "$SRC/hdmon" "$SRC/dist/hdmon-$OS-$ARCH" "$SRC/dist/hdmon-$OS-universal" "$SRC/build/hdmon"; do
    [ -n "$c" ] || continue
    if runs "$c"; then CANDIDATE="$c"; break; fi
    [ "$c" = "$BINARY" ] && { echo "--binary $BINARY does not run on this machine" >&2; exit 1; }
  done
fi
if [ -z "$CANDIDATE" ]; then build_from_source; else say "Using prebuilt binary $CANDIDATE"; fi

mkdir -p "$BIN"
tmpbin="$BIN/.hdmon.new.$$"
cp "$CANDIDATE" "$tmpbin" && chmod 755 "$tmpbin" && mv -f "$tmpbin" "$BIN/hdmon"   # atomic replace, safe while running
[ -n "$BUILD_DIR" ] && rm -rf "$BUILD_DIR"
METHOD="binary"
[ -n "$BUILD_DIR" ] && METHOD="built"

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

# --- automatic update checks: OFF unless --enable-auto-update was given (never turned on silently)
if [ "$AUTO_UPDATE" = 1 ]; then
  "$BIN/hdmon" update --enable-auto-check || say "Could not enable automatic update checks."
else
  say "Automatic update checks are off (default). Check by hand with: hdmon update --check"
fi

cat <<MSG

Done. Open a NEW terminal (or run:  source ~/.profile ), then try:
  hdmon status
Turn the background check off any time with:  hdmon uninstall-service
Remove everything with:                        ./uninstall.sh
MSG
