#!/bin/sh
# TCG by default: nested KVM on the build box hung with no console output. KVM="-enable-kvm -cpu host" to try it.
KVM="${KVM:-}"
exec qemu-system-x86_64 $KVM -m 1024 -smp 2 -display none -monitor none -serial file:console.log -no-reboot \
  -kernel kroot/boot/vmlinuz-6.12.111+deb13-amd64 -initrd initrd.gz -append "console=ttyS0 quiet panic=-1 scsi_mod.scan=sync" \
  -device ahci,id=ahci \
  -drive if=none,id=da,file=disk-a.img,format=raw -device ide-hd,drive=da,bus=ahci.0,model="QEMU-HDD-A",serial=QMHDD0001,rotation_rate=7200 \
  -drive if=none,id=db,file=disk-b.img,format=raw -device ide-hd,drive=db,bus=ahci.1,model="QEMU-SSD-B",serial=QMSSD0002,rotation_rate=1 \
  -drive if=none,id=dc,file=disk-c.img,format=raw -device ide-hd,drive=dc,bus=ahci.2,model="QEMU-MD-C",serial=QMMDC0003 \
  -drive if=none,id=dd,file=disk-d.img,format=raw -device ide-hd,drive=dd,bus=ahci.3,model="QEMU-MD-D",serial=QMMDD0004 \
  -drive if=none,id=nv,file=nvme.img,format=raw -device nvme,drive=nv,serial=QMNVME0005 \
  -device qemu-xhci -drive if=none,id=us,file=usb.img,format=raw -device usb-storage,drive=us,serial=QMUSB0006
