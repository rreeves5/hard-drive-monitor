#!/usr/bin/env bash
# Hard Drive Monitor installer for Linux and macOS.
# Installs the `hdmon` command and makes sure it is on PATH for new shells.
#
#   ./install.sh                install for the current user + schedule the hourly check
#   ./install.sh --no-service   do not schedule anything (run `hdmon install-service` later)
#   ./install.sh --system       also symlink hdmon into /usr/local/bin (uses sudo)
#   ./install.sh --setcap       Linux: let hdmon read SMART without sudo (setcap, asks for sudo)
#   ./install.sh --no-setcap    never ask about it
#   ./install.sh --binary FILE  install this hdmon binary
#   ./install.sh --build        always build from source with CMake
#   ./install.sh --no-gui       skip the desktop app (hdmon-gui / Hard Drive Monitor.app) even if present
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
CAPS=ask
BINARY=""
FORCE_BUILD=0
GUI=1

while [ $# -gt 0 ]; do
  case "$1" in
    --system) SYSTEM=1 ;;
    --no-service) SERVICE=0 ;;
    --setcap|--sudoers) CAPS=yes ;;        # --sudoers: old name (0.2.3 and earlier added a smartctl sudoers rule)
    --no-setcap|--no-sudoers) CAPS=no ;;
    --binary) shift; BINARY="${1:-}" ;;
    --binary=*) BINARY="${1#--binary=}" ;;
    --build) FORCE_BUILD=1 ;;
    --no-gui) GUI=0 ;;
    -h|--help) sed -n '2,18p' "$0"; exit 0 ;;
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

# Replacing the file drops any file capabilities it had (setcap is stored on the inode). Remember whether the
# installed copy had them so they can be granted again below.
has_caps() {
  [ -f "$1" ] || return 1
  if command -v getcap >/dev/null 2>&1 || [ -x /usr/sbin/getcap ] || [ -x /sbin/getcap ]; then
    gc="$(command -v getcap 2>/dev/null || true)"; [ -n "$gc" ] || gc=/usr/sbin/getcap; [ -x "$gc" ] || gc=/sbin/getcap
    "$gc" "$1" 2>/dev/null | grep -q cap_sys_rawio && return 0
    return 1
  fi
  command -v getfattr >/dev/null 2>&1 && getfattr -n security.capability "$1" >/dev/null 2>&1 && return 0
  grep -q '^caps=1' "$STATE_DIR/install-state" 2>/dev/null   # no tool to look: trust the last install
}
PREV_CAPS=0
if [ "$OS" = linux ] && { has_caps "$BIN/hdmon" || has_caps "$BIN/hdmon-gui"; }; then PREV_CAPS=1; fi

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

# --- desktop app (optional): installed when a build is present; --no-gui skips it
install_gui() {
  if [ "$OS" = macos ]; then
    local app=""
    for c in "$SRC/Hard Drive Monitor.app" "$SRC/dist/Hard Drive Monitor.app" "$SRC/build-gui/Hard Drive Monitor.app"; do
      [ -d "$c" ] && { app="$c"; break; }
    done
    if [ -z "$app" ] && [ -f "$SRC/dist/Hard-Drive-Monitor-macos.zip" ]; then
      local tmpd; tmpd="$(mktemp -d)"
      ditto -x -k "$SRC/dist/Hard-Drive-Monitor-macos.zip" "$tmpd" && app="$tmpd/Hard Drive Monitor.app"
    fi
    [ -n "$app" ] && [ -d "$app" ] || { say "No desktop app build found (Hard Drive Monitor.app); skipped."; return 0; }
    mkdir -p "$HOME/Applications"
    rm -rf "$HOME/Applications/Hard Drive Monitor.app"
    ditto "$app" "$HOME/Applications/Hard Drive Monitor.app"
    echo "gui_app=$HOME/Applications/Hard Drive Monitor.app" >> "$STATE_DIR/install-state"
    say "Installed the desktop app: ~/Applications/Hard Drive Monitor.app"
    return 0
  fi
  local g=""
  for c in "$SRC/hdmon-gui" "$SRC/dist/hdmon-gui-$OS-$ARCH" "$SRC/build-gui/hdmon-gui"; do
    if [ -f "$c" ] && "$c" --version >/dev/null 2>&1; then g="$c"; break; fi
  done
  [ -n "$g" ] || { say "No runnable desktop app build found (hdmon-gui needs GTK 3); skipped."; return 0; }
  local tmpg="$BIN/.hdmon-gui.new.$$"
  cp "$g" "$tmpg" && chmod 755 "$tmpg" && mv -f "$tmpg" "$BIN/hdmon-gui"
  local apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
  local icons="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/256x256/apps"
  mkdir -p "$apps" "$icons"
  [ -f "$SRC/gui/res/hard-drive-monitor.png" ] && cp "$SRC/gui/res/hard-drive-monitor.png" "$icons/hard-drive-monitor.png"
  sed "s|^Exec=.*|Exec=\"$BIN/hdmon-gui\" --show|" "$SRC/gui/res/hard-drive-monitor.desktop" > "$apps/hard-drive-monitor.desktop" 2>/dev/null ||
    printf '[Desktop Entry]\nType=Application\nName=Hard Drive Monitor\nExec="%s" --show\nIcon=hard-drive-monitor\nTerminal=false\nCategories=System;Monitor;\n' "$BIN/hdmon-gui" > "$apps/hard-drive-monitor.desktop"
  command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$apps" >/dev/null 2>&1 || true
  { echo "gui=$BIN/hdmon-gui"; echo "gui_desktop=$apps/hard-drive-monitor.desktop"; echo "gui_icon=$icons/hard-drive-monitor.png"; } >> "$STATE_DIR/install-state"
  say "Installed the desktop app: $BIN/hdmon-gui (menu entry: Hard Drive Monitor)"
}
if [ "$GUI" = 1 ]; then install_gui; else say "Skipped the desktop app (--no-gui)."; fi

