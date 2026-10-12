#!/bin/busybox sh
/bin/busybox --install -s /bin
mount -t proc proc /proc; mount -t sysfs sys /sys; mount -t devtmpfs dev /dev
for m in $(cat /mods/order); do insmod /mods/$m.ko 2>/dev/null; done
sleep 4
mdev -s 2>/dev/null
O=/out/report.txt
{
echo "== block devices"; ls /sys/block; cat /proc/partitions
# SCSI names are not tied to AHCI ports, so pick the md pair by model.
MDA=""; MDB=""
for b in /sys/block/sd*; do case "$(cat $b/device/model)" in QEMU-MD-C*) MDA=/dev/${b##*/};; QEMU-MD-D*) MDB=/dev/${b##*/};; esac; done
echo "== create degraded md raid1 on $MDA+$MDB, fail $MDB"
mdadm --create /dev/md0 --run --level=1 --raid-devices=2 --metadata=1.2 --assume-clean $MDA $MDB 2>&1
mdadm /dev/md0 --fail $MDB 2>&1
sleep 1
cat /proc/mdstat
echo "== hdmon check (degraded, first sight)"; HOME=/tmp hdmon check --color never 2>&1; echo rc=$?
for d in /dev/sd? /dev/nvme0; do echo "== smartctl -x --json $d"; smartctl -x --json $d > /out/smartctl-$(basename $d).json 2>&1; echo rc=$?; done
mkdir -p /out/raw
echo "== hdmon status --json (native)"; HDMON_NATIVE_DUMP=/out/raw HOME=/tmp hdmon status --json --no-record > /out/hdmon-native.json 2>&1; echo rc=$?
echo "== hdmon status (native)"; HOME=/tmp hdmon status --no-record --color never 2>&1
echo "== hdmon raid"; HOME=/tmp hdmon raid --no-record --color never 2>&1
echo "== hdmon detail sda"; HOME=/tmp hdmon detail /dev/sda --color never 2>&1
echo "== md recover: re-add $MDB and watch rebuild"
mdadm /dev/md0 --remove $MDB 2>&1; mdadm --zero-superblock $MDB 2>&1; mdadm /dev/md0 --add $MDB 2>&1
sleep 1; cat /proc/mdstat; HOME=/tmp hdmon raid --no-record --color never 2>&1
echo "== hdmon check (rebuilding)"; HOME=/tmp hdmon check --quiet --color never 2>&1; echo rc=$?
i=0; while grep -q recovery /proc/mdstat && [ $i -lt 120 ]; do sleep 1; i=$((i+1)); done
cat /proc/mdstat
echo "== hdmon check (after rebuild)"; HOME=/tmp hdmon check --color never 2>&1; echo rc=$?
echo "== hdmon status --source smartctl"; printf "[smart]\nsource = \"smartctl\"\n" > /tmp/sc.toml; HOME=/tmp hdmon status --no-record --color never --config /tmp/sc.toml 2>&1 | tail -4
echo "== non-root (uid 1000), no capabilities"
mkdir -p /tmp/u && chown 1000:1000 /tmp/u
cp /bin/hdmon /tmp/hdmon-user && chmod 755 /tmp/hdmon-user
HOME=/tmp/u setfcap --run 1000 /tmp/hdmon-user status --no-record --color never 2>&1 | tail -5
echo "== non-root (uid 1000) with file capabilities cap_sys_rawio,cap_sys_admin,cap_dac_read_search+ep"
setfcap /tmp/hdmon-user && echo "caps set"
HOME=/tmp/u setfcap --run 1000 /tmp/hdmon-user status --no-record --color never 2>&1 | head -9
} > $O 2>&1
for f in /out/*; do [ -f "$f" ] || continue; echo "@@@FILE $f"; cat $f; echo; echo "@@@END"; done > /dev/console
for f in /out/raw/*; do echo "@@@B64 $f"; base64 $f; echo "@@@END"; done > /dev/console
sync
poweroff -f
