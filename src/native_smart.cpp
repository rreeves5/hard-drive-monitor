#include "native_smart.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>

namespace hdmon::native {

// ------------------------------------------------------------------ little-endian helpers
static uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t* p) { return le16(p) | (static_cast<uint32_t>(le16(p + 2)) << 16); }
static uint64_t le48(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 5; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
static int64_t le128_clamped(const uint8_t* p) {   // NVMe 16-byte counters; JSON holds int64
    for (int i = 8; i < 16; ++i)
        if (p[i]) return std::numeric_limits<int64_t>::max();
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ? std::numeric_limits<int64_t>::max()
                                                                          : static_cast<int64_t>(v);
}
static uint16_t word(const uint8_t* id, int w) { return le16(id + 2 * w); }

static std::string clean_ascii(const std::string& s) {
    std::string o;
    for (char c : s) o += (static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7F) ? c : ' ';
    return trim(o);
}

std::string ata_string(const uint8_t* id, int first_word, int words) {
    std::string s;
    for (int w = first_word; w < first_word + words; ++w) {
        s += static_cast<char>(id[2 * w + 1]);
        s += static_cast<char>(id[2 * w]);
    }
    return clean_ascii(s);
}

bool ata_checksum_ok(const uint8_t* sector) {
    uint8_t sum = 0;
    for (int i = 0; i < 512; ++i) sum = static_cast<uint8_t>(sum + sector[i]);
    return sum == 0;
}

const char* ata_attribute_name(int id) {   // smartctl's default attribute names
    switch (id) {
        case 1: return "Raw_Read_Error_Rate"; case 2: return "Throughput_Performance";
        case 3: return "Spin_Up_Time"; case 4: return "Start_Stop_Count";
        case 5: return "Reallocated_Sector_Ct"; case 7: return "Seek_Error_Rate";
        case 8: return "Seek_Time_Performance"; case 9: return "Power_On_Hours";
        case 10: return "Spin_Retry_Count"; case 11: return "Calibration_Retry_Count";
        case 12: return "Power_Cycle_Count"; case 13: return "Read_Soft_Error_Rate";
        case 160: return "Uncorrectable_Error_Cnt"; case 161: return "Valid_Spare_Block_Cnt";
        case 163: return "Initial_Bad_Block_Count"; case 164: return "Total_Erase_Count";
        case 165: return "Max_Erase_Count"; case 166: return "Min_Erase_Count";
        case 167: return "Average_Erase_Count"; case 168: return "Max_Erase_Count_of_Spec";
        case 169: return "Remaining_Lifetime_Perc"; case 170: return "Available_Reservd_Space";
        case 171: return "Program_Fail_Count"; case 172: return "Erase_Fail_Count";
        case 173: return "Wear_Leveling_Count"; case 174: return "Unexpect_Power_Loss_Ct";
        case 175: return "Program_Fail_Count_Chip"; case 176: return "Erase_Fail_Count_Chip";
        case 177: return "Wear_Leveling_Count"; case 178: return "Used_Rsvd_Blk_Cnt_Chip";
        case 179: return "Used_Rsvd_Blk_Cnt_Tot"; case 180: return "Unused_Rsvd_Blk_Cnt_Tot";
        case 181: return "Program_Fail_Cnt_Total"; case 182: return "Erase_Fail_Count_Total";
        case 183: return "Runtime_Bad_Block"; case 184: return "End-to-End_Error";
        case 187: return "Reported_Uncorrect"; case 188: return "Command_Timeout";
        case 189: return "High_Fly_Writes"; case 190: return "Airflow_Temperature_Cel";
        case 191: return "G-Sense_Error_Rate"; case 192: return "Power-Off_Retract_Count";
        case 193: return "Load_Cycle_Count"; case 194: return "Temperature_Celsius";
        case 195: return "Hardware_ECC_Recovered"; case 196: return "Reallocated_Event_Count";
        case 197: return "Current_Pending_Sector"; case 198: return "Offline_Uncorrectable";
        case 199: return "UDMA_CRC_Error_Count"; case 200: return "Multi_Zone_Error_Rate";
        case 201: return "Soft_Read_Error_Rate"; case 202: return "Data_Address_Mark_Errs";
        case 220: return "Disk_Shift"; case 222: return "Loaded_Hours";
        case 223: return "Load_Retry_Count"; case 224: return "Load_Friction";
        case 225: return "Load_Cycle_Count"; case 226: return "Load-in_Time";
        case 230: return "Head_Amplitude"; case 231: return "Temperature_Celsius";
        case 232: return "Available_Reservd_Space"; case 233: return "Media_Wearout_Indicator";
        case 235: return "Unknown_Attribute"; case 240: return "Head_Flying_Hours";
        case 241: return "Total_LBAs_Written"; case 242: return "Total_LBAs_Read";
        case 250: return "Read_Error_Retry_Rate"; case 254: return "Free_Fall_Sensor";
        default: return "Unknown_Attribute";
    }
}

json ata_identify_json(const uint8_t* id) {
    json j = json::object();
    j["model_name"] = ata_string(id, 27, 20);
    j["serial_number"] = ata_string(id, 10, 10);
    j["firmware_version"] = ata_string(id, 23, 4);
    uint64_t sectors = (word(id, 83) & (1u << 10))
                           ? (static_cast<uint64_t>(word(id, 100)) | (static_cast<uint64_t>(word(id, 101)) << 16) |
                              (static_cast<uint64_t>(word(id, 102)) << 32) | (static_cast<uint64_t>(word(id, 103)) << 48))
                           : (static_cast<uint64_t>(word(id, 60)) | (static_cast<uint64_t>(word(id, 61)) << 16));
    if (sectors == 0) sectors = static_cast<uint64_t>(word(id, 60)) | (static_cast<uint64_t>(word(id, 61)) << 16);
    uint64_t logical = 512, physical = 512;
    uint16_t w106 = word(id, 106);
    if ((w106 & 0xC000) == 0x4000) {
        if (w106 & (1u << 12)) {
            uint64_t words = static_cast<uint64_t>(word(id, 117)) | (static_cast<uint64_t>(word(id, 118)) << 16);
            if (words >= 256) logical = words * 2;
        }
        physical = logical << (w106 & 0x0F);
    }
    j["user_capacity"] = {{"blocks", sectors}, {"bytes", sectors * logical}};
    j["logical_block_size"] = logical;
    j["physical_block_size"] = physical;
    uint16_t rpm = word(id, 217);
    if (rpm == 1) j["rotation_rate"] = 0;                       // non-rotating (SSD)
    else if (rpm >= 0x0401 && rpm <= 0xFFFE) j["rotation_rate"] = rpm;
    static const char* FF[] = {nullptr, "5.25 inches", "3.5 inches", "2.5 inches", "1.8 inches",
                               "< 1.8 inches", "mSATA", "M.2", "MicroSSD", "CFast"};
    int ff = word(id, 168) & 0x0F;
    if (ff >= 1 && ff <= 9) j["form_factor"] = {{"ata_value", ff}, {"name", FF[ff]}};
    uint16_t w222 = word(id, 222);
    if ((w222 >> 12) == 1) {   // serial transport
        static const char* SATA[] = {"ATA8-AST", "SATA 1.0a", "SATA II Ext", "SATA 2.5", "SATA 2.6",
                                     "SATA 3.0", "SATA 3.1", "SATA 3.2", "SATA 3.3", "SATA 3.4", "SATA 3.5"};
        for (int b = 11; b >= 1; --b)
            if (w222 & (1u << b)) { j["sata_version"] = {{"string", SATA[b - 1]}, {"value", w222 & 0x0FFF}}; break; }
    }
    uint16_t w76 = word(id, 76);
    if (w76 != 0 && w76 != 0xFFFF) {
        const char* sp = (w76 & 8) ? "6.0 Gb/s" : (w76 & 4) ? "3.0 Gb/s" : (w76 & 2) ? "1.5 Gb/s" : nullptr;
        if (sp) j["interface_speed"] = {{"max", {{"string", sp}}}};
    }
    bool supported = word(id, 82) & 1, enabled = word(id, 85) & 1;
    j["smart_support"] = {{"available", supported}, {"enabled", enabled}};
    return j;
}

static std::string flags_string(uint16_t f) {
    std::string s;
    s += (f & 0x01) ? 'P' : '-';
    s += (f & 0x02) ? 'O' : '-';
    s += (f & 0x04) ? 'S' : '-';
    s += (f & 0x08) ? 'R' : '-';
    s += (f & 0x10) ? 'C' : '-';
    s += (f & 0x20) ? 'K' : '-';
    return s + " ";
}

static const char* selftest_text(int code) {
    switch (code) {
        case 0: return "completed without error";
        case 1: return "aborted by host";
        case 2: return "interrupted (host reset)";
        case 3: return "fatal error or unknown test error";
        case 4: return "completed: unknown failure";
        case 5: return "completed: electrical failure";
        case 6: return "completed: servo/seek failure";
        case 7: return "completed: read failure";
        case 8: return "completed: handling damage??";
        case 15: return "in progress";
        default: return "unknown";
    }
}

std::optional<bool> ata_status_from_lba(uint8_t mid, uint8_t high) {
    if (mid == 0x4F && high == 0xC2) return true;
    if (mid == 0xF4 && high == 0x2C) return false;
    return std::nullopt;
}

// smartctl's RAWFMT_TEMPMINMAX (atacmds.cpp), so the raw string matches byte for byte.
static int check_temp_word(unsigned w) {
    if (w <= 0x7f) return 0x11;
    if (w <= 0xff) return 0x01;
    if (0xff80 <= w) return 0x10;
    return 0x00;
}
static bool check_temp_range(int t, unsigned char u1, unsigned char u2, int& lo, int& hi) {
    int t1 = static_cast<signed char>(u1), t2 = static_cast<signed char>(u2);
    if (t1 > t2) std::swap(t1, t2);
    if (-60 <= t1 && t1 <= t && t <= t2 && t2 <= 120 && !(t1 == -1 && t2 <= 0)) { lo = t1; hi = t2; return true; }
    return false;
}
static void temp_text(const uint8_t* raw, std::string& s, int& t) {
    unsigned word[3] = {static_cast<unsigned>(raw[0] | raw[1] << 8), static_cast<unsigned>(raw[2] | raw[3] << 8),
                        static_cast<unsigned>(raw[4] | raw[5] << 8)};
    t = static_cast<signed char>(raw[0]);
    int lo = 0, hi = 0, fmt;
    int ctw0 = check_temp_word(word[0]);
    if (!word[2]) {
        if (!word[1] && ctw0) fmt = 0;
        else if (ctw0 && check_temp_range(t, raw[2], raw[3], lo, hi)) fmt = 1;
        else if (!raw[3] && check_temp_range(t, raw[1], raw[2], lo, hi)) fmt = 2;
        else fmt = -1;
    } else if (ctw0) {
        if ((ctw0 & check_temp_word(word[1]) & check_temp_word(word[2])) != 0 && check_temp_range(t, raw[2], raw[4], lo, hi)) fmt = 3;
        else if (word[2] < 0x7fff && check_temp_range(t, raw[2], raw[3], lo, hi) && hi >= 40) fmt = 4;
        else fmt = -2;
    } else fmt = -3;
    char buf[96];
    if (fmt == 0) std::snprintf(buf, sizeof buf, "%d", t);
    else if (fmt >= 1 && fmt <= 3) std::snprintf(buf, sizeof buf, "%d (Min/Max %d/%d)", t, lo, hi);
    else if (fmt == 4) std::snprintf(buf, sizeof buf, "%d (Min/Max %d/%d #%u)", t, lo, hi, word[2]);
    else std::snprintf(buf, sizeof buf, "%d (%d %d %d %d %d)", raw[0], raw[5], raw[4], raw[3], raw[2], raw[1]);
    s = buf;
}

json ata_smart_json(const uint8_t* data, const uint8_t* thr, std::optional<bool> passed) {
    json j = json::object();
    json table = json::array();
    bool any_failing_prefail = false;
    std::optional<int64_t> temp, hours, cycles;
    for (int i = 0; i < 30; ++i) {
        const uint8_t* e = data + 2 + i * 12;
        int id = e[0];
        if (id == 0) continue;
        uint16_t flags = le16(e + 1);
        int value = e[3], worst = e[4];
        uint64_t raw = le48(e + 5);
        int thresh = 0;
        if (thr)
            for (int k = 0; k < 30; ++k)
                if (thr[2 + k * 12] == id) { thresh = thr[2 + k * 12 + 1]; break; }
        bool prefail = flags & 1;
        std::string wf;
        bool valid = value >= 1 && value <= 0xFD;
        if (thresh > 0 && valid && value <= thresh) wf = "now";
        else if (thresh > 0 && worst >= 1 && worst <= 0xFD && worst <= thresh) wf = "past";
        if (wf == "now" && prefail) any_failing_prefail = true;
        std::string rs = std::to_string(raw);
        if (id == 194 || id == 190) {
            int t = 0;
            temp_text(e + 5, rs, t);
            temp = temp ? temp : std::optional<int64_t>(t);
        } else if (id == 9) {
            hours = static_cast<int64_t>(raw & 0xFFFFFFFFull);
            rs = std::to_string(*hours);
        } else if (id == 12) {
            cycles = static_cast<int64_t>(raw);
        }
        table.push_back({{"id", id}, {"name", ata_attribute_name(id)}, {"value", value}, {"worst", worst},
                         {"thresh", thresh}, {"when_failed", wf},
                         {"flags", {{"value", flags}, {"string", flags_string(flags)}, {"prefailure", prefail},
                                    {"updated_online", (flags & 2) != 0}, {"performance", (flags & 4) != 0},
                                    {"error_rate", (flags & 8) != 0}, {"event_count", (flags & 16) != 0},
                                    {"auto_keep", (flags & 32) != 0}}},
                         {"raw", {{"value", raw}, {"string", rs}}}});
    }
    j["ata_smart_attributes"] = {{"revision", le16(data)}, {"table", table}};
    int st = data[363];
    json status = {{"value", st}, {"string", selftest_text(st >> 4)}, {"passed", (st >> 4) == 0}};
    if ((st >> 4) == 0x0F) status["remaining_percent"] = (st & 0x0F) * 10;
    int ext = data[373] == 0xFF ? le16(data + 375) : data[373];
    json polling = json::object();
    if (data[372]) polling["short"] = data[372];
    if (ext) polling["extended"] = ext;
    if (data[374] && (data[367] & 0x20)) polling["conveyance"] = data[374];  // only when conveyance self-test is supported
    j["ata_smart_data"] = {{"offline_data_collection", {{"status", {{"value", data[362]}}}}},
                           {"self_test", {{"status", status}, {"polling_minutes", polling}}},
                           {"capabilities", {{"values", {data[367], le16(data + 368)}}}}};
    if (temp) j["temperature"] = {{"current", *temp}};
    if (hours) j["power_on_time"] = {{"hours", *hours}};
    if (cycles) j["power_cycle_count"] = *cycles;
    // smartctl rule: the drive's own verdict when it answers RETURN STATUS, else any prefail attribute at/below threshold.
    bool ok = passed.value_or(!any_failing_prefail);
    j["smart_status"] = {{"passed", ok}};
    if (!passed) j["smart_status"]["source"] = "attributes";
    return j;
}

json nvme_identify_json(const uint8_t* id) {
    json j = json::object();
    auto str = [&](int off, int len) { return clean_ascii(std::string(reinterpret_cast<const char*>(id + off), len)); };
    j["model_name"] = str(24, 40);
    j["serial_number"] = str(4, 20);
    j["firmware_version"] = str(64, 8);
    j["nvme_pci_vendor"] = {{"id", le16(id)}, {"subsystem_id", le16(id + 2)}};
    int64_t cap = le128_clamped(id + 280);
    if (cap > 0) {
        j["nvme_total_capacity"] = cap;
        j["user_capacity"] = {{"bytes", cap}};
    }
    int wctemp = le16(id + 266), cctemp = le16(id + 268);
    json t = json::object();
    if (wctemp > 0) t["op_limit_max"] = wctemp - 273;
    if (cctemp > 0) t["critical_limit_max"] = cctemp - 273;
    if (!t.empty()) j["temperature"] = t;
    j["nvme_number_of_namespaces"] = le32(id + 516);
    return j;
}

json nvme_health_json(const uint8_t* p) {
    json log = json::object();
    int cw = p[0];
    int temp_k = le16(p + 1);
    log["critical_warning"] = cw;
    log["temperature"] = temp_k ? temp_k - 273 : 0;
    log["available_spare"] = p[3];
    log["available_spare_threshold"] = p[4];
    log["percentage_used"] = p[5];
    log["data_units_read"] = le128_clamped(p + 32);
    log["data_units_written"] = le128_clamped(p + 48);
    log["host_reads"] = le128_clamped(p + 64);
    log["host_writes"] = le128_clamped(p + 80);
    log["controller_busy_time"] = le128_clamped(p + 96);
    log["power_cycles"] = le128_clamped(p + 112);
    log["power_on_hours"] = le128_clamped(p + 128);
    log["unsafe_shutdowns"] = le128_clamped(p + 144);
    log["media_errors"] = le128_clamped(p + 160);
    log["num_err_log_entries"] = le128_clamped(p + 176);
    log["warning_temp_time"] = le32(p + 192);
    log["critical_comp_time"] = le32(p + 196);
    json sensors = json::array();
    for (int i = 0; i < 8; ++i) {
        int k = le16(p + 200 + 2 * i);
        if (k) sensors.push_back(k - 273);
    }
    if (!sensors.empty()) log["temperature_sensors"] = sensors;
    json j = json::object();
    j["nvme_smart_health_information_log"] = log;
    j["smart_status"] = {{"passed", cw == 0}, {"nvme", {{"value", cw}}}};
    if (temp_k) j["temperature"] = {{"current", temp_k - 273}};
    j["power_on_time"] = {{"hours", log["power_on_hours"]}};
    j["power_cycle_count"] = log["power_cycles"];
    return j;
}

static void merge(json& dst, const json& src) {
    for (auto it = src.begin(); it != src.end(); ++it) {
        if (dst.contains(it.key()) && dst[it.key()].is_object() && it.value().is_object()) merge(dst[it.key()], it.value());
        else dst[it.key()] = it.value();
    }
}

json assemble(const std::string& device, const std::string& protocol, const json& identify, const json& smart,
              const std::string& dev_type) {
    json j = json::object();
    std::string type = !dev_type.empty() ? dev_type : (protocol == "NVMe" ? "nvme" : "ata");
    j["device"] = {{"name", device}, {"type", type}, {"protocol", protocol}};
    merge(j, identify);
    merge(j, smart);
    if (protocol == "ATA" && !j.contains("smart_support")) j["smart_support"] = {{"available", true}};
    return j;
}

bool ata_lba_from_sense(const uint8_t* s, size_t len, uint8_t& mid, uint8_t& high) {
    if (len < 8) return false;
    uint8_t code = s[0] & 0x7F;
    if (code == 0x72 || code == 0x73) {            // descriptor format: find the ATA Status Return descriptor (09h)
        size_t add = s[7], off = 8;
        while (off + 1 < len && off < 8 + add) {
            uint8_t type = s[off], dl = s[off + 1];
            if (type == 0x09 && dl >= 0x0C && off + 13 < len) {
                mid = s[off + 9];
                high = s[off + 11];
                return true;
            }
            off += 2 + dl;
        }
        return false;
    }
    if ((code == 0x70 || code == 0x71) && len >= 12) {   // fixed format (SAT-2): bytes 9..11 = LBA low/mid/high
        mid = s[10];
        high = s[11];
        return true;
    }
    return false;
}

Drive to_drive(const Device& dev, const ReadResult& r) {
    Drive d = parse_smartctl(r.data, dev.name);
    d.source = "native (" + r.method + ")";
    if (d.volumes.empty()) d.volumes = dev.volumes;
    if (!d.model || d.model->empty()) d.model = dev.model;
    if (!d.serial || d.serial->empty()) d.serial = dev.serial;
    if (!d.capacity_bytes || *d.capacity_bytes == 0) d.capacity_bytes = dev.capacity_bytes;
    if (d.kind == "Unknown" && dev.rotational) d.kind = *dev.rotational ? "HDD" : "SSD";
    if (jget(jget(r.data, "smart_status"), "source") == "attributes")
        d.notes.push_back("SMART RETURN STATUS not available through this path; verdict derived from attribute thresholds");
    return d;
}

}  // namespace hdmon::native

namespace hdmon::native {
void dump_raw(const std::string& device, const char* part, const uint8_t* data, size_t len) {
    const char* dir = std::getenv("HDMON_NATIVE_DUMP");
    if (!dir || !*dir || !data) return;
    std::string base = device;
    for (char& c : base) if (c == '/' || c == '\\' || c == ':') c = '_';
    while (!base.empty() && base[0] == '_') base.erase(0, 1);
    std::ofstream f(std::string(dir) + "/" + base + "-" + part + ".bin", std::ios::binary);
    f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
}
}  // namespace hdmon::native