# --- SMART access. hdmon reads SMART natively (no smartmontools needed).
# Linux: the raw ATA/NVMe commands need root or three capabilities on the binary. Granting them lets the
#        background check (which runs as you) read the drives; nothing else on the system changes.
# macOS: IOKit reads internal drives without root; nothing to set up.
CAP_SET="cap_sys_rawio,cap_sys_admin,cap_dac_read_search+ep"
if [ "$OS" = linux ]; then
  SETCAP="$(command -v setcap 2>/dev/null || true)"
  for c in /usr/sbin/setcap /sbin/setcap; do [ -z "$SETCAP" ] && [ -x "$c" ] && SETCAP="$c"; done
  targets="$BIN/hdmon"
  [ -x "$BIN/hdmon-gui" ] && targets="$targets $BIN/hdmon-gui"
  if [ "$CAPS" = ask ] && [ "$PREV_CAPS" = 1 ]; then
    say "The previous hdmon had SMART capabilities; replacing the binary removed them, so granting them again."
    CAPS=again
  fi
  if [ "$CAPS" = no ] && [ "$PREV_CAPS" = 1 ]; then
    say "WARNING: the previous hdmon had SMART capabilities and replacing it removed them (--no-setcap given)."
    say "         Background checks running as you cannot read the drives until you run:"
    echo "    sudo setcap $CAP_SET $targets"
  fi
  if [ "$CAPS" = ask ]; then
    if [ -t 0 ]; then
      echo
      echo "Reading SMART data needs root. The background check runs as you, so it can only read your"
      echo "drives if the hdmon binary has these capabilities (raw disk commands only; no sudo rule needed):"
      echo "    sudo setcap $CAP_SET $targets"
      printf "Grant them now? [y/N] "
      read -r ans || ans=""
      case "$ans" in y|Y|yes|YES) CAPS=yes ;; *) CAPS=no ;; esac
    else CAPS=no; fi
  fi
  if [ "$CAPS" = yes ] || [ "$CAPS" = again ]; then
    if [ -z "$SETCAP" ]; then
      say "setcap not found (Debian/Ubuntu: sudo apt install libcap2-bin; Fedora: libcap; Arch: libcap)."
      [ "$CAPS" = again ] && say "WARNING: SMART capabilities were NOT re-applied; background checks cannot read the drives."
    else
      ok=1
      # Re-applying without a terminal: only if sudo needs no password (never hang a scripted update).
      SUDO="sudo"; { [ "$CAPS" = again ] && [ ! -t 0 ]; } && SUDO="sudo -n"
      for t in $targets; do $SUDO "$SETCAP" "$CAP_SET" "$t" || ok=0; done
      if [ "$ok" = 1 ]; then echo "caps=1" >> "$STATE_DIR/install-state"; say "Granted SMART access to: $targets"
      else
        say "WARNING: setcap failed, so SMART capabilities are NOT set on: $targets"
        say "         (no sudo, or $BIN is on a filesystem mounted nosuid). Fix with:"
        echo "    sudo setcap $CAP_SET $targets"
      fi
    fi
  elif [ "$PREV_CAPS" = 0 ]; then
    say "No capabilities granted: run 'sudo hdmon status', or later: ./install.sh --setcap"
    echo "    (or by hand: sudo setcap $CAP_SET \"$BIN/hdmon\")"
  fi
fi
SMARTCTL="$(command -v smartctl 2>/dev/null || true)"
if [ -n "$SMARTCTL" ]; then
  say "smartmontools found ($SMARTCTL): used only as a fallback for drives the native read cannot handle."
fi

# --- updates: never automatic. Check by hand; `hdmon update --enable-auto-check` adds a daily notify-only check.
say "Updates are manual: hdmon update --check (optional daily notification: hdmon update --enable-auto-check)"

cat <<MSG

Done. Open a NEW terminal (or run:  source ~/.profile ), then try:
  hdmon status
Turn the background check off any time with:  hdmon uninstall-service
Remove everything with:                        ./uninstall.sh
MSG
