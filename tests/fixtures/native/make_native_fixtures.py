"""Byte-level fixtures for the native SMART decoders (src/native_smart.cpp).

SYNTHETIC: these sectors are encoded by this script from the smartctl-shaped JSON fixtures one folder up
(hdd_failing.json -> ATA IDENTIFY / SMART READ DATA / READ THRESHOLDS, nvme_worn.json -> NVMe Identify
Controller / log page 02h), following the ATA/ATAPI-8 ACS and NVMe 1.4 layouts. They let the tests check that
the native decode of the raw bytes produces the same attributes, health score and status as the smartctl path.
Run:  python tests/fixtures/native/make_native_fixtures.py
"""
import json
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
FIX = os.path.dirname(HERE)


def ata_str(s, words):
    s = s.ljust(words * 2)[: words * 2].encode("ascii")
    out = bytearray()
    for i in range(0, len(s), 2):
        out += bytes([s[i + 1], s[i]])
    return bytes(out)


def checksum(b):
    b[511] = (-sum(b[:511])) & 0xFF


def identify(d):
    w = [0] * 256
    b = bytearray(512)
    b[20:40] = ata_str(d["serial_number"], 10)
    b[46:54] = ata_str(d["firmware_version"], 4)
    b[54:94] = ata_str(d["model_name"], 20)
    blocks = d["user_capacity"]["blocks"]
    w[60] = min(blocks, 0x0FFFFFFF) & 0xFFFF
    w[61] = (min(blocks, 0x0FFFFFFF) >> 16) & 0xFFFF
    w[76] = 0x000E            # SATA 1.5/3.0/6.0 Gb/s
    w[82] = 0x0001            # SMART feature set supported
    w[83] = 0x0400            # 48-bit LBA
    w[85] = 0x0001            # SMART enabled
    for i in range(4):
        w[100 + i] = (blocks >> (16 * i)) & 0xFFFF
    w[106] = 0x4000           # valid, 512-byte logical/physical
    w[168] = d["form_factor"]["ata_value"]
    w[217] = d["rotation_rate"] if d["rotation_rate"] else 1
    w[222] = 0x1000 | 0x00FE  # serial transport, SATA 1.0a .. 3.1 (bits 1-7)
    for i, v in enumerate(w):
        if v:
            struct.pack_into("<H", b, 2 * i, v)
    b[510] = 0xA5             # integrity word signature
    checksum(b)
    return b


def smart_data(d):
    b = bytearray(512)
    struct.pack_into("<H", b, 0, 0x0010)
    for i, a in enumerate(d["ata_smart_attributes"]["table"]):
        off = 2 + 12 * i
        b[off] = a["id"]
        struct.pack_into("<H", b, off + 1, a["flags"]["value"])
        b[off + 3] = a["value"]
        b[off + 4] = a["worst"]
        raw = a["raw"]["value"]
        b[off + 5: off + 11] = raw.to_bytes(6, "little")
    st = d["ata_smart_data"]["self_test"]
    b[362] = 0x82
    b[363] = st["status"]["value"]
    b[367] = 0x7B
    struct.pack_into("<H", b, 368, 0x0003)
    b[370] = 0x01
    pm = st["polling_minutes"]
    b[372] = pm["short"]
    if pm["extended"] > 254:
        b[373] = 0xFF
        struct.pack_into("<H", b, 375, pm["extended"])
    else:
        b[373] = pm["extended"]
    b[374] = pm["conveyance"]
    checksum(b)
    return b


def thresholds(d):
    b = bytearray(512)
    struct.pack_into("<H", b, 0, 0x0010)
    for i, a in enumerate(d["ata_smart_attributes"]["table"]):
        off = 2 + 12 * i
        b[off] = a["id"]
        b[off + 1] = a["thresh"]
    checksum(b)
    return b


def nvme_id(d):
    b = bytearray(4096)
    struct.pack_into("<HH", b, 0, d["nvme_pci_vendor"]["id"], d["nvme_pci_vendor"]["subsystem_id"])
    b[4:24] = d["serial_number"].ljust(20).encode()
    b[24:64] = d["model_name"].ljust(40).encode()
    b[64:72] = d["firmware_version"].ljust(8).encode()
    struct.pack_into("<HH", b, 266, 70 + 273, 80 + 273)       # WCTEMP / CCTEMP in Kelvin
    b[280:296] = d["nvme_total_capacity"].to_bytes(16, "little")
    struct.pack_into("<I", b, 516, 1)                        # NN
    return b


def nvme_log(d):
    L = d["nvme_smart_health_information_log"]
    b = bytearray(512)
    b[0] = L["critical_warning"]
    struct.pack_into("<H", b, 1, L["temperature"] + 273)
    b[3] = L["available_spare"]
    b[4] = L["available_spare_threshold"]
    b[5] = L["percentage_used"]
    for off, k in [(32, "data_units_read"), (48, "data_units_written"), (64, "host_reads"), (80, "host_writes"),
                   (96, "controller_busy_time"), (112, "power_cycles"), (128, "power_on_hours"),
                   (144, "unsafe_shutdowns"), (160, "media_errors"), (176, "num_err_log_entries")]:
        b[off: off + 16] = L[k].to_bytes(16, "little")
    struct.pack_into("<II", b, 192, L["warning_temp_time"], L["critical_comp_time"])
    for i, t in enumerate(L.get("temperature_sensors", [])):
        struct.pack_into("<H", b, 200 + 2 * i, t + 273)
    return b


def main():
    hdd = json.load(open(os.path.join(FIX, "hdd_failing.json")))
    nv = json.load(open(os.path.join(FIX, "nvme_worn.json")))
    for name, data in [("ata_identify.bin", identify(hdd)), ("ata_smart_data.bin", smart_data(hdd)),
                       ("ata_smart_thresholds.bin", thresholds(hdd)), ("nvme_identify_ctrl.bin", nvme_id(nv)),
                       ("nvme_smart_log.bin", nvme_log(nv))]:
        with open(os.path.join(HERE, name), "wb") as f:
            f.write(bytes(data))
        print("wrote", name, len(data))


if __name__ == "__main__":
    main()
