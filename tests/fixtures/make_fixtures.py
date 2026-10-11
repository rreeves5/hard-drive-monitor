"""Regenerates the SYNTHETIC fixtures in this folder.

Every file here was written by hand to mimic the shape of `smartctl -x --json`
(smartmontools 7.4) or of Hard Drive Monitor's Windows PowerShell inventory query.
None of them was captured from a real drive; serial numbers are fake.
Run:  python tests/fixtures/make_fixtures.py
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SYN = "SYNTHETIC test fixture hand-written for Hard Drive Monitor. Not captured from a real drive."

FLAGS = {True: {"value": 51, "string": "PO--CK ", "prefailure": True, "updated_online": True,
                "performance": False, "error_rate": False, "event_count": False, "auto_keep": True},
         False: {"value": 50, "string": "-O--CK ", "prefailure": False, "updated_online": True,
                 "performance": False, "error_rate": False, "event_count": False, "auto_keep": True}}


def attr(i, name, value, worst, thresh, raw, prefail=False, when_failed="", raw_string=None):
    return {"id": i, "name": name, "value": value, "worst": worst, "thresh": thresh,
            "when_failed": when_failed, "flags": FLAGS[prefail],
            "raw": {"value": raw, "string": raw_string if raw_string is not None else str(raw)}}


def smartctl(devname, devtype, protocol, argv_extra=()):
    return {"json_format_version": [1, 0],
            "smartctl": {"version": [7, 4], "svn_revision": "5530", "platform_info": "x86_64-linux-synthetic",
                         "build_info": "(synthetic)", "argv": ["smartctl", "-x", "--json", *argv_extra, devname],
                         "exit_status": 0},
            "local_time": {"time_t": 1791748800, "asctime": "Sun Oct 11 12:00:00 2026 EDT"},
            "device": {"name": devname, "info_name": f"{devname} [{devtype.upper()}]", "type": devtype,
                       "protocol": protocol}}


def selftest_ata(rows):
    return {"standard": {"revision": 1, "table": rows, "count": len(rows),
                         "error_count_total": sum(1 for r in rows if not r["status"]["passed"]),
                         "error_count_outdated": 0}}


def st_row(kind, ok, hours, text=None, lba=None):
    r = {"type": {"value": 1 if kind == "Short offline" else 2, "string": kind},
         "status": {"value": 0 if ok else 121, "string": text or "Completed without error", "passed": ok},
         "lifetime_hours": hours}
    if lba is not None:
        r["lba"] = lba
    return r


def hdd_healthy():
    d = smartctl("/dev/sda", "sat", "ATA")
    d.update({
        "_synthetic": SYN,
        "model_family": "Western Digital Red", "model_name": "WDC WD40EFRX-68N32N0",
        "serial_number": "WD-SYNTH00000001", "firmware_version": "82.00A82",
        "user_capacity": {"blocks": 7814037168, "bytes": 4000787030016}, "logical_block_size": 512,
        "physical_block_size": 4096, "rotation_rate": 5400,
        "form_factor": {"ata_value": 2, "name": "3.5 inches"},
        "sata_version": {"string": "SATA 3.0", "value": 63},
        "interface_speed": {"max": {"string": "6.0 Gb/s"}, "current": {"string": "6.0 Gb/s"}},
        "smart_support": {"available": True, "enabled": True},
        "smart_status": {"passed": True},
        "ata_smart_data": {"offline_data_collection": {"status": {"value": 0, "string": "was never started"}},
                           "self_test": {"status": {"value": 0, "string": "completed without error", "passed": True},
                                         "polling_minutes": {"short": 2, "extended": 470, "conveyance": 5}}},
        "ata_smart_attributes": {"revision": 16, "table": [
            attr(1, "Raw_Read_Error_Rate", 200, 200, 51, 0, True),
            attr(3, "Spin_Up_Time", 177, 175, 21, 6141, True),
            attr(4, "Start_Stop_Count", 100, 100, 0, 412),
            attr(5, "Reallocated_Sector_Ct", 200, 200, 140, 0, True),
            attr(7, "Seek_Error_Rate", 200, 200, 0, 0),
            attr(9, "Power_On_Hours", 68, 68, 0, 23456),
            attr(10, "Spin_Retry_Count", 100, 253, 0, 0),
            attr(12, "Power_Cycle_Count", 100, 100, 0, 312),
            attr(192, "Power-Off_Retract_Count", 200, 200, 0, 96),
            attr(193, "Load_Cycle_Count", 196, 196, 0, 13020),
            attr(194, "Temperature_Celsius", 116, 104, 0, 34),
            attr(196, "Reallocated_Event_Count", 200, 200, 0, 0),
            attr(197, "Current_Pending_Sector", 200, 200, 0, 0),
            attr(198, "Offline_Uncorrectable", 100, 253, 0, 0),
            attr(199, "UDMA_CRC_Error_Count", 200, 200, 0, 0),
            attr(200, "Multi_Zone_Error_Rate", 200, 200, 0, 0),
        ]},
        "power_on_time": {"hours": 23456}, "power_cycle_count": 312,
        "temperature": {"current": 34, "power_cycle_min": 22, "power_cycle_max": 41},
        "ata_smart_self_test_log": selftest_ata([st_row("Short offline", True, 23400),
                                                 st_row("Extended offline", True, 22800)]),
    })
    return d


def hdd_failing(realloc=120, pending=16, offline=16, reported=3, timeout_raw=4295032833, poh=38211):
    d = smartctl("/dev/sdb", "sat", "ATA")
    d.update({
        "_synthetic": SYN,
        "model_family": "Seagate BarraCuda 3.5", "model_name": "ST2000DM008-2FR102",
        "serial_number": "ZFL0SYNTH02", "firmware_version": "0001",
        "user_capacity": {"blocks": 3907029168, "bytes": 2000398934016}, "logical_block_size": 512,
        "physical_block_size": 4096, "rotation_rate": 7200,
        "form_factor": {"ata_value": 2, "name": "3.5 inches"},
        "sata_version": {"string": "SATA 3.1", "value": 127},
        "interface_speed": {"max": {"string": "6.0 Gb/s"}, "current": {"string": "6.0 Gb/s"}},
        "smart_support": {"available": True, "enabled": True},
        "smart_status": {"passed": True},
        "ata_smart_data": {"self_test": {"status": {"value": 121, "string": "completed: read failure", "passed": False},
                                         "polling_minutes": {"short": 1, "extended": 213, "conveyance": 2}}},
        "ata_smart_attributes": {"revision": 10, "table": [
            attr(1, "Raw_Read_Error_Rate", 72, 63, 6, 18834944, True),
            attr(3, "Spin_Up_Time", 96, 96, 0, 0, True),
            attr(5, "Reallocated_Sector_Ct", 91, 91, 10, realloc, True),
            attr(9, "Power_On_Hours", 57, 57, 0, poh),
            attr(12, "Power_Cycle_Count", 98, 98, 20, 2187),
            attr(187, "Reported_Uncorrect", 97, 97, 0, reported),
            attr(188, "Command_Timeout", 100, 99, 0, timeout_raw, raw_string="1 1 1"),
            attr(190, "Airflow_Temperature_Cel", 59, 52, 40, 41, raw_string="41 (Min/Max 24/48)"),
            attr(194, "Temperature_Celsius", 41, 48, 0, 41, raw_string="41 (0 18 0 0 0)"),
            attr(197, "Current_Pending_Sector", 100, 100, 0, pending),
            attr(198, "Offline_Uncorrectable", 100, 100, 0, offline),
            attr(199, "UDMA_CRC_Error_Count", 200, 200, 0, 0),
        ]},
        "power_on_time": {"hours": poh}, "power_cycle_count": 2187,
        "temperature": {"current": 41},
        "ata_smart_self_test_log": selftest_ata([
            st_row("Short offline", False, poh - 10, "Completed: read failure", 1052763384),
            st_row("Short offline", True, 30120)]),
    })
    return d


def ssd_sata():
    d = smartctl("/dev/sdc", "sat", "ATA")
    d.update({
        "_synthetic": SYN,
        "model_family": "Samsung based SSDs", "model_name": "Samsung SSD 860 EVO 500GB",
        "serial_number": "S3Z1NB0SYNTH03", "firmware_version": "RVT04B6Q",
        "user_capacity": {"blocks": 976773168, "bytes": 500107862016}, "logical_block_size": 512,
        "rotation_rate": 0, "form_factor": {"ata_value": 3, "name": "2.5 inches"},
        "trim": {"supported": True},
        "sata_version": {"string": "SATA 3.1", "value": 127},
        "smart_support": {"available": True, "enabled": True},
        "smart_status": {"passed": True},
        "ata_smart_data": {"self_test": {"status": {"value": 0, "string": "completed without error", "passed": True},
                                         "polling_minutes": {"short": 2, "extended": 85}}},
        "ata_smart_attributes": {"revision": 1, "table": [
            attr(5, "Reallocated_Sector_Ct", 100, 100, 10, 0, True),
            attr(9, "Power_On_Hours", 95, 95, 0, 21877),
            attr(12, "Power_Cycle_Count", 98, 98, 0, 1544),
            attr(177, "Wear_Leveling_Count", 94, 94, 0, 61, True),
            attr(179, "Used_Rsvd_Blk_Cnt_Tot", 100, 100, 10, 0, True),
            attr(181, "Program_Fail_Cnt_Total", 100, 100, 10, 0),
            attr(182, "Erase_Fail_Count_Total", 100, 100, 10, 0),
            attr(183, "Runtime_Bad_Block", 100, 100, 10, 0, True),
            attr(187, "Uncorrectable_Error_Cnt", 100, 100, 0, 0),
            attr(190, "Airflow_Temperature_Cel", 69, 50, 0, 31),
            attr(195, "ECC_Error_Rate", 200, 200, 0, 0),
            attr(199, "CRC_Error_Count", 100, 100, 0, 0),
            attr(235, "POR_Recovery_Count", 99, 99, 0, 41),
            attr(241, "Total_LBAs_Written", 99, 99, 0, 34561287512),
        ]},
        "ata_device_statistics": {"pages": [{"number": 7, "name": "Solid State Device Statistics", "revision": 1,
                                             "table": [{"offset": 8, "name": "Percentage Used Endurance Indicator",
                                                        "size": 1, "value": 6}]}]},
        "power_on_time": {"hours": 21877}, "power_cycle_count": 1544,
        "temperature": {"current": 31},
        "ata_smart_self_test_log": selftest_ata([st_row("Short offline", True, 21500)]),
    })
    return d


def nvme(devname, model, serial, cap, used, spare, media, cw, unsafe, poh, cycles, temp, written_units,
         warn_min=0, crit_min=0, err_entries=0):
    d = smartctl(devname, "nvme", "NVMe")
    d.update({
        "_synthetic": SYN,
        "model_name": model, "serial_number": serial, "firmware_version": "2B2QEXM7",
        "nvme_pci_vendor": {"id": 5197, "subsystem_id": 5197},
        "nvme_total_capacity": cap, "nvme_unallocated_capacity": 0,
        "nvme_namespaces": [{"id": 1, "size": {"blocks": cap // 512, "bytes": cap}}],
        "user_capacity": {"blocks": cap // 512, "bytes": cap}, "logical_block_size": 512,
        "smart_support": {"available": True, "enabled": True},
        "smart_status": {"passed": cw == 0, "nvme": {"value": cw}},
        "nvme_smart_health_information_log": {
            "critical_warning": cw, "temperature": temp, "available_spare": spare,
            "available_spare_threshold": 10, "percentage_used": used,
            "data_units_read": written_units * 2, "data_units_written": written_units,
            "host_reads": 512331812, "host_writes": 389112033, "controller_busy_time": 1832,
            "power_cycles": cycles, "power_on_hours": poh, "unsafe_shutdowns": unsafe,
            "media_errors": media, "num_err_log_entries": err_entries, "warning_temp_time": warn_min,
            "critical_comp_time": crit_min, "temperature_sensors": [temp, temp + 6]},
        "temperature": {"current": temp},
        "power_cycle_count": cycles, "power_on_time": {"hours": poh},
        "nvme_self_test_log": {"current_self_test_operation": {"value": 0, "string": "No self-test in progress"},
                               "table": [{"self_test_code": {"value": 1, "string": "Short"},
                                          "self_test_result": {"value": 0, "string": "Completed without error"},
                                          "power_on_hours": poh - 30}]},
    })
    return d


def win_inventory():
    """Shape of Hard Drive Monitor's PS_PHYSICAL query output (ConvertTo-Json)."""
    return {
        "_synthetic": SYN,
        "PhysicalDisks": [
            {"DeviceId": "0", "FriendlyName": "Samsung SSD 970 EVO Plus 1TB", "SerialNumber": "0025_3852_91B0_SYN1.",
             "MediaType": "SSD", "BusType": "NVMe", "Size": 1000204886016, "HealthStatus": "Healthy",
             "OperationalStatus": "OK", "FirmwareVersion": "2B2QEXM7",
             "Reliability": {"Temperature": 39, "TemperatureMax": 70, "Wear": 4, "PowerOnHours": 6120,
                             "StartStopCycleCount": 902, "ReadErrorsTotal": 0, "ReadErrorsCorrected": 0,
                             "ReadErrorsUncorrected": 0, "WriteErrorsTotal": 0, "WriteErrorsCorrected": 0,
                             "WriteErrorsUncorrected": 0, "ReadLatencyMax": 1012, "WriteLatencyMax": 2210}},
            {"DeviceId": "1", "FriendlyName": "ST3000DM001-1CH166", "SerialNumber": "Z1F2SYNTH1",
             "MediaType": "HDD", "BusType": "SATA", "Size": 3000592982016, "HealthStatus": "Warning",
             "OperationalStatus": "OK", "FirmwareVersion": "CC27",
             "Reliability": {"Temperature": 44, "TemperatureMax": 55, "Wear": 0, "PowerOnHours": 41877,
                             "StartStopCycleCount": 3311, "ReadErrorsTotal": 912, "ReadErrorsCorrected": 905,
                             "ReadErrorsUncorrected": 7, "WriteErrorsTotal": 0, "WriteErrorsCorrected": 0,
                             "WriteErrorsUncorrected": 0, "ReadLatencyMax": 48211, "WriteLatencyMax": 3021}},
            {"DeviceId": "2", "FriendlyName": "WDC WD20EARX-00PASB0", "SerialNumber": "WD-WMAZASYNTH2",
             "MediaType": "HDD", "BusType": "USB", "Size": 2000398934016, "HealthStatus": "Unhealthy",
             "OperationalStatus": "Predictive Failure", "FirmwareVersion": "51.0AB51", "Reliability": None},
        ],
        # ConvertTo-Json-style: a single partition can arrive as an object, not a list.
        "Partitions": [{"DiskNumber": 0, "DriveLetter": "C"}, {"DiskNumber": 1, "DriveLetter": "D"},
                       {"DiskNumber": 2, "DriveLetter": "G"}],
        "Disks": [{"Number": 0, "SerialNumber": "0025_3852_91B0_SYN1.", "FriendlyName": "Samsung SSD 970 EVO Plus 1TB",
                   "BusType": "NVMe", "Size": 1000204886016},
                  {"Number": 1, "SerialNumber": "Z1F2SYNTH1", "FriendlyName": "ST3000DM001-1CH166", "BusType": "SATA",
                   "Size": 3000592982016},
                  {"Number": 2, "SerialNumber": "WD-WMAZASYNTH2", "FriendlyName": "WDC WD20EARX-00PASB0",
                   "BusType": "USB", "Size": 2000398934016}],
    }


