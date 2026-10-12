# Real-kernel captures (QEMU)

Not synthetic. These were read from emulated drives by a real Linux kernel (Debian 6.12.111 under
QEMU 10.0.13, AHCI `ide-hd` with model QEMU-HDD-A, serial QMHDD0001, rotation 7200, plus an emulated NVMe
controller with serial QMNVME0005), using `tools/vm-verify/verify.sh`.

* `hdd-*.bin` (QEMU-HDD-A, /dev/sdc in that boot), `ssd-*.bin` (QEMU-SSD-B, /dev/sda) and `nvme-*.bin` are the
  raw sectors hdmon's native Linux backend got back from SG_IO ATA PASS-THROUGH(16) and NVME_IOCTL_ADMIN_CMD.
  They were written with `HDMON_NATIVE_DUMP=<dir>`.
* `smartctl-*.json` is `smartctl -x --json` (smartmontools 7.4) run on the same devices during the same boot.

`tests/test_native.cpp` decodes the bytes and checks that the result matches what parse_smartctl gets from
smartctl's JSON: the same attributes, raw strings, health score and status.

The devices are emulated. These files show that the kernel ioctl path and the decoders work. They do not
show that any vendor's firmware behaves the same way.

hdd-selftest-log.bin / smartctl-selftest.json (0.2.5): SMART READ LOG 06h dumped by hdmon's native backend after
`hdmon selftest /dev/sda short` (SMART EXECUTE OFF-LINE IMMEDIATE, sent natively) completed in the QEMU VM, and
smartctl's independent decoding of the same log.
