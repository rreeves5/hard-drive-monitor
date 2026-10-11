"""Per-OS file locations.

  Linux:   history ~/.local/share/hard-drive-monitor/history.db      config ~/.config/hard-drive-monitor/config.toml
  macOS:   history ~/Library/Application Support/hard-drive-monitor   config ~/.config/hard-drive-monitor/config.toml
  Windows: history %LOCALAPPDATA%\\hard-drive-monitor\\history.db     config %APPDATA%\\hard-drive-monitor\\config.toml
"""
import os
import sys
from pathlib import Path

IS_MAC = sys.platform == "darwin"
IS_WINDOWS = sys.platform.startswith("win")
IS_LINUX = sys.platform.startswith("linux")


def os_name(platform=None) -> str:
    p = platform or sys.platform
    if p == "darwin":
        return "macos"
    if p.startswith("win"):
        return "windows"
    return "linux"


def data_dir() -> Path:
    if IS_WINDOWS:
        base = os.environ.get("LOCALAPPDATA") or str(Path.home() / "AppData" / "Local")
        return Path(base) / "hard-drive-monitor"
    if IS_MAC:
        return Path.home() / "Library" / "Application Support" / "hard-drive-monitor"
    base = os.environ.get("XDG_DATA_HOME") or str(Path.home() / ".local" / "share")
    return Path(base) / "hard-drive-monitor"


def default_db() -> Path:
    return data_dir() / "history.db"


def config_path() -> Path:
    if IS_WINDOWS:
        base = os.environ.get("APPDATA") or str(Path.home() / "AppData" / "Roaming")
        return Path(base) / "hard-drive-monitor" / "config.toml"
    base = os.environ.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")
    return Path(base) / "hard-drive-monitor" / "config.toml"