def win_letters_single():
    """PS_LETTERS output where ConvertTo-Json collapsed 1-element arrays into objects."""
    return {"_synthetic": SYN,
            "Partitions": {"DiskNumber": 1, "DriveLetter": "D"},
            "Disks": {"Number": 1, "SerialNumber": "  ZFL0SYNTH02", "FriendlyName": "ST2000DM008-2FR102",
                      "BusType": "SATA", "Size": 2000398934016}}


FILES = {
    "hdd_healthy.json": hdd_healthy(),
    "hdd_failing.json": hdd_failing(),
    "ssd_sata_healthy.json": ssd_sata(),
    "nvme_healthy.json": nvme("/dev/nvme0", "Samsung SSD 970 EVO Plus 1TB", "S4EWNX0SYNTH04", 1000204886016,
                              3, 100, 0, 0, 14, 5120, 890, 38, 31250000),
    "nvme_worn.json": nvme("/dev/nvme1", "INTEL SSDPEKNW512G8", "BTNH9SYNTH05", 512110190592,
                           62, 64, 0, 0, 233, 31877, 4410, 49, 612000000, warn_min=95, err_entries=12),
}
LATER = {"hdd_failing.json": hdd_failing(realloc=152, pending=21, offline=19, reported=3, poh=38236)}
WINDOWS = {"windows_inventory.json": win_inventory(), "windows_letters_single.json": win_letters_single()}

if __name__ == "__main__":
    for sub, files in (("", FILES), ("later", LATER), ("windows", WINDOWS)):
        os.makedirs(os.path.join(HERE, sub), exist_ok=True)
        for name, data in files.items():
            with open(os.path.join(HERE, sub, name), "w") as fh:
                json.dump(data, fh, indent=2)
                fh.write("\n")
            print("wrote", os.path.join(sub, name))
