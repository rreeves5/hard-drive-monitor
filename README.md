# Hard Drive Monitor

**Hard Drive Monitor** (`hdmon`) is a small command-line health monitor for hard drives, SATA SSDs and NVMe
drives on **Linux, macOS and Windows**, in the spirit of Hard Disk Sentinel. It reads SMART data with
[smartmontools](https://www.smartmontools.org/) (`smartctl --json`, version 7 or newer), turns it into a
**Health %**, a **Performance %** and an **OK / WARNING / CRITICAL** status with plain-English reasons, keeps a
local history, and alerts you when something gets worse.

## Quick install

Run from a clone or the unzipped folder. Each installer puts `hdmon` on your PATH (works in any new
terminal) **and turns on the automatic background check** (hourly + at login/boot):

| OS | One line |
|---|---|
| Linux | `./install.sh` |
| macOS | `./install.sh` |
| Windows | `powershell -ExecutionPolicy Bypass -File .\install.ps1` (in an **elevated** PowerShell for full SMART access) |

Opt out of the background check with `./install.sh --no-service` or `install.ps1 -NoService`. Undo
everything with `./uninstall.sh` or `.\uninstall.ps1`. You also need smartmontools (`sudo apt install
smartmontools`, `brew install smartmontools`, or `winget install smartmontools.smartmontools`).

![hdmon status (synthetic demo data)](demo-status.png)

*The screenshot uses the synthetic test fixtures in `tests/fixtures/`, not real drives.*

> **Disclaimer.** Hard Drive Monitor is an **early-warning tool, not a guarantee**. Many drives die suddenly
> with perfect SMART data, and a WARNING drive can keep going for years. A good score does not mean
> your data is safe. **Keep backups.** The Health and Performance numbers are heuristics, not
> manufacturer figures or benchmarks.

## Features

- Drive discovery
  - Linux: `smartctl --scan-open --json` plus `lsblk` (so disks smartctl skips still show up)
  - macOS: `smartctl --scan` plus `diskutil list` (physical disks only)
  - Windows: `smartctl --scan`; **if smartctl is not installed**, falls back to PowerShell
    `Get-PhysicalDisk` + `Get-StorageReliabilityCounter` (wear, temperature, read/write errors, power-on hours).
    Drive letters are mapped to physical disks with `Get-Partition` / `Get-Disk` (matched by serial number,
    then by smartctl's `/dev/sdX` = `PhysicalDriveN` naming), so you see e.g. `/dev/sdb (D:)`.
- SATA/ATA HDDs and SSDs, and NVMe. Shows model, serial, capacity, type, temperature, power-on hours, power cycles and:
  - ATA: reallocated sectors (5), reported uncorrectable (187), command timeout (188), pending sectors (197),
    offline uncorrectable (198), UDMA CRC errors (199), SSD wear (device statistics or attributes 231/233/202/169/177)
  - NVMe: `percentage_used`, `media_errors`, `critical_warning`, `available_spare`, `unsafe_shutdowns`,
    error-log entries, time spent above warning temperature
- Commands (all accept `--json`):

| Command | What it does |
|---|---|
| `hdmon status` | Colored table of all drives, plus reasons for anything not OK. Records the run to history. |
| `hdmon detail <dev>` | Full attribute view: every ATA attribute, the NVMe health log, self-test log. `<dev>` can be `/dev/sda`, `sda`, `nvme0`, `disk0`, a serial number, or a Windows drive letter like `D:`. |
| `hdmon history` | Health/temperature sparklines from the SQLite log and every counter that got worse. |
| `hdmon check` | One scheduled-style run: record, alert on worsening, exit code 0/1/2 = OK/WARNING/CRITICAL (3 = error). |
| `hdmon watch --interval 1h` | Loop mode (`check` every interval; `--count N` to stop). |
| `hdmon selftest <dev> [short\|long]` | Starts a SMART self-test and prints the expected duration. `--results` shows the self-test log. |
| `hdmon install-service` / `uninstall-service` | Hourly schedule: systemd user timer / launchd agent / Task Scheduler. |
| `hdmon serve` | Minimal local web dashboard on `http://127.0.0.1:8765` (never binds to other interfaces). |
| `hdmon init-config` | Writes a commented sample config. |
| `hdmon alert-test` | Sends a test notification (and e-mail, if enabled). |

- `--from-json FILE_OR_DIR` (status, detail, check, watch, selftest --results, serve): read saved
  `smartctl -x --json` output instead of live drives. Useful for demos, for checking a drive from another
  machine, or for bug reports. A `--from-json` run is **not** written to history unless you add `--record`.

## Install

You need Python 3.8+ and smartmontools 7.0+.

### What the installers do

`install.sh` (Linux, macOS):

1. Installs the package with **pipx** if you have it, otherwise `pip install --user`, otherwise (when pip
   refuses, e.g. "externally-managed-environment" on Debian 12 / Ubuntu 23.04+) into a private venv at
   `~/.local/share/hard-drive-monitor/venv` with `hdmon` linked into `~/.local/bin`.
2. Puts the scripts folder on PATH: runs `pipx ensurepath` (pipx case) and adds a marked block to
   `~/.profile`, `~/.bashrc`, `~/.bash_profile` (if it exists) and `~/.zshrc` (if it exists, always on macOS).
   Running it again does not add duplicates.
3. Offers a **smartctl-only sudoers rule** (asks interactively; `--sudoers` / `--no-sudoers` to decide up front).
   It is validated with `visudo` before it is installed, and never written without `visudo`.
4. Runs `hdmon install-service` to schedule the background check (skip with `--no-service`).
5. `--system` also symlinks `hdmon` into `/usr/local/bin` (uses sudo).

`install.ps1` (Windows):

1. Installs with **pipx** if present, otherwise `py -m pip install --user`.
2. Adds the Python user Scripts folder (or the pipx bin folder) to the **User** `Path` with
   `[Environment]::SetEnvironmentVariable('Path', ..., 'User')`; `-System` uses the **Machine** `Path` instead
   (needs admin). Idempotent, and it broadcasts the change (`WM_SETTINGCHANGE`) so new windows see it.
   If no `hdmon.exe` was created it drops an `hdmon.cmd` shim so `hdmon` works in cmd.exe too.
3. Runs `hdmon install-service`: a Task Scheduler task at logon + hourly. When the script runs **elevated**
   the task gets **highest privileges**, so smartctl can read the drives (skip with `-NoService`).

After installing, **open a new terminal / Command Prompt / PowerShell** and run `hdmon status`.

`uninstall.sh` / `uninstall.ps1` remove the scheduled check, the package, the PATH entries the installer
added, the shim and the sudoers rule (if the installer created them). History and config are kept unless you
pass `--purge` / `-Purge`.

### Manual install (alternative)

#### Linux

```bash
sudo apt install smartmontools pipx          # Fedora: sudo dnf install smartmontools pipx
                                              # Arch:   sudo pacman -S smartmontools python-pipx
pipx ensurepath                               # then open a new terminal
pipx install ./hard-drive-monitor             # from a clone or the unzipped folder
sudo ~/.local/bin/hdmon status --no-record    # quick look (smartctl needs root, see below)
```

Note: on Debian/Ubuntu `smartctl` lives in `/usr/sbin`, which is not on a normal user's PATH. Hard Drive Monitor
finds it there anyway.

#### macOS

```bash
brew install smartmontools pipx
pipx ensurepath                               # then open a new terminal
pipx install ./hard-drive-monitor
hdmon status                                  # internal Apple SSDs often work without sudo
```

External USB drives on macOS often do not pass SMART data through at all (that is the USB bridge, not this
tool); they show up as `UNKNOWN`.

#### Windows

```powershell
winget install smartmontools.smartmontools    # or: choco install smartmontools
winget install Python.Python.3.12
py -m pip install --user pipx; py -m pipx ensurepath   # then open a new terminal
pipx install .\hard-drive-monitor
# In an elevated terminal (Run as Administrator):
hdmon status
```

**Administrator rights are needed** for smartctl to open physical drives on Windows. Without admin (or
without smartmontools), Hard Drive Monitor falls back to the Windows Storage counters, which give fewer
attributes and sometimes none at all for USB drives.

## smartctl needs root (sudo notes)

Reading SMART data needs raw device access, so on Linux (and for most non-Apple disks on macOS) `smartctl`
needs root. You have three options:

1. **Run it with sudo** for one-off checks: `sudo ~/.local/bin/hdmon status --no-record`. (Under sudo the history
   would go to root's home directory, hence `--no-record`.)
2. **Recommended for the background check: a sudoers rule just for smartctl** (`./install.sh` offers to add
   it; `./install.sh --sudoers` adds it without asking). Hard Drive Monitor then runs
   as you and calls `sudo -n smartctl` only when smartctl reports a permission error (`sudo = "auto"`, the default):

   ```bash
   # Linux (path from `command -v smartctl` or /usr/sbin/smartctl)
   echo "$USER ALL=(root) NOPASSWD: /usr/sbin/smartctl" | sudo tee /etc/sudoers.d/hard-drive-monitor
   sudo chmod 440 /etc/sudoers.d/hard-drive-monitor && sudo visudo -c
   # macOS: use /opt/homebrew/bin/smartctl (Apple Silicon) or /usr/local/bin/smartctl (Intel)
   ```

   This lets your account run smartctl as root without a password, including commands that change SMART
   settings or start self-tests. That is low risk, but it is a privilege, so decide consciously.
3. **Run the service as root** (`hdmon install-service --system` prints a root systemd unit / LaunchDaemon and
   the exact commands to install it; it never installs root services by itself). A root service keeps its
   history under root's home and cannot show desktop notifications in your session, so use e-mail alerts.

On macOS the background check runs as you: the internal Apple SSD usually works without root, other disks
need the sudoers rule (option 2) or a root LaunchDaemon (option 3, `hdmon install-service --system`).

On Windows, run `install.ps1` (or `hdmon install-service`) from an **elevated** PowerShell so the scheduled
task gets highest privileges. From a normal window the task runs without admin and falls back to the
Windows Storage counters.

## Automatic background check (on by default)

The installers schedule `hdmon check --quiet` to run **every hour and shortly after login/boot**. Each run
records to the history database and alerts you only when something got worse.

| OS | What gets scheduled | Check it | Turn it off |
|---|---|---|---|
| Linux (systemd) | User timer `~/.config/systemd/user/hard-drive-monitor.{service,timer}`: 2 min after your session starts, then hourly (`Persistent=true` catches up after sleep). | `systemctl --user list-timers hard-drive-monitor.timer` | `hdmon uninstall-service` |
| Linux (no systemd user session, e.g. WSL/containers) | Two lines in your **crontab** marked `# hard-drive-monitor`: `@reboot` (+2 min) and hourly. | `crontab -l` | `hdmon uninstall-service` |
| macOS | launchd agent `~/Library/LaunchAgents/com.hard-drive-monitor.check.plist`: `RunAtLoad` (at login) + `StartInterval` 3600. Log: `~/Library/Logs/hard-drive-monitor.log`. | `launchctl print gui/$UID/com.hard-drive-monitor.check` | `hdmon uninstall-service` |
| Windows | Task Scheduler task `hard-drive-monitor`: at logon (+2 min) and every hour, `pythonw -m hard_drive_monitor check --quiet` (no console window). Highest privileges when installed from an elevated window. | `schtasks /Query /TN hard-drive-monitor` | `hdmon uninstall-service` |

`hdmon install-service` prints everything it writes. Options: `--interval 30m`, `--dry-run` (print only),
`--no-enable` (write files, don't enable), `--highest` (Windows: force highest privileges), `--system`
(print a root systemd unit / root LaunchDaemon and the commands to install it; root services are never
installed silently).

Notes:

- Linux user timers only run while you are logged in. To keep them running without a login:
  `sudo loginctl enable-linger $USER`.
- The background check runs **as you**, so it can only read SMART data if smartctl may run as root; see the
  next section. Without that, drives show as `UNKNOWN` (macOS internal Apple SSDs usually work anyway).

## Alerts

Alerts fire **only when something gets worse** compared with the previous recorded run: the status worsens
(OK to WARNING, WARNING to CRITICAL), or one of these counters increases: reallocated, pending, offline
uncorrectable, reported uncorrectable, command timeouts, UDMA CRC errors, NVMe media errors, Windows
uncorrected errors. A drive that has had 12 reallocated sectors for a year does not alert you every hour; it
alerts again the moment it becomes 13. Wear and unsafe-shutdown counts rise normally, so they are shown as
trends in `hdmon history` but do not alert on their own.

- Desktop: `notify-send` (Linux), `osascript` (macOS), PowerShell toast on Windows (BurntToast module if
  installed, otherwise a tray balloon). If none is available, it says so and skips.
- E-mail: SMTP settings in the config file.

## Configuration

`hdmon init-config` writes a commented sample (file mode 600):

- Linux/macOS: `~/.config/hard-drive-monitor/config.toml`
- Windows: `%APPDATA%\hard-drive-monitor\config.toml`

```toml
[smartctl]
sudo = "auto"            # "auto" | true | false
# path = "/usr/sbin/smartctl"

[alerts]
desktop = true

[email]
enabled = true
host = "smtp.gmail.com"
port = 587
starttls = true
username = "you@example.com"
password_env = "HDMON_SMTP_PASSWORD"   # read the password from this environment variable
from = "you@example.com"
to = ["you@example.com"]

[thresholds]             # temperatures in C
hdd_temp_warn = 50
hdd_temp_crit = 55

[ignore]
devices = ["/dev/sdz"]
```

On Python 3.11+ the file is parsed with `tomllib`; on older Pythons with `tomli` if installed, otherwise with a
small built-in parser that handles the simple `key = value` format above.

History database (SQLite):

- Linux: `~/.local/share/hard-drive-monitor/history.db`
- macOS: `~/Library/Application Support/hard-drive-monitor/history.db`
- Windows: `%LOCALAPPDATA%\hard-drive-monitor\history.db`

## How the scores work

### Health %

Health starts at **100**. Each defect removes a **percentage of what is left** (so the score drops quickly for
the first few bad sectors and then flattens out, like Hard Disk Sentinel). This is the same model as the author's
Windows DiskMonitor tool, so the numbers line up.

| Defect | Each one keeps | i.e. roughly |
|---|---|---|
| Reallocated sector (ATA 5) | 99.0% | -1% each |
| Pending sector (197) | 97.0% | -3% each |
| Offline uncorrectable (198) | 97.0% | -3% each |
| Reported uncorrectable (187) | 98.0% | -2% each |
| Command timeout (188) | 99.5% | -0.5% each |
| NVMe media error | 97.0% | -3% each |
| Windows uncorrected read/write error (fallback mode) | 98.0% | -2% each |
| SSD/NVMe wear | multiply by (100 - % used) / 100 | 62% used leaves 38% |

Then hard caps apply: drive's own SMART verdict FAILED (or Windows "Unhealthy"): at most **5%**; an attribute
below its failure threshold right now: at most **10%**; NVMe spare below its threshold: at most **10%**; NVMe
critical warning (spare, reliability, read-only, backup failure): at most **20%**.

Example: 12 reallocated sectors give 100 x 0.99^12 = **88%**. The synthetic failing drive (120 reallocated,
16 pending, 16 offline uncorrectable, 3 reported uncorrectable, 1 timeout) scores **10%**.

UDMA CRC errors (199) are **not** counted against Health: they mean data got garbled on the cable or port,
not that the disk surface is bad. They still make the status WARNING ("reseat or replace the SATA cable") and
lower Performance. Temperature never changes Health either; it affects status and Performance.

### Status

- **CRITICAL**: Health below 30%, SMART FAILED, an attribute failing now, NVMe critical warning / spare below
  threshold, wear at 100% or more, temperature at the critical limit, or a large count of one bad-sector
  type (10+ pending/offline/reported/media errors, 100+ reallocated).
- **WARNING**: Health below 70%, or any of the counters above is non-zero, an attribute failed in the past,
  CRC errors, NVMe spare close to its threshold, or temperature at the warning limit.
- **OK**: none of the above. **UNKNOWN**: no SMART data (virtual disk, USB bridge, permissions).

Default temperature limits (warning / critical): HDD 50 / 55 C, SATA SSD 60 / 70 C, NVMe 70 / 80 C, or the
drive's own limits if it reports them. Change them under `[thresholds]`.

### Performance %

An estimate of how much the drive's condition is likely slowing it down. It is **not a benchmark**. Starts at
100 and subtracts:

| Cause | Points | Max |
|---|---|---|
| UDMA CRC errors (retransmits) | 1 each | 30 |
| Command timeouts (stalls) | 2 each | 30 |
| Pending sectors (repeated re-reads) | 1 each | 20 |
| Reallocated sectors (extra seeks to the spare area) | 0.1 each | 20 |
| Offline uncorrectable | 0.5 each | 10 |
| NVMe media errors | 1 each | 10 |
| Currently at warning / critical temperature | 10 / 25 | |
| NVMe time above warning/critical temperature (throttling) | 1 + hours | 15 |

All constants live at the top of `hard_drive_monitor/health.py`.

## Examples

```bash
hdmon status
hdmon status --json | jq '.drives[] | {dev: .drive.device, health: .assessment.health, status: .assessment.status}'
hdmon detail /dev/sdb
hdmon detail D:                       # Windows drive letter
hdmon history --limit 48
hdmon selftest /dev/sda short         # prints expected duration
hdmon selftest /dev/sda --results
hdmon watch --interval 30m
hdmon serve                           # http://127.0.0.1:8765

# Demo without real drives (synthetic fixtures):
hdmon status --from-json tests/fixtures
hdmon detail nvme1 --from-json tests/fixtures
hdmon status --from-json tests/fixtures/windows/windows_inventory.json
```

Save a drive's data for later or for a bug report: `sudo smartctl -x --json /dev/sda > sda.json`, then
`hdmon detail sda --from-json sda.json`.

## Development

```bash
python -m unittest discover -s tests -t . -v
python tests/fixtures/make_fixtures.py     # regenerate the synthetic fixtures
python tools/render_demo.py                # regenerate demo-status.png (needs Pillow)
```

All fixtures in `tests/fixtures/` are **synthetic**: hand-written to match the shape of `smartctl -x --json`
(smartmontools 7.4) and of the Windows PowerShell inventory query. They were not captured from real drives,
and their serial numbers are fake. Every file carries a `_synthetic` marker.

CI (GitHub Actions) runs the tests on Ubuntu, macOS and Windows with Python 3.10 and 3.12.

## License

MIT. See [LICENSE](LICENSE).
