# External programs hdmon still runs (0.2.4 audit)

Standing rule: use OS system calls and APIs, not other programs. This is the full list of child processes
left in `src/` and `gui/` after the 0.2.4 audit. For each one: when it runs, and why it is still a program.

## Replaced by native calls in 0.2.4

| Was | Now |
|---|---|
| `smartctl` for SMART (all OSes) | Linux `SG_IO` + `NVME_IOCTL_ADMIN_CMD`; Windows `DeviceIoControl` (ATA pass-through, `SMART_RCV_DRIVE_DATA`, `IOCTL_STORAGE_QUERY_PROPERTY`); macOS IOKit SMART interfaces |
| `lsblk` (Linux discovery) | `/sys/block` + `/proc/mounts` |
| `diskutil list` (macOS discovery) | IOKit registry (`IOMedia` / `IOBlockStorageDevice`) |
| PowerShell `Get-PhysicalDisk` / `Get-StorageReliabilityCounter` / `Get-Partition` | `DeviceIoControl` for letters and disk numbers; WMI over COM (`MSFT_PhysicalDisk`, `MSFT_StorageReliabilityCounter`) for the non-admin fallback |
| PowerShell `Get-StoragePool` / `Get-VirtualDisk` / `Get-PhysicalDisk` / `Get-StorageJob` | WMI over COM (`root\Microsoft\Windows\Storage`) |
| `mdadm --detail` | `/proc/mdstat` + `/sys/block/md*/md/` |
| `hostname` (e-mail Message-ID) | `gethostname(2)` / `%COMPUTERNAME%` |

## Still run as programs

| Program | Where | When it runs | Why it stayed |
|---|---|---|---|
| `smartctl` | `smart.cpp`, `collect.cpp`, `cli.cpp` | Only if installed: (a) `[smart] source = "auto"` and the native read failed for a drive; (b) `source = "smartctl"`; (c) hardware RAID members (`--scan-open`, `-d megaraid,N` and so on); (d) `hdmon selftest` | (a)/(b) optional fallback, by design. (c) controller pass-through (MegaRAID MFI, Adaptec, Areca, 3ware, cciss, HighPoint) uses a different private ioctl per vendor, and smartmontools has them all. (d) self-tests are not native yet (planned: ATA SMART EXECUTE OFF-LINE IMMEDIATE and NVMe Device Self-test through the same pass-through paths). |
| `lsblk`, `diskutil list/info` | `smart.cpp` `discover()` | Only on the smartctl path (`source = "smartctl"`, `hdmon selftest`) | Part of the smartctl fallback; the native path does not use them. |
| PowerShell (`PS_LETTERS`, `PS_PHYSICAL`, `PS_SPACES`) | `smart.cpp`, `wmi.cpp` | Windows, only when the WMI COM query fails (WMI service disabled or broken) | Last-resort fallback. |
| `mdadm --detail` | `raid_live.cpp` | Linux, only when an array is in `/proc/mdstat` but has no `/sys/block/mdX/md/` directory (old kernels, some containers) | Fallback. |
| `rstcli64` / `rstcli` | `raid_live.cpp` | Windows, only if Intel's RST CLI is installed | Intel publishes no API for RST volume and member state. The volume itself is still found natively, as a virtual disk. |
| `diskutil appleRAID list` | `raid_live.cpp` | macOS | AppleRAID set state is not in a public framework. The IORegistry `AppleRAID*` objects are undocumented, and I could not test them without a Mac. Planned: read them through IOKit and keep diskutil as the fallback. |
| `notify-send` | `alerts.cpp` | Linux desktop alerts from the CLI (`hdmon check`) | Planned: D-Bus `org.freedesktop.Notifications.Notify`. The static Linux binary has no libdbus, so this needs a small D-Bus client over the session socket. The GUI already uses native notifications through wxWidgets. |
| `osascript` | `alerts.cpp` | macOS desktop alerts from the CLI | `UNUserNotificationCenter` works only from a signed app bundle, and the CLI is a bare executable. The GUI app uses native notifications. |
| PowerShell toast script | `alerts.cpp` | Windows desktop alerts from the CLI | WinRT toasts need an AppUserModelID and a Start Menu shortcut. Planned: `Shell_NotifyIcon` balloon directly. The GUI uses native tray balloons. |
| `curl` | `update.cpp`, `alerts.cpp` | `hdmon update` / auto-check; e-mail alerts | Planned: WinHTTP (Windows), NSURLSession (macOS), and libcurl or sockets with TLS on Linux. Linux needs a TLS library linked statically into the static binary, and SMTP STARTTLS needs the same. |
| `systemctl --user`, `crontab` | `service.cpp` | `hdmon install-service` / `uninstall-service` (Linux) | The unit files are written directly; `systemctl` only reloads and enables them (D-Bus `org.freedesktop.systemd1` is planned, same libdbus issue as notify-send). Cron has no API. |
| `launchctl` | `service.cpp` | `install-service` / `uninstall-service` (macOS) | The plist is written directly. `launchctl bootstrap` has no public replacement (`SMAppService` needs an app bundle). |
| `schtasks` | `service.cpp`, `autostart.cpp` | `install-service` / GUI "Start at login" (Windows) | Planned: Task Scheduler COM (`ITaskService`). It was not done in this round. |
| `hdmon(-gui) --version` (the downloaded file) | `update.cpp` | After downloading an update | Runs the new binary once to check that it starts before it replaces the old one. It is our own program. |
| `hdmon-gui` (itself) | `gui/hdmon_gui.cpp` | GUI "Restart" after an update | Relaunches itself. |
