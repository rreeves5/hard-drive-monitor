// Native SMART on Linux: /sys/block enumeration, SG_IO ATA PASS-THROUGH, NVMe admin ioctl.
#include "native_smart.hpp"

#if defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <linux/nvme_ioctl.h>
#include <scsi/sg.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <map>
#include <regex>

namespace hdmon::native {

const char* backend_name() { return "Linux SG_IO / NVMe ioctl"; }

std::string permission_hint() {
    return "native SMART needs root or three capabilities: run with sudo, or grant the binary once with "
           "`sudo setcap cap_sys_rawio,cap_sys_admin,cap_dac_read_search+ep \"$(command -v hdmon)\"`";
}

static std::string sys_read(const std::string& p) { return trim(read_file(p)); }

static std::vector<std::string> dir_entries(const std::string& p) {
    std::vector<std::string> out;
    if (DIR* d = opendir(p.c_str())) {
        while (auto* e = readdir(d)) {
            std::string n = e->d_name;
            if (n != "." && n != "..") out.push_back(n);
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// /proc/mounts: partition device -> mount points (octal escapes decoded for spaces)
static std::map<std::string, std::vector<std::string>> mounts() {
    std::map<std::string, std::vector<std::string>> m;
    for (auto& line : split_lines(read_file("/proc/mounts"))) {
        auto f = split(line, ' ');
        if (f.size() < 2 || !starts_with(f[0], "/dev/")) continue;
        std::string mp = replace_all(replace_all(f[1], "\\040", " "), "\\011", "\t");
        m[f[0].substr(5)].push_back(mp);
    }
    return m;
}

static std::string vpd_serial(const std::string& blk) {   // SCSI VPD page 80h: unit serial number
    std::string raw = read_file("/sys/block/" + blk + "/device/vpd_pg80");
    if (raw.size() <= 4) return "";
    return trim(raw.substr(4));
}

std::vector<Device> list_devices() {
    std::vector<Device> out;
    auto mnt = mounts();
    static const std::regex keep(R"(^(sd[a-z]+|hd[a-z]+|nvme\d+n\d+)$)");
    std::map<std::string, bool> seen_ctrl;
    for (auto& blk : dir_entries("/sys/block")) {
        if (!std::regex_match(blk, keep)) continue;
        std::string base = "/sys/block/" + blk;
        Device d;
        bool nvme = starts_with(blk, "nvme");
        if (nvme) {
            std::string ctrl = blk.substr(0, blk.find('n', 4));   // nvme0n1 -> nvme0
            if (seen_ctrl[ctrl]) continue;
            seen_ctrl[ctrl] = true;
            d.name = "/dev/" + ctrl;
            d.path = "/dev/" + ctrl;
            d.protocol = "NVMe";
            d.transport = "nvme";
            std::string m = sys_read(base + "/device/model");
            if (m.empty()) m = sys_read("/sys/class/nvme/" + ctrl + "/model");
            if (!m.empty()) d.model = m;
            std::string s = sys_read("/sys/class/nvme/" + ctrl + "/serial");
            if (!s.empty()) d.serial = s;
        } else {
            d.name = "/dev/" + blk;
            d.path = d.name;
            std::string vendor = sys_read(base + "/device/vendor");
            std::string model = sys_read(base + "/device/model");
            d.protocol = vendor == "ATA" ? "ATA" : "SCSI";
            if (!model.empty()) d.model = (vendor.empty() || vendor == "ATA") ? model : vendor + " " + model;
            std::string s = vpd_serial(blk);
            if (!s.empty()) d.serial = s;
            char link[512] = {0};
            ssize_t n = readlink((base).c_str(), link, sizeof link - 1);
            std::string lp = n > 0 ? std::string(link, n) : "";
            if (contains(lp, "/usb")) d.transport = "usb";
            else if (contains(lp, "/ata")) d.transport = "sata";
            else d.transport = "scsi";
        }
        std::string sz = sys_read(base + "/size");
        if (!sz.empty()) try { d.capacity_bytes = std::stoll(sz) * 512; } catch (...) {}
        std::string rot = sys_read(base + "/queue/rotational");
        if (!rot.empty()) d.rotational = rot == "1";
        for (auto& part : dir_entries(base))
            if (starts_with(part, blk)) {
                auto it = mnt.find(part);
                if (it != mnt.end()) d.volumes.insert(d.volumes.end(), it->second.begin(), it->second.end());
            }
        if (auto it = mnt.find(blk); it != mnt.end()) d.volumes.insert(d.volumes.end(), it->second.begin(), it->second.end());
        out.push_back(d);
    }
    return out;
}

// ------------------------------------------------------------------ SG_IO ATA pass-through
namespace {
enum class Rc { Ok, Denied, Unsupported, Failed };

struct AtaReg { uint8_t command, feature, count, lba_low, lba_mid, lba_high; };

Rc ata_passthrough(int fd, bool cdb16, const AtaReg& r, uint8_t* buf, uint8_t* lba_mid_out, uint8_t* lba_high_out,
                   std::string& err) {
    uint8_t cdb[16] = {0};
    uint8_t sense[64] = {0};
    bool data_in = buf != nullptr;
    uint8_t proto = data_in ? 4 : 3;                // PIO data-in / non-data
    uint8_t flags = data_in ? 0x0E : 0x20;          // t_dir=in, byt_blok, t_length=count | ck_cond for non-data
    if (cdb16) {
        cdb[0] = 0x85; cdb[1] = static_cast<uint8_t>(proto << 1); cdb[2] = flags;
        cdb[4] = r.feature; cdb[6] = r.count; cdb[8] = r.lba_low; cdb[10] = r.lba_mid; cdb[12] = r.lba_high;
        cdb[13] = 0x40; cdb[14] = r.command;
    } else {
        cdb[0] = 0xA1; cdb[1] = static_cast<uint8_t>(proto << 1); cdb[2] = flags;
        cdb[3] = r.feature; cdb[4] = r.count; cdb[5] = r.lba_low; cdb[6] = r.lba_mid; cdb[7] = r.lba_high;
        cdb[8] = 0x40; cdb[9] = r.command;
    }
    sg_io_hdr_t io{};
    io.interface_id = 'S';
    io.cmd_len = cdb16 ? 16 : 12;
    io.cmdp = cdb;
    io.mx_sb_len = sizeof sense;
    io.sbp = sense;
    io.dxfer_direction = data_in ? SG_DXFER_FROM_DEV : SG_DXFER_NONE;
    io.dxfer_len = data_in ? 512 : 0;
    io.dxferp = buf;
    io.timeout = 20000;
    if (ioctl(fd, SG_IO, &io) < 0) {
        int e = errno;
        err = std::string("SG_IO: ") + std::strerror(e);
        return (e == EACCES || e == EPERM) ? Rc::Denied : Rc::Failed;
    }
    if (io.host_status != 0) { err = "SG_IO host status " + std::to_string(io.host_status); return Rc::Failed; }
    uint8_t key = 0;
    if (io.sb_len_wr > 0) key = ((sense[0] & 0x7F) >= 0x72) ? (sense[1] & 0x0F) : (sense[2] & 0x0F);
    if (io.status == 0 && key == 0) {
        if (lba_mid_out && io.sb_len_wr > 0) ata_lba_from_sense(sense, io.sb_len_wr, *lba_mid_out, *lba_high_out);
        return Rc::Ok;
    }
    if (key == 0x01 || (key == 0 && io.status == 2)) {   // RECOVERED ERROR + ATA return descriptor (ck_cond)
        if (lba_mid_out) {
            if (!ata_lba_from_sense(sense, io.sb_len_wr, *lba_mid_out, *lba_high_out)) {
                err = "no ATA return descriptor";
                return Rc::Failed;
            }
        }
        return Rc::Ok;
    }
    if (key == 0x05) { err = "device rejected ATA PASS-THROUGH (not an ATA/SAT device)"; return Rc::Unsupported; }
    char b[96];
    std::snprintf(b, sizeof b, "SCSI status 0x%02x sense key 0x%x", io.status, key);
    err = b;
    return key == 0x0B || key == 0x02 ? Rc::Failed : Rc::Unsupported;
}

bool word_is_atapi(const uint8_t* id) { return (id[1] & 0x80) != 0; }   // word 0 bit 15: ATAPI / not ATA

int open_dev(const std::string& path, int& err_no) {
    int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    err_no = fd < 0 ? errno : 0;
    return fd;
}
}  // namespace

static ReadResult read_ata(const Device& dev) {
    ReadResult r;
    int en = 0;
    int fd = open_dev(dev.path, en);
    if (fd < 0) {
        r.error = "open " + dev.path + ": " + std::strerror(en);
        r.permission_denied = en == EACCES || en == EPERM;
        return r;
    }
    uint8_t id[512] = {0}, data[512] = {0}, thr[512] = {0};
    std::string err;
    bool use16 = true;
    Rc rc = ata_passthrough(fd, true, {0xEC, 0, 1, 0, 0, 0}, id, nullptr, nullptr, err);
    if (rc == Rc::Unsupported || rc == Rc::Failed) {   // some USB bridges only take the 12-byte CDB
        std::string err12;
        Rc rc12 = ata_passthrough(fd, false, {0xEC, 0, 1, 0, 0, 0}, id, nullptr, nullptr, err12);
        if (rc12 == Rc::Ok) { rc = rc12; use16 = false; }
    }
    if (rc != Rc::Ok || (word_is_atapi(id))) {
        ::close(fd);
        r.error = err.empty() ? "IDENTIFY DEVICE failed" : err;
        r.permission_denied = rc == Rc::Denied;
        r.unsupported = rc == Rc::Unsupported || rc == Rc::Failed;
        return r;
    }
    r.method = use16 ? "SG_IO ATA PASS-THROUGH(16)" : "SG_IO ATA PASS-THROUGH(12)";
    json ident = ata_identify_json(id);
    if (!truthy(jget(jget(ident, "smart_support"), "available"))) {
        ::close(fd);
        r.ok = true;
        r.data = assemble(dev.name, "ATA", ident, json::object(), "sat");
        r.data["smart_support"] = {{"available", false}};
        return r;
    }
    if (!truthy(jget(jget(ident, "smart_support"), "enabled")))   // SMART ENABLE OPERATIONS (non-destructive)
        ata_passthrough(fd, use16, {0xB0, 0xD8, 0, 0, 0x4F, 0xC2}, nullptr, nullptr, nullptr, err);
    Rc rd = ata_passthrough(fd, use16, {0xB0, 0xD0, 1, 0, 0x4F, 0xC2}, data, nullptr, nullptr, err);
    Rc rt = ata_passthrough(fd, use16, {0xB0, 0xD1, 1, 0, 0x4F, 0xC2}, thr, nullptr, nullptr, err);
    uint8_t mid = 0, high = 0;
    Rc rs = ata_passthrough(fd, use16, {0xB0, 0xDA, 0, 0, 0x4F, 0xC2}, nullptr, &mid, &high, err);
    uint8_t stlog[512] = {0};
    std::string lerr;   // SMART READ LOG 06h (self-test log); optional, older drives may not have it
    Rc rl = ata_passthrough(fd, use16, {0xB0, 0xD5, 1, 0x06, 0x4F, 0xC2}, stlog, nullptr, nullptr, lerr);
    ::close(fd);
    if (rl == Rc::Ok) dump_raw(dev.name, "smart-selftest-log", stlog, sizeof stlog);
    dump_raw(dev.name, "identify", id, sizeof id);
    if (rd == Rc::Ok) dump_raw(dev.name, "smart-data", data, sizeof data);
    if (rt == Rc::Ok) dump_raw(dev.name, "smart-thresholds", thr, sizeof thr);
    if (rd != Rc::Ok) {
        r.error = "SMART READ DATA failed: " + err;
        r.permission_denied = rd == Rc::Denied;
        return r;
    }
    std::optional<bool> passed = rs == Rc::Ok ? ata_status_from_lba(mid, high) : std::nullopt;
    r.ok = true;
    r.data = assemble(dev.name, "ATA", ident, ata_smart_json(data, rt == Rc::Ok ? thr : nullptr, passed), "sat");
    if (rl == Rc::Ok) r.data["ata_smart_self_test_log"] = {{"standard", ata_selftest_log_json(stlog)}};
    if (!ata_checksum_ok(data)) r.data["_native_warning"] = "SMART data checksum mismatch";
    return r;
}

static ReadResult read_nvme(const Device& dev) {
    ReadResult r;
    int en = 0;
    int fd = open_dev(dev.path, en);
    if (fd < 0) {   // char device is root-only on most distros; namespace block device is an alternative
        int en2 = 0;
        fd = open_dev(dev.path + "n1", en2);
        if (fd < 0) {
            r.error = "open " + dev.path + ": " + std::strerror(en);
            r.permission_denied = en == EACCES || en == EPERM || en2 == EACCES || en2 == EPERM;
            return r;
        }
    }
    alignas(8) uint8_t id[4096] = {0};
    alignas(8) uint8_t log[512] = {0};
    nvme_admin_cmd c{};
    c.opcode = 0x06;   // IDENTIFY, CNS 01h = controller
    c.addr = reinterpret_cast<uintptr_t>(id);
    c.data_len = sizeof id;
    c.cdw10 = 1;
    int rc1 = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &c);
    int e1 = errno;
    nvme_admin_cmd g{};
    g.opcode = 0x02;   // GET LOG PAGE, LID 02h SMART / Health Information, controller scope
    g.nsid = 0xFFFFFFFF;
    g.addr = reinterpret_cast<uintptr_t>(log);
    g.data_len = sizeof log;
    g.cdw10 = ((sizeof log / 4 - 1) << 16) | 0x02;
    int rc2 = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &g);
    int e2 = errno;
    alignas(8) uint8_t stlog[564] = {0};
    nvme_admin_cmd sl{};
    sl.opcode = 0x02;   // GET LOG PAGE, LID 06h Device Self-test (optional: OACS bit 4)
    sl.nsid = 0xFFFFFFFF;
    sl.addr = reinterpret_cast<uintptr_t>(stlog);
    sl.data_len = sizeof stlog;
    sl.cdw10 = ((sizeof stlog / 4 - 1) << 16) | 0x06;
    bool have_stlog = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &sl) == 0;
    ::close(fd);
    if (have_stlog) dump_raw(dev.name, "nvme-log06", stlog, sizeof stlog);
    if (rc2 != 0) {
        r.error = rc2 < 0 ? std::string("NVMe GET LOG PAGE: ") + std::strerror(e2) : "NVMe status " + std::to_string(rc2);
        r.permission_denied = rc2 < 0 && (e2 == EACCES || e2 == EPERM);
        return r;
    }
    if (rc1 == 0) dump_raw(dev.name, "nvme-identify", id, sizeof id);
    dump_raw(dev.name, "nvme-log02", log, sizeof log);
    r.ok = true;
    r.method = "NVMe admin ioctl";
    r.data = assemble(dev.name, "NVMe", rc1 == 0 ? nvme_identify_json(id) : json::object(), nvme_health_json(log));
    if (have_stlog) r.data["nvme_self_test_log"] = nvme_selftest_log_json(stlog);
    (void)e1;
    return r;
}

SelfTestResult start_self_test(const Device& dev, const std::string& kind) {
    SelfTestResult r;
    int en = 0;
    int fd = open_dev(dev.path, en);
    if (fd < 0 && dev.protocol == "NVMe") fd = open_dev(dev.path + "n1", en);
    if (fd < 0) {
        r.error = "open " + dev.path + ": " + std::strerror(en);
        r.permission_denied = en == EACCES || en == EPERM;
        return r;
    }
    if (dev.protocol == "NVMe") {
        auto code = nvme_selftest_code(kind);
        if (!code) { ::close(fd); r.unsupported = true; r.error = "NVMe has no '" + kind + "' self-test"; return r; }
        nvme_admin_cmd c{};
        c.opcode = 0x14;   // Device Self-test
        c.nsid = 0xFFFFFFFF;
        c.cdw10 = *code;
        int rc = ioctl(fd, NVME_IOCTL_ADMIN_CMD, &c);
        int e = errno;
        ::close(fd);
        r.method = "NVMe admin ioctl (Device Self-test 14h)";
        if (rc < 0) { r.error = std::string("NVMe ioctl: ") + std::strerror(e); r.permission_denied = e == EACCES || e == EPERM; return r; }
        if (rc != 0) {
            char b[64];
            std::snprintf(b, sizeof b, "NVMe status 0x%x", rc);
            r.error = b;
            if ((rc & 0x7FF) == 0x01) { r.unsupported = true; r.error += " (Device Self-test not supported by this controller)"; }
            if ((rc & 0x7FF) == 0x1D) r.error += " (a self-test is already in progress)";
            return r;
        }
        r.ok = true;
        return r;
    }
    auto sub = ata_selftest_subcommand(kind);
    if (!sub) { ::close(fd); r.unsupported = true; r.error = "unknown self-test '" + kind + "'"; return r; }
    std::string err;
    Rc rc = ata_passthrough(fd, true, {0xB0, 0xD4, 0, *sub, 0x4F, 0xC2}, nullptr, nullptr, nullptr, err);
    r.method = "SG_IO ATA PASS-THROUGH(16) SMART EXECUTE OFF-LINE IMMEDIATE";
    if (rc == Rc::Unsupported || rc == Rc::Failed) {
        std::string err12;
        if (ata_passthrough(fd, false, {0xB0, 0xD4, 0, *sub, 0x4F, 0xC2}, nullptr, nullptr, nullptr, err12) == Rc::Ok) {
            rc = Rc::Ok;
            r.method = "SG_IO ATA PASS-THROUGH(12) SMART EXECUTE OFF-LINE IMMEDIATE";
        }
    }
    ::close(fd);
    if (rc != Rc::Ok) {
        r.error = err;
        r.permission_denied = rc == Rc::Denied;
        r.unsupported = rc == Rc::Unsupported;
        return r;
    }
    r.ok = true;
    return r;
}

ReadResult read_device(const Device& dev) {
    if (dev.protocol == "NVMe") return read_nvme(dev);
    return read_ata(dev);   // ATA and SCSI (SAT bridges); SAS answers ILLEGAL REQUEST -> unsupported
}

}  // namespace hdmon::native
#endif
