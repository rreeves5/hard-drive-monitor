# RAID fixtures (SYNTHETIC)

Hand-written to match the output format of each tool or file; none of it was captured from a real array.

| File | Mimics | Used by |
|------|--------|---------|
| `mdstat-clean.txt`, `mdstat-degraded.txt`, `mdstat-recovering.txt` | Linux `/proc/mdstat` | `parse_mdstat` tests |
| `mdadm-detail-md0.txt` | `mdadm --detail /dev/md0` while recovering | `parse_mdadm_detail` tests |
| `storage-spaces-healthy.json`, `-degraded.json`, `-repairing.json` | the JSON hdmon builds from `MSFT_StoragePool` / `MSFT_VirtualDisk` / `MSFT_PhysicalDisk` / `MSFT_StorageJob` (WMI) or the PowerShell fallback | `parse_storage_spaces` tests |
| `diskutil-appleraid.txt` | macOS `diskutil appleRAID list` | `parse_appleraid` tests |
| `rstcli-volume.txt` | Intel `rstcli64 --information --volume` | `parse_rstcli` tests |
| `smartctl-scan-megaraid.json` | `smartctl --scan-open --json` on a MegaRAID host | `parse_scan` / hardware grouping tests |
| `sample/` | a whole machine: NVMe system disk, two plain disks, a degraded md RAID1 whose surviving member is CRITICAL, and a MegaRAID controller with two disks | `hdmon status --from-json tests/fixtures/raid/sample`, GUI screenshots |

Regenerate the JSON in `sample/` with `python tests/fixtures/raid/make_raid_fixtures.py`.
