#!/bin/sh
# Boot a real Linux kernel in QEMU with emulated AHCI/NVMe/USB disks and run hdmon's native SMART + md RAID paths
# next to smartctl/mdadm on the same devices. Results land in ./out (report.txt, JSON, raw sectors in out/raw).
# Needs: qemu-system-x86_64, busybox, smartmontools, mdadm, and a Debian kernel image extracted to ./kroot
#   (apt-get download linux-image-6.12.111+deb13-amd64 && dpkg-deb -x linux-image-*.deb kroot)
# Usage: tools/vm-verify/verify.sh <static hdmon binary>   (run from a scratch dir that holds kroot/)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
HDMON=$(readlink -f "$1")
cp "$HERE/init.sh" "$HERE/run.sh" "$HERE/mkinitrd.py" .
mkdir -p helpers && cc -static -O2 -o helpers/setfcap "$HERE/setfcap.c"
python3 mkinitrd.py "$HDMON"
for i in a b c d; do rm -f disk-$i.img; truncate -s2G disk-$i.img; done
[ -f nvme.img ] || truncate -s1G nvme.img
[ -f usb.img ] || truncate -s512M usb.img
rm -f console.log
timeout 900 sh ./run.sh </dev/null
rm -rf out; mkdir -p out/raw
python3 - <<'PY'
import re, base64
t = open('console.log', errors='replace').read().replace('\r', '')
for m in re.finditer(r'@@@FILE (\S+)\n(.*?)\n@@@END', t, re.S):
    open('out/' + m.group(1).split('/')[-1], 'w').write(m.group(2))
for m in re.finditer(r'@@@B64 (\S+)\n(.*?)\n@@@END', t, re.S):
    open('out/raw/' + m.group(1).split('/')[-1], 'wb').write(base64.b64decode(m.group(2)))
PY
ls out out/raw
