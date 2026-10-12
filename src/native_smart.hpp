// Native SMART backend: reads ATA SMART and the NVMe health log straight from the OS (no smartmontools).
//   Linux:   SG_IO ATA PASS-THROUGH(16) (12-byte fallback) for SATA/SAT incl. USB bridges; NVME_IOCTL_ADMIN_CMD.
//   Windows: IOCTL_ATA_PASS_THROUGH (SMART_RCV_DRIVE_DATA fallback); IOCTL_STORAGE_QUERY_PROPERTY
//            StorageDeviceProtocolSpecificProperty for NVMe identify + log page 02h.
//   macOS:   IOKit IOATASMARTInterface (SATA) and IONVMeSMARTInterface (internal NVMe).
// The raw sectors are decoded into the same JSON shape `smartctl -x --json` produces (the subset
// parse_smartctl reads), so both paths feed one attribute model and one scorer.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "smart.hpp"

namespace hdmon::native {

using Bytes = std::vector<uint8_t>;

// ---- pure decoders (all OSes; unit-tested with byte fixtures)
std::string ata_string(const uint8_t* identify, int first_word, int words);   // byte-swapped ATA string, trimmed
bool ata_checksum_ok(const uint8_t* sector512);                                // byte 511 makes the sum 0 (or 0 = not used)
const char* ata_attribute_name(int id);
json ata_identify_json(const uint8_t* identify512);
// SMART READ DATA + READ THRESHOLDS sectors; `passed` from SMART RETURN STATUS when the drive answered it.
json ata_smart_json(const uint8_t* data512, const uint8_t* thresholds512, std::optional<bool> passed);
json nvme_identify_json(const uint8_t* id_ctrl4096);
json nvme_health_json(const uint8_t* log512);
// Combine the decoded parts with a device block ({"name","type","protocol"}) into one smartctl-shaped object.
// dev_type: the smartctl -d name this read corresponds to ("sat" for SCSI-layer pass-through); defaults to ata/nvme.
json assemble(const std::string& device, const std::string& protocol, const json& identify, const json& smart,
              const std::string& dev_type = "");

// SMART RETURN STATUS: LBA mid/high 4Fh/C2h = passed, F4h/2Ch = threshold exceeded.
std::optional<bool> ata_status_from_lba(uint8_t lba_mid, uint8_t lba_high);
// Linux SG_IO sense data (descriptor 72h with ATA Return descriptor 09h, or fixed 70h) -> LBA mid/high.
bool ata_lba_from_sense(const uint8_t* sense, size_t len, uint8_t& lba_mid, uint8_t& lba_high);

// Debug aid: when HDMON_NATIVE_DUMP=<dir> is set, the live backends write the raw sectors they read
// (<dev>-identify.bin, -smart-data.bin, -smart-thresholds.bin, -nvme-identify.bin, -nvme-log02.bin) there.
void dump_raw(const std::string& device, const char* part, const uint8_t* data, size_t len);

// ---- live access
struct Device {
    std::string name;        // /dev/sda, /dev/nvme0, PhysicalDrive0, /dev/disk0
    std::string path;        // what gets opened (\\.\PhysicalDrive0, /dev/nvme0, IOKit BSD name)
    std::string protocol;    // ATA | NVMe | SCSI | RAID | Spaces | Unknown (hint)
    std::optional<std::string> model, serial, transport;
    std::optional<int64_t> capacity_bytes;
    std::optional<bool> rotational;
    std::vector<std::string> volumes;
    bool virtual_disk = false;   // RAID volume / Storage Space / md: health comes from the array, not SMART
};

struct ReadResult {
    bool ok = false;
    json data;                     // smartctl-shaped JSON when ok
    std::string method;            // e.g. "SG_IO ATA PASS-THROUGH(16)", "NVMe admin ioctl"
    std::string error;
    bool permission_denied = false;
    bool unsupported = false;      // device answered but does not speak ATA/NVMe SMART (e.g. SAS, some USB)
};

const char* backend_name();                     // "Linux SG_IO / NVMe ioctl", ...
std::vector<Device> list_devices();             // whole disks the OS knows about
ReadResult read_device(const Device& dev);
std::string permission_hint();
// Drive in the shared model from a ReadResult (same parse_smartctl as the smartctl path).
Drive to_drive(const Device& dev, const ReadResult& r);

}  // namespace hdmon::native
