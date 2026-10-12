"""Builds tests/fixtures/raid/sample/ (SYNTHETIC) from the drive fixtures one folder up.

The sample machine: NVMe system disk, a plain SATA HDD and SSD, a Linux md RAID1 (md0) that is degraded
(sdd failed out) while the surviving member sdb is itself CRITICAL, and an LSI MegaRAID controller with two
SATA disks behind it (smartctl -d sat+megaraid,N).
Run:  python tests/fixtures/raid/make_raid_fixtures.py
"""
import copy
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
FIX = os.path.dirname(HERE)
OUT = os.path.join(HERE, "sample")


def load(name):
    return json.load(open(os.path.join(FIX, name)))


def save(name, d):
    with open(os.path.join(OUT, name), "w") as f:
        json.dump(d, f, indent=2)
        f.write("\n")


def retarget(d, dev, dtype=None, serial=None, model=None, info=None):
    d = copy.deepcopy(d)
    d["device"]["name"] = dev
    d["device"]["info_name"] = info or dev
    if dtype:
        d["device"]["type"] = dtype
    if serial:
        d["serial_number"] = serial
    if model:
        d["model_name"] = model
    d["smartctl"]["argv"] = ["smartctl", "-x", "--json"] + (["-d", dtype] if dtype and "megaraid" in dtype else []) + [dev]
    return d


def main():
    os.makedirs(OUT, exist_ok=True)
    nvme = load("nvme_healthy.json")
    nvme["_volumes"] = ["/", "/boot/efi"]
    save("nvme0.json", nvme)
    sda = load("hdd_healthy.json")
    sda["_volumes"] = ["/srv/media"]
    save("sda.json", sda)
    sdc = load("ssd_sata_healthy.json")
    sdc["_volumes"] = ["/home"]
    save("sdc.json", sdc)
    save("sdb.json", load("hdd_failing.json"))   # md0 member, CRITICAL
    sdd = retarget(load("hdd_healthy.json"), "/dev/sdd", serial="WD-SYNTH00000006", model="WDC WD20EFRX-68EUZN0")
    sdd["user_capacity"] = {"blocks": 3907029168, "bytes": 2000398934016}
    save("sdd.json", sdd)
    for n, serial in [(0, "WD-SYNTHMR0000"), (1, "WD-SYNTHMR0001")]:
        m = retarget(load("hdd_healthy.json"), "/dev/bus/0", "sat+megaraid,%d" % n, serial=serial,
                     model="WDC WD4003FFBX-68MU3N0", info="/dev/bus/0 [megaraid_disk_%02d] [SAT]" % n)
        save("bus0-megaraid%d.json" % n, m)
    with open(os.path.join(OUT, "mdstat.txt"), "w") as f:
        f.write("Personalities : [raid1]\n"
                "md0 : active raid1 sdb1[0] sdd1[1](F)\n"
                "      1953382464 blocks super 1.2 [2/1] [U_]\n"
                "      bitmap: 3/15 pages [12KB], 65536KB chunk\n\n"
                "unused devices: <none>\n")
    print("wrote", sorted(os.listdir(OUT)))


if __name__ == "__main__":
    main()
