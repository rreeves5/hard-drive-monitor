# External programs hdmon still runs (0.2.5 audit)

Standing rule: use OS system calls and APIs, not other programs; a helper program is only a fallback. This is
the full list of child processes left in `src/` and `gui/` after the 0.2.5 audit. For each one: when it runs,
and why it is still a program. Every one of them is skipped when the native call works.

## Replaced by native calls

| Was | Now | Since |
|---|---|---|
| `smartctl` for SMART (all OSes) | Linux `SG_IO` + `NVME_IOCTL_ADMIN_CMD`; Windows `DeviceIoControl` (ATA pass-through, `SMART_RCV_DRIVE_DATA`, `IOCTL_STORAGE_QUERY_PROPERTY`); macOS IOKit SMART interfaces | 0.2.4 |
| `smartctl -t short/long` (`hdmon selftest`) | ATA SMART EXECUTE OFF-LINE IMMEDIATE (B0h/D4h) through `SG_IO` / `IOCTL_ATA_PASS_THROUGH` (`SMART_SEND_DRIVE_COMMAND` fallback) / IOKit `SMARTExecuteOffLineImmediate`; NVMe Device Self-test (admin 14h) through `NVME_IOCTL_ADMIN_CMD` / `IOCTL_STORAGE_PROTOCOL_COMMAND`. The self-test logs (ATA log 06h, NVMe log page 06h) are read natively too, so `selftest --results` needs no smartctl | 0.2.5 |
| `lsblk` (Linux discovery) | `/sys/block` + `/proc/mounts` | 0.2.4 |
| `diskutil list` (macOS discovery) | IOKit registry (`IOMedia` / `IOBlockStorageDevice`) | 0.2.4 |
| `diskutil appleRAID list` | IOKit registry: `AppleRAIDSet` / `AppleRAIDMember` objects and their `AppleRAID-*` properties (keys from Apple's open-source `AppleRAIDUserLib.h`) | 0.2.5 |
| PowerShell `Get-PhysicalDisk` / `Get-StorageReliabilityCounter` / `Get-Partition` | `DeviceIoControl`; WMI over COM (`MSFT_PhysicalDisk`, `MSFT_StorageReliabilityCounter`) | 0.2.4 |
| PowerShell `Get-StoragePool` / `Get-VirtualDisk` / `Get-StorageJob` | WMI over COM (`root\Microsoft\Windows\Storage`) | 0.2.4 |
| `mdadm --detail` | `/proc/mdstat` + `/sys/block/md*/md/` | 0.2.4 |
| `hostname` | `gethostname(2)` / `%COMPUTERNAME%` | 0.2.4 |
| `notify-send` | D-Bus `org.freedesktop.Notifications.Notify` over the session bus socket (a small built-in D-Bus client, so the static binary needs no libdbus) | 0.2.5 |
| PowerShell toast script | `Shell_NotifyIcon` balloon from a message-only window (shown as a toast on Windows 10/11) | 0.2.5 |
| `osascript` (when running inside an app bundle) | `UNUserNotificationCenter` | 0.2.5 |
| `systemctl --user daemon-reload / enable --now / disable --now` | Unit files written directly, then D-Bus `org.freedesktop.systemd1.Manager` `Reload`, `EnableUnitFiles`, `StartUnit`, `StopUnit`, `DisableUnitFiles` (session bus, or systemd's private socket `$XDG_RUNTIME_DIR/systemd/private`) | 0.2.5 |
| `systemctl --user show-environment` (is there a user manager?) | D-Bus `Peer.Ping` to `org.freedesktop.systemd1` | 0.2.5 |
| `schtasks /Create /XML`, `/Delete`, `/Query` | Task Scheduler 2.0 COM API (`ITaskService::Connect`, `ITaskFolder::RegisterTask` with the same XML, `DeleteTask`, `GetTask`) | 0.2.5 |
| `launchctl bootstrap / bootout` | Plist written directly to `~/Library/LaunchAgents` (that alone makes it run from the next login), then ServiceManagement `SMJobSubmit` / `SMJobRemove` to load or unload it in the running session | 0.2.5 |
| `setcap` after `hdmon update` | The `security.capability` xattr is copied from the old binary to the new one with `getxattr` / `setxattr` before the atomic rename | 0.2.5 |

## Still run as programs

| Program | Where | When it runs | Why it stayed |
|---|---|---|---|
| `smartctl` | `smart.cpp`, `collect.cpp`, `cli.cpp` | Only if installed: (a) `[smart] source = "auto"` and the native read or self-test start failed for a drive; (b) `source = "smartctl"`; (c) hardware RAID members (`--scan-open`, `-d megaraid,N`, ...) | (a)/(b) optional fallback, by design. (c) controller pass-through (MegaRAID MFI, Adaptec, Areca, 3ware, cciss, HighPoint) uses a different private ioctl per vendor; smartmontools has them all. |
| `lsblk`, `diskutil list/info` | `smart.cpp` `discover()` | Only on the smartctl path above | Part of the smartctl fallback. |
| PowerShell (`PS_LETTERS`, `PS_PHYSICAL`, `PS_SPACES`) | `smart.cpp`, `wmi.cpp` | Windows, only when the WMI COM query fails (WMI service disabled or broken) | Last-resort fallback. |
| `mdadm --detail` | `raid_live.cpp` | Linux, only when an array is in `/proc/mdstat` but has no `/sys/block/mdX/md/` (old kernels, some containers) | Fallback. |
| `rstcli64` / `rstcli` | `raid_live.cpp` | Windows, only if Intel's RST CLI is installed | Intel publishes no API for RST volume/member state. The volume itself is still found natively, as a virtual disk. |
| `diskutil appleRAID list` | `raid_live.cpp` | macOS, only if the IOKit registry query itself fails | Fallback. |
| `notify-send` | `alerts.cpp` | Linux, only when the D-Bus call fails (no session bus, no notification server) | Fallback. |
| `osascript` | `alerts.cpp` | macOS CLI alerts | `UNUserNotificationCenter` refuses callers outside an app bundle, and `hdmon` is a bare executable, so the CLI always lands here (the GUI app and anything bundled use the native call). There is no other public notification API on current macOS. |
| PowerShell balloon script | `alerts.cpp` | Windows, only when `Shell_NotifyIcon` fails (no shell / notification area, e.g. a service session) | Fallback. |
| `curl` | `update.cpp`, `alerts.cpp` | `hdmon update` / auto-check; e-mail alerts | Native HTTPS/SMTP (WinHTTP, NSURLSession, a statically linked TLS library on Linux) was deliberately deferred by Richard in 0.2.5; curl stays the transport for now. |
| `systemctl --user` | `service.cpp` | Linux, only when the systemd D-Bus calls fail | Fallback. |
| `crontab -l` / `crontab -` | `service.cpp` | Linux without a systemd user manager | Cron has no API: the per-user spool is only writable through the setuid `crontab` program. |
| `launchctl bootout/bootstrap/load/unload` | `service.cpp` | macOS, only when `SMJobSubmit` / `SMJobRemove` fail | Fallback. (`SMJobSubmit` is deprecated in favour of `SMAppService`, which needs an app bundle and macOS 13, so it is still the API a bare CLI can use.) |
| `schtasks` | `service.cpp`, `autostart.cpp` | Windows, only when the Task Scheduler COM call fails | Fallback. |
| `hdmon(-gui) --version` (the new file) | `update.cpp` | Once after `hdmon update` replaced the binary | Starts the new binary to confirm it runs and to report its version. It is our own program, and running it is the point of the check. |
| `hdmon-gui` (itself) | `gui/hdmon_gui.cpp` | GUI "Restart" after an update | Relaunches itself. |

## How the native paths were checked

Linux paths ran for real: a QEMU kernel for SMART, self-test start, the self-test log and the setcap copy in
`hdmon update` (`tools/vm-verify/`); a private `dbus-daemon` with libdbus-based fake Notifications/systemd1
services for D-Bus (`tools/dbus-verify/`, also run in CI). Windows Task Scheduler COM ran under Wine (register
and delete) and runs in CI on the Windows runner (register, read back with schtasks, delete). macOS code
(UserNotifications, SMJobSubmit, IOKit AppleRAID, IOKit self-test) is compiled in CI; launchd and notifications
are exercised there informationally; AppleRAID parsing is fixture-tested only.
