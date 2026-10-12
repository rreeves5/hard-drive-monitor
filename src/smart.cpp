#include "smart.hpp"

#include <algorithm>
#include <cmath>
#include <regex>

namespace hdmon {

#ifdef _WIN32
static std::vector<std::string> extra_paths() {
    std::vector<std::string> out;
    for (const char* v : {"ProgramFiles", "ProgramFiles(x86)", "ProgramW6432"}) {
        std::string b = get_env(v);
        if (!b.empty()) out.push_back(b + "\\smartmontools\\bin");
    }
    out.push_back("C:\\Program Files\\smartmontools\\bin");
    std::string choco = get_env("ChocolateyInstall");
    out.push_back((choco.empty() ? std::string("C:\\ProgramData\\chocolatey") : choco) + "\\bin");
    return out;
}
static const char* SMARTCTL_NAMES[] = {"smartctl.exe"};
#else
static std::vector<std::string> extra_paths() {
    return {"/usr/sbin", "/sbin", "/usr/local/sbin", "/usr/local/bin",
            "/opt/homebrew/bin", "/opt/homebrew/sbin", "/opt/local/sbin", "/opt/local/bin"};
}
static const char* SMARTCTL_NAMES[] = {"smartctl"};
#endif

// ------------------------------------------------------------------ Drive
std::string Drive::key() const {
    if (serial && !serial->empty()) return trim(model.value_or("")) + "|" + trim(*serial);
    return trim(model.value_or("")) + "|" + device;
}
std::string Drive::short_name() const {
    auto p = device.rfind('/');
    return p == std::string::npos ? device : device.substr(p + 1);
}
template <class T>
static json oj(const std::optional<T>& v) { return v ? json(*v) : json(nullptr); }

json Drive::to_json() const {
    json d = json::object();
    d["device"] = device; d["dev_type"] = oj(dev_type); d["protocol"] = oj(protocol);
    d["model"] = oj(model); d["serial"] = oj(serial); d["firmware"] = oj(firmware);
    d["capacity_bytes"] = oj(capacity_bytes); d["kind"] = kind; d["rotation_rate"] = oj(rotation_rate);
    d["form_factor"] = oj(form_factor); d["interface"] = oj(interface);
    d["temperature_c"] = oj(temperature_c); d["temp_limit_warn"] = oj(temp_limit_warn);
    d["temp_limit_crit"] = oj(temp_limit_crit); d["power_on_hours"] = oj(power_on_hours);
    d["power_cycles"] = oj(power_cycles); d["smart_available"] = smart_available;
    d["smart_passed"] = oj(smart_passed); d["metrics"] = metrics;
    d["ata_attributes"] = ata_attributes; d["nvme_log"] = nvme_log;
    d["windows_reliability"] = windows_reliability; d["self_test"] = self_test;
    d["self_test_log"] = self_test_log; d["volumes"] = volumes; d["notes"] = notes;
    d["source"] = source; d["key"] = key();
    return d;
}

// ------------------------------------------------------------------ smartctl runner
std::optional<std::string> find_smartctl(const std::string& configured) {
    if (!configured.empty()) return file_exists(configured) ? std::optional<std::string>(configured) : std::nullopt;
    if (auto p = which("smartctl")) return p;
    for (auto& d : extra_paths()) {
        for (const char* n : SMARTCTL_NAMES) {
            std::string c = path_join(d, n);
            if (is_executable(c)) return c;
        }
    }
    return std::nullopt;
}

std::string install_hint(std::optional<OS> os) {
    OS o = os.value_or(current_os());
    if (o == OS::MacOS) return "brew install smartmontools";
    if (o == OS::Windows) return "winget install smartmontools.smartmontools   (or: choco install smartmontools)";
    if (!os) {
        const std::vector<std::pair<const char*, const char*>> mgrs = {
            {"apt-get", "sudo apt install smartmontools"}, {"dnf", "sudo dnf install smartmontools"},
            {"yum", "sudo yum install smartmontools"}, {"pacman", "sudo pacman -S smartmontools"},
            {"zypper", "sudo zypper install smartmontools"}, {"apk", "sudo apk add smartmontools"}};
        for (auto& m : mgrs) if (which(m.first)) return m.second;
    }
    return "sudo apt install smartmontools  |  sudo dnf install smartmontools  |  sudo pacman -S smartmontools";
}

std::optional<std::pair<int, int>> parse_version(const std::string& text) {
    std::smatch m;
    static const std::regex re(R"(smartctl\s+(\d+)\.(\d+))");
    if (std::regex_search(text, m, re)) return std::make_pair(std::stoi(m[1]), std::stoi(m[2]));
    return std::nullopt;
}

std::string messages_text(const json& data) {
    std::vector<std::string> parts;
    const json& msgs = jget(jget(data, "smartctl"), "messages");
    if (msgs.is_array())
        for (auto& m : msgs) if (m.is_object()) parts.push_back(jstr(m, "string").value_or(""));
    return join(parts, "; ");
}

static bool permission_error(const std::string& t) {
    static const std::regex re(R"(permission denied|operation not permitted|requires? (root|admin)|access is denied|must be root)",
                               std::regex::icase);
    return std::regex_search(t, re);
}

Smartctl::Smartctl(std::string p, json sudo_mode) : path(std::move(p)) {
    sudo_mode_ = sudo_mode.is_boolean() ? (sudo_mode.get<bool>() ? "true" : "false") : lower(to_str(sudo_mode));
    use_sudo = sudo_mode_ == "true" && can_sudo();
}
bool Smartctl::can_sudo() const {
#ifdef _WIN32
    return false;
#else
    return !is_root() && which("sudo").has_value();
#endif
}
std::optional<std::pair<int, int>> Smartctl::version() {
    auto r = run_cmd({path, "--version"}, 20);
    return parse_version(r.out);
}
int Smartctl::exec(const std::vector<std::string>& args, bool sudo, int timeout_s, json& data, std::string& err) {
    std::vector<std::string> cmd;
    if (sudo) { cmd.push_back("sudo"); cmd.push_back("-n"); }
    cmd.push_back(path);
    cmd.insert(cmd.end(), args.begin(), args.end());
    auto r = run_cmd(cmd, timeout_s);
    data = nullptr;
    if (!trim(r.out).empty()) data = json::parse(r.out, nullptr, false);
    if (data.is_discarded()) data = nullptr;
    err = r.err;
    return r.started ? r.rc : 2;
}
int Smartctl::run(const std::vector<std::string>& args, json& data, std::string& err, int timeout_s) {
    int rc = exec(args, use_sudo, timeout_s, data, err);
    if ((rc & 0x02) && !use_sudo && sudo_mode_ == "auto" && can_sudo() &&
        permission_error(messages_text(data) + " " + err)) {
        json d2; std::string e2;
        int rc2 = exec(args, true, timeout_s, d2, e2);
        if (!d2.is_null() && !(rc2 & 0x02)) { use_sudo = true; data = d2; err = e2; return rc2; }
    }
    if ((rc & 0x02) && permission_error(messages_text(data) + " " + err)) permission_problem = true;
    return rc;
}

// ------------------------------------------------------------------ parsing
int64_t ata_raw_count(int attr_id, const json& raw, bool* ok) {
    auto v = jint(raw, "value");
    if (ok) *ok = v.has_value();
    if (!v) return 0;
    if (attr_id == 188 && *v > 0xFFFF) return *v & 0xFFFF;      // Seagate packs 3 x 16-bit counters
    if (*v > 0xFFFFFFFFLL) return *v & 0xFFFF;                   // implausible counts are packed fields
    return *v;
}

static const std::vector<std::pair<int, const char*>> ATA_KEYS = {
    {5, "reallocated_sectors"}, {187, "reported_uncorrectable"}, {188, "command_timeout"},
    {197, "pending_sectors"}, {198, "offline_uncorrectable"}, {199, "udma_crc_errors"}};

static void set_metric(json& m, const char* k, const std::optional<int64_t>& v) { m[k] = v ? json(*v) : json(nullptr); }

Drive parse_smartctl(const json& data, const std::string& device) {
    const json& dev = jget(data, "device");
    Drive d;
    d.device = !device.empty() ? device : jstr(dev, "name").value_or("?");
    d.dev_type = jstr(dev, "type");
    d.protocol = jstr(dev, "protocol");
    d.model = jstr(data, "model_name");
    if (!d.model || d.model->empty()) { auto s = jstr(data, "scsi_model_name"); if (s && !s->empty()) d.model = s; else { auto p = jstr(data, "product"); if (p) d.model = p; } }
    d.serial = jstr(data, "serial_number");
    d.firmware = jstr(data, "firmware_version");
    auto cap = jint(jget(data, "user_capacity"), "bytes");
    if (!cap || *cap == 0) { auto n = jint(data, "nvme_total_capacity"); if (n) cap = n; }
    d.capacity_bytes = cap;
    d.rotation_rate = jint(data, "rotation_rate");
    d.form_factor = jstr(jget(data, "form_factor"), "name");
    d.interface = jstr(jget(data, "sata_version"), "string");
    if (!d.interface || d.interface->empty()) d.interface = jstr(jget(jget(data, "interface_speed"), "current"), "string");

    bool is_nvme = upper(d.protocol.value_or("")) == "NVME" || data.contains("nvme_smart_health_information_log");
    if (is_nvme) d.kind = "NVMe";
    else if (d.rotation_rate) d.kind = *d.rotation_rate == 0 ? "SSD" : "HDD";

    const json& temp = jget(data, "temperature");
    d.temperature_c = jint(temp, "current");
    d.temp_limit_warn = jint(temp, "op_limit_max");
    d.temp_limit_crit = jint(temp, "critical_limit_max");
    d.power_on_hours = jint(jget(data, "power_on_time"), "hours");
    d.power_cycles = jint(data, "power_cycle_count");

    const json& support = jget(data, "smart_support");
    if (jget(support, "available").is_boolean() && !jget(support, "available").get<bool>()) {
        d.smart_available = false;
        d.notes.push_back("Device does not support SMART (common for USB enclosures and virtual disks)");
    }
    const json& ss = jget(data, "smart_status");
    if (ss.is_object() && ss.contains("passed")) d.smart_passed = truthy(ss["passed"]);
    else if (!is_nvme && !data.contains("ata_smart_attributes")) d.smart_available = d.smart_available && truthy(ss);

    json m = json::object();
    if (is_nvme) {
        const json& log = jget(data, "nvme_smart_health_information_log");
        d.nvme_log = log.is_object() ? log : json::object();
        set_metric(m, "wear_percent_used", jint(log, "percentage_used"));
        set_metric(m, "media_errors", jint(log, "media_errors"));
        set_metric(m, "critical_warning", jint(log, "critical_warning"));
        set_metric(m, "available_spare", jint(log, "available_spare"));
        set_metric(m, "available_spare_threshold", jint(log, "available_spare_threshold"));
        set_metric(m, "unsafe_shutdowns", jint(log, "unsafe_shutdowns"));
        set_metric(m, "error_log_entries", jint(log, "num_err_log_entries"));
        set_metric(m, "warning_temp_minutes", jint(log, "warning_temp_time"));
        set_metric(m, "critical_temp_minutes", jint(log, "critical_comp_time"));
        auto duw = jint(log, "data_units_written");
        if (duw) m["data_written_tb"] = std::nearbyint(static_cast<double>(*duw) * 512000.0 / 1e12 * 100.0) / 100.0;
        if (!d.temperature_c) d.temperature_c = jint(log, "temperature");
        if (!d.power_on_hours) d.power_on_hours = jint(log, "power_on_hours");
        if (!d.power_cycles) d.power_cycles = jint(log, "power_cycles");
        if (d.nvme_log.empty()) d.smart_available = false;
        const json& st = jget(data, "nvme_self_test_log");
        const json& cur = jget(st, "current_self_test_operation");
        auto curv = jint(cur, "value");
        auto pct = jint(st, "current_self_test_completion_percent");
        d.self_test = json::object();
        d.self_test["in_progress"] = curv.value_or(0) != 0;
        d.self_test["status"] = oj(jstr(cur, "string"));
        d.self_test["remaining_percent"] = (curv.value_or(0) != 0 && pct) ? json(100 - *pct) : json(nullptr);
        d.self_test["short_minutes"] = 2;   // NVMe spec: short test completes within 2 minutes
        d.self_test["extended_minutes"] = nullptr;
        const json& tbl = jget(st, "table");
        if (tbl.is_array()) for (auto& row : tbl) {
            const json& res = jget(row, "self_test_result");
            json r = json::object();
            r["type"] = oj(jstr(jget(row, "self_test_code"), "string"));
            r["status"] = oj(jstr(res, "string"));
            r["passed"] = jint(res, "value") == std::optional<int64_t>(0);
            r["lifetime_hours"] = oj(jint(row, "power_on_hours"));
            r["lba"] = jget(row, "lba");
            d.self_test_log.push_back(r);
        }
    } else {
        const json& table = jget(jget(data, "ata_smart_attributes"), "table");
        std::map<int, json> attrs;
        if (table.is_array()) for (auto& a : table) {
            auto aid = jint(a, "id");
            if (!aid) continue;
            attrs[static_cast<int>(*aid)] = a;
            const json& raw = jget(a, "raw");
            json at = json::object();
            at["id"] = *aid; at["name"] = jget(a, "name"); at["value"] = jget(a, "value");
            at["worst"] = jget(a, "worst"); at["thresh"] = jget(a, "thresh");
            std::string wf = jstr(a, "when_failed").value_or("");
            at["when_failed"] = wf;
            at["raw"] = raw.is_object() && raw.contains("string") ? raw["string"] : jget(raw, "value");
            at["raw_value"] = jget(raw, "value");
            at["prefailure"] = truthy(jget(jget(a, "flags"), "prefailure"));
            d.ata_attributes.push_back(at);
        }
        for (auto& [aid, key] : ATA_KEYS) {
            auto it = attrs.find(aid);
            if (it != attrs.end()) {
                bool ok = false;
                int64_t v = ata_raw_count(aid, jget(it->second, "raw"), &ok);
                m[key] = ok ? json(v) : json(nullptr);
            }
        }
        if (!d.temperature_c) {
            for (int aid : {194, 190}) {
                auto it = attrs.find(aid);
                if (it != attrs.end()) {
                    auto v = jint(jget(it->second, "raw"), "value");
                    if (v) { d.temperature_c = *v & 0xFF; break; }
                }
            }
        }
        if (!d.power_on_hours && attrs.count(9)) d.power_on_hours = jint(jget(attrs[9], "raw"), "value");
        if (!d.power_cycles && attrs.count(12)) d.power_cycles = jint(jget(attrs[12], "raw"), "value");
        if (d.kind != "HDD") {
            std::optional<int64_t> wear;
            std::string src;
            auto eu = jget(jget(data, "endurance_used"), "current_percent");
            if (!eu.is_null()) { wear = opt_int(eu); src = "endurance_used"; }
            if (!wear) {
                const json& pages = jget(jget(data, "ata_device_statistics"), "pages");
                if (pages.is_array()) for (auto& page : pages) {
                    const json& rows = jget(page, "table");
                    if (!rows.is_array()) continue;
                    for (auto& row : rows) {
                        if (contains(jstr(row, "name").value_or(""), "Percentage Used Endurance") && !jget(row, "value").is_null()) {
                            wear = jint(row, "value"); src = "device statistics"; break;
                        }
                    }
                    if (wear) break;
                }
            }
            if (!wear) {
                for (int aid : {231, 233, 202, 169, 177}) {
                    auto it = attrs.find(aid);
                    if (it == attrs.end()) continue;
                    auto v = jint(it->second, "value");
                    if (v && *v >= 0 && *v <= 100) {
                        wear = 100 - *v;
                        src = "attribute " + std::to_string(aid) + " " + jstr(it->second, "name").value_or("");
                        break;
                    }
                }
            }
            if (wear) {
                m["wear_percent_used"] = *wear;
                m["wear_source"] = src;
                if (d.kind == "Unknown") d.kind = "SSD";
            }
        }
        json fn = json::array(), fp = json::array();
        for (auto& a : d.ata_attributes) {
            std::string label = std::to_string(a["id"].get<int64_t>()) + " " + to_str(a["name"]);
            if (a["when_failed"] == "now") fn.push_back(label);
            if (a["when_failed"] == "past") fp.push_back(label);
        }
        m["failing_now"] = fn;
        m["failed_past"] = fp;
        const json& sd = jget(jget(data, "ata_smart_data"), "self_test");
        const json& status = jget(sd, "status");
        int64_t val = jint(status, "value").value_or(0);
        const json& pm = jget(sd, "polling_minutes");
        d.self_test = json::object();
        d.self_test["in_progress"] = (val >> 4) == 0xF;
        d.self_test["status"] = oj(jstr(status, "string"));
        d.self_test["remaining_percent"] = oj(jint(status, "remaining_percent"));
        d.self_test["short_minutes"] = oj(jint(pm, "short"));
        d.self_test["extended_minutes"] = oj(jint(pm, "extended"));
        d.self_test["conveyance_minutes"] = oj(jint(pm, "conveyance"));
        const json& log = jget(jget(jget(data, "ata_smart_self_test_log"), "standard"), "table");
        if (log.is_array()) for (auto& row : log) {
            const json& st = jget(row, "status");
            json r = json::object();
            r["type"] = oj(jstr(jget(row, "type"), "string"));
            r["status"] = oj(jstr(st, "string"));
            r["passed"] = st.contains("passed") ? truthy(st["passed"]) : (jint(st, "value") == std::optional<int64_t>(0));
            r["lifetime_hours"] = oj(jint(row, "lifetime_hours"));
            r["lba"] = jget(row, "lba");
            d.self_test_log.push_back(r);
        }
        if ((!table.is_array() || table.empty()) && !d.smart_passed) d.smart_available = false;
    }
    json clean = json::object();
    for (auto it = m.begin(); it != m.end(); ++it) {
        if (it.value().is_null()) continue;
        if (it.value().is_array() && it.value().empty()) continue;
        clean[it.key()] = it.value();
    }
    d.metrics = clean;
    return d;
}

// ------------------------------------------------------------------ discovery
static std::string nvme_ctrl(const std::string& name) {
    static const std::regex re(R"((/dev/nvme\d+)n\d+$)");
    std::smatch m;
    return std::regex_match(name, m, re) ? std::string(m[1]) : name;
}

static void mounts_of(const json& bd, std::vector<std::string>& out) {
    const json& ch = jget(bd, "children");
    if (!ch.is_array()) return;
    for (auto& c : ch) {
        auto mp = jstr(c, "mountpoint");
        if (mp && !mp->empty()) out.push_back(*mp);
        mounts_of(c, out);
    }
}

static void fill_missing(DeviceRef& dst, const DeviceRef& src) {
    if (!dst.model) dst.model = src.model;
    if (!dst.serial) dst.serial = src.serial;
    if (!dst.capacity_bytes) dst.capacity_bytes = src.capacity_bytes;
    if (!dst.rotational) dst.rotational = src.rotational;
    if (!dst.transport) dst.transport = src.transport;
    if (dst.volumes.empty()) dst.volumes = src.volumes;
}

std::vector<DeviceRef> parse_scan(const json& data) {
    std::vector<DeviceRef> refs;
    const json& devs = jget(data, "devices");
    if (devs.is_array()) for (auto& dv : devs) {
        DeviceRef r;
        r.name = jstr(dv, "name").value_or("");
        r.dev_type = jstr(dv, "type");
        r.protocol = jstr(dv, "protocol");
        r.note = jstr(dv, "open_error");
        if (!r.name.empty()) refs.push_back(r);
    }
    return refs;
}

std::vector<DeviceRef> scan_refs(Smartctl* sc, const char* flag) {
    if (!sc) return {};
    json data; std::string err;
    sc->run({flag, "--json"}, data, err, 60);
    return parse_scan(data);
}
static std::vector<DeviceRef> scan_devices(Smartctl* sc, const char* flag) { return scan_refs(sc, flag); }

static std::vector<DeviceRef> discover_linux(Smartctl* sc) {
    auto refs = scan_devices(sc, "--scan-open");
    if (!which("lsblk")) return refs;
    auto r = run_cmd({"lsblk", "-J", "-b", "-o", "NAME,TYPE,TRAN,ROTA,SIZE,MODEL,SERIAL,MOUNTPOINT"}, 20);
    if (r.rc != 0) return refs;
    json j = json::parse(r.out, nullptr, false);
    const json& bds = jget(j, "blockdevices");
    if (!bds.is_array()) return refs;
    static const std::regex skip(R"(^(loop|zram|ram|sr|fd|md|dm-|nbd))");
    for (auto& bd : bds) {
        std::string name = jstr(bd, "name").value_or("");
        if (jstr(bd, "type").value_or("") != "disk" || std::regex_search(name, skip)) continue;
        DeviceRef info;
        info.name = "/dev/" + name;
        info.source = "lsblk";
        std::string model = trim(jstr(bd, "model").value_or(""));
        if (!model.empty()) info.model = model;
        info.serial = jstr(bd, "serial");
        info.capacity_bytes = jint(bd, "size");
        if (!jget(bd, "rota").is_null()) info.rotational = truthy(jget(bd, "rota"));
        info.transport = jstr(bd, "tran");
        mounts_of(bd, info.volumes);
        auto it = std::find_if(refs.begin(), refs.end(), [&](const DeviceRef& x) {
            return x.name == info.name || x.name == nvme_ctrl(info.name); });
        if (it != refs.end()) fill_missing(*it, info);
        else refs.push_back(info);
    }
    return refs;
}

static std::string diskutil_field(const std::string& text, const std::string& label) {
    for (auto& line : split_lines(text)) {
        std::string t = trim(line);
        if (starts_with(t, label + ":")) return trim(t.substr(label.size() + 1));
    }
    return "";
}

static std::vector<DeviceRef> discover_macos(Smartctl* sc) {
    auto refs = scan_devices(sc, "--scan");
    auto r = run_cmd({"diskutil", "list", "physical"}, 30);
    if (r.rc != 0) return refs;
    static const std::regex whole(R"(^(/dev/disk\d+)\s)");
    for (auto& line : split_lines(r.out)) {
        std::smatch m;
        if (!std::regex_search(line, m, whole)) continue;
        DeviceRef info;
        info.name = m[1];
        info.source = "diskutil";
        auto ir = run_cmd({"diskutil", "info", info.name.substr(5)}, 30);
        std::string media = diskutil_field(ir.out, "Device / Media Name");
        if (!media.empty()) info.model = media;
        std::string size = diskutil_field(ir.out, "Disk Size");
        std::smatch sm;
        static const std::regex bytes(R"(\((\d+) Bytes\))");
        if (std::regex_search(size, sm, bytes)) info.capacity_bytes = std::stoll(sm[1]);
        std::string solid = diskutil_field(ir.out, "Solid State");
        if (!solid.empty()) info.rotational = solid != "Yes";
        std::string proto = diskutil_field(ir.out, "Protocol");
        if (!proto.empty()) info.transport = proto;
        auto it = std::find_if(refs.begin(), refs.end(), [&](const DeviceRef& x) { return x.name == info.name; });
        if (it != refs.end()) fill_missing(*it, info);
        else refs.push_back(info);
    }
    return refs;
}

std::vector<DeviceRef> discover(Smartctl* sc) {
    switch (current_os()) {
        case OS::MacOS: return discover_macos(sc);
        case OS::Windows: return scan_devices(sc, "--scan");
        default: return discover_linux(sc);
    }
}

std::string elevation_hint() {
    if (current_os() == OS::Windows) return "run hdmon from an elevated (Run as Administrator) terminal";
    return "run with sudo, or add a sudoers rule for smartctl and set sudo = \"auto\" in config.toml (see README)";
}

Drive read_drive(Smartctl& sc, const DeviceRef& ref) {
    std::vector<std::string> args = {"-x", "--json"};
    if (ref.dev_type) { args.push_back("-d"); args.push_back(*ref.dev_type); }
    args.push_back(ref.name);
    json data; std::string err;
    int rc = sc.run(args, data, err);
    if (!data.is_object() || (rc & 0x03) || !data.contains("device")) {
        Drive d;
        d.device = ref.name; d.dev_type = ref.dev_type;
        d.protocol = ref.protocol ? ref.protocol : ref.transport;
        d.model = ref.model; d.serial = ref.serial; d.capacity_bytes = ref.capacity_bytes;
        d.smart_available = false; d.source = ref.source;
        if (ref.rotational) d.kind = *ref.rotational ? "HDD" : "SSD";
        std::string msg = messages_text(data);
        if (msg.empty()) msg = trim(err);
        if (msg.empty() && ref.note) msg = *ref.note;
        if (msg.empty()) msg = "smartctl could not read this device";
        d.notes.push_back(msg);
        if (sc.permission_problem) d.notes.push_back("Needs elevated rights: " + elevation_hint());
        d.volumes = ref.volumes;
        return d;
    }
    Drive d = parse_smartctl(data, ref.name);
    if (d.volumes.empty()) d.volumes = ref.volumes;
    if ((rc & 0x04) && !messages_text(data).empty()) d.notes.push_back("smartctl: " + messages_text(data));
    return d;
}

std::vector<Drive> load_json_files(const std::vector<std::string>& paths) {
    std::vector<std::string> files;
    for (auto& p : paths) {
        if (is_dir(p)) { auto v = list_json_files(p); files.insert(files.end(), v.begin(), v.end()); }
        else files.push_back(p);
    }
    std::vector<Drive> drives;
    for (auto& f : files) {
        if (!file_exists(f)) throw std::runtime_error("no such file: " + f);
        json data = json::parse(read_file(f), nullptr, false);
        if (data.is_discarded()) throw std::runtime_error("not valid JSON: " + f);
        if (data.is_object() && (data.contains("StoragePools") ||
                                 (data.contains("devices") && !data.contains("device")))) continue;   // RAID fixtures (raid.cpp)
        if (data.is_object() && data.contains("PhysicalDisks")) {
            for (auto& d : parse_windows_physical(data)) { d.source = "file:" + basename(f); drives.push_back(d); }
            continue;
        }
        Drive d = parse_smartctl(data);
        d.source = "file:" + basename(f);
        const json& vols = jget(data, "_volumes");
        if (vols.is_array()) for (auto& v : vols) d.volumes.push_back(to_str(v));
        drives.push_back(d);
    }
    return drives;
}

// ------------------------------------------------------------------ Windows
const char* PS_LETTERS = R"PS(
$ErrorActionPreference = 'SilentlyContinue'
$parts = @(Get-Partition | Where-Object { $_.DriveLetter } | ForEach-Object {
  [pscustomobject]@{ DiskNumber = $_.DiskNumber; DriveLetter = [string]$_.DriveLetter } })
$disks = @(Get-Disk | ForEach-Object {
  [pscustomobject]@{ Number = $_.Number; SerialNumber = $_.SerialNumber; FriendlyName = $_.FriendlyName;
                     BusType = [string]$_.BusType; Size = $_.Size } })
[pscustomobject]@{ Partitions = $parts; Disks = $disks } | ConvertTo-Json -Depth 5 -Compress
)PS";

const char* PS_PHYSICAL = R"PS(
$ErrorActionPreference = 'SilentlyContinue'
$pd = @(Get-PhysicalDisk | ForEach-Object {
  $r = $_ | Get-StorageReliabilityCounter
  $rel = $null
  if ($r) { $rel = [pscustomobject]@{
      Temperature = $r.Temperature; TemperatureMax = $r.TemperatureMax; Wear = $r.Wear;
      PowerOnHours = $r.PowerOnHours; StartStopCycleCount = $r.StartStopCycleCount;
      ReadErrorsTotal = $r.ReadErrorsTotal; ReadErrorsCorrected = $r.ReadErrorsCorrected;
      ReadErrorsUncorrected = $r.ReadErrorsUncorrected; WriteErrorsTotal = $r.WriteErrorsTotal;
      WriteErrorsCorrected = $r.WriteErrorsCorrected; WriteErrorsUncorrected = $r.WriteErrorsUncorrected;
      ReadLatencyMax = $r.ReadLatencyMax; WriteLatencyMax = $r.WriteLatencyMax } }
  [pscustomobject]@{ DeviceId = $_.DeviceId; FriendlyName = $_.FriendlyName; SerialNumber = $_.SerialNumber;
    MediaType = [string]$_.MediaType; BusType = [string]$_.BusType; Size = $_.Size;
    HealthStatus = [string]$_.HealthStatus; OperationalStatus = ([string[]]$_.OperationalStatus) -join ', ';
    FirmwareVersion = $_.FirmwareVersion; Reliability = $rel } })
$parts = @(Get-Partition | Where-Object { $_.DriveLetter } | ForEach-Object {
  [pscustomobject]@{ DiskNumber = $_.DiskNumber; DriveLetter = [string]$_.DriveLetter } })
$disks = @(Get-Disk | ForEach-Object {
  [pscustomobject]@{ Number = $_.Number; SerialNumber = $_.SerialNumber; FriendlyName = $_.FriendlyName;
                     BusType = [string]$_.BusType; Size = $_.Size } })
[pscustomobject]@{ PhysicalDisks = $pd; Partitions = $parts; Disks = $disks } | ConvertTo-Json -Depth 6 -Compress
)PS";

std::optional<std::string> powershell_exe() {
    if (auto p = which("powershell")) return p;
    if (auto p = which("powershell.exe")) return p;
    return which("pwsh");
}

json run_powershell_json(const std::string& script, int timeout_s) {
    auto ps = powershell_exe();
    if (!ps) return json::object();
    auto r = run_cmd({*ps, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", script}, timeout_s);
    if (trim(r.out).empty()) return json::object();
    json j = json::parse(r.out, nullptr, false);
    return j.is_discarded() ? json::object() : j;
}

static std::vector<json> as_list(const json& v) {
    std::vector<json> out;
    if (v.is_null()) return out;
    if (v.is_array()) { for (auto& x : v) out.push_back(x); }
    else out.push_back(v);   // ConvertTo-Json unwraps 1-element arrays
    return out;
}

std::string norm_serial(const std::string& s) {
    std::string out;
    for (char c : upper(s)) if (std::isalnum(static_cast<unsigned char>(c))) out += c;
    return out;
}

std::map<int64_t, std::vector<std::string>> windows_letter_map(const json& inv) {
    std::map<int64_t, std::vector<std::string>> out;
    for (auto& p : as_list(jget(inv, "Partitions"))) {
        auto n = jint(p, "DiskNumber");
        std::string letter = trim(jstr(p, "DriveLetter").value_or(""));
        if (!n || letter.empty()) continue;
        while (!letter.empty() && letter.back() == ':') letter.pop_back();
        out[*n].push_back(letter + ":");
    }
    for (auto& kv : out) std::sort(kv.second.begin(), kv.second.end());
    return out;
}

std::optional<int64_t> windows_disk_number(const std::string& device, const std::optional<std::string>& serial,
                                           const json& inv) {
    std::string ns = norm_serial(serial.value_or(""));
    if (!ns.empty()) {
        auto disks = as_list(jget(inv, "Disks"));
        auto pds = as_list(jget(inv, "PhysicalDisks"));
        disks.insert(disks.end(), pds.begin(), pds.end());
        for (auto& d : disks) {
            std::string ds = norm_serial(jstr(d, "SerialNumber").value_or(""));
            if (!ds.empty() && (ds == ns || ends_with(ds, ns) || ends_with(ns, ds))) {
                auto n = d.contains("Number") ? jint(d, "Number") : jint(d, "DeviceId");
                if (n) return n;
            }
        }
    }
    std::smatch m;
    static const std::regex pd(R"(/dev/pd(\d+)$)"), sd(R"(/dev/sd([a-z]+)$)"), phys(R"(^PhysicalDrive(\d+)$)");
    if (std::regex_match(device, m, pd) || std::regex_match(device, m, phys)) return std::stoll(m[1]);
    if (std::regex_match(device, m, sd)) {
        int64_t n = 0;
        for (char ch : std::string(m[1])) n = n * 26 + (ch - 96);
        return n - 1;
    }
    return std::nullopt;
}

std::vector<Drive> parse_windows_physical(const json& inv) {
    auto letters = windows_letter_map(inv);
    std::vector<Drive> drives;
    for (auto& pd : as_list(jget(inv, "PhysicalDisks"))) {
        auto num = jint(pd, "DeviceId");
        std::string bus = trim(jstr(pd, "BusType").value_or(""));
        std::string media = trim(jstr(pd, "MediaType").value_or(""));
        Drive d;
        d.kind = upper(bus) == "NVME" ? "NVMe" : (media == "HDD" ? "HDD" : (media == "SSD" ? "SSD" : "Unknown"));
        d.device = num ? "PhysicalDrive" + std::to_string(*num) : jstr(pd, "FriendlyName").value_or("?");
        if (!bus.empty()) { d.protocol = bus; d.interface = bus; }
        std::string fn = trim(jstr(pd, "FriendlyName").value_or(""));
        if (!fn.empty()) d.model = fn;
        std::string sn = trim(jstr(pd, "SerialNumber").value_or(""));
        if (!sn.empty()) d.serial = sn;
        d.firmware = jstr(pd, "FirmwareVersion");
        d.capacity_bytes = jint(pd, "Size");
        d.source = "windows-storage";
        if (num && letters.count(*num)) d.volumes = letters[*num];
        std::string hs = trim(jstr(pd, "HealthStatus").value_or(""));
        if (hs == "Healthy") d.smart_passed = true;
        else if (hs == "Unhealthy") d.smart_passed = false;
        json m = json::object();
        m["windows_health_status"] = hs.empty() ? json(nullptr) : json(hs);
        auto os = jstr(pd, "OperationalStatus");
        m["windows_operational_status"] = (os && !os->empty()) ? json(*os) : json(nullptr);
        const json& rel = jget(pd, "Reliability");
        d.windows_reliability = rel.is_object() ? rel : json::object();
        if (rel.is_object() && !rel.empty()) {
            auto t = jint(rel, "Temperature");
            if (t && *t != 0) d.temperature_c = t;   // 0 = not reported
            d.power_on_hours = jint(rel, "PowerOnHours");
            d.power_cycles = jint(rel, "StartStopCycleCount");
            if (d.kind != "HDD" && !jget(rel, "Wear").is_null()) {
                m["wear_percent_used"] = oj(jint(rel, "Wear"));
                m["wear_source"] = "Get-StorageReliabilityCounter";
            }
            auto ru = jint(rel, "ReadErrorsUncorrected"), wu = jint(rel, "WriteErrorsUncorrected");
            if (ru || wu) m["uncorrected_errors"] = ru.value_or(0) + wu.value_or(0);
        } else {
            d.notes.push_back("Reliability counters unavailable (run as Administrator, or the driver does not expose them)");
        }
        if ((!rel.is_object() || rel.empty()) && !d.smart_passed) d.smart_available = false;
        d.notes.push_back("Windows Storage fallback: fewer attributes than native SMART. Run as Administrator for the full SMART view.");
        json clean = json::object();
        for (auto it = m.begin(); it != m.end(); ++it) if (!it.value().is_null()) clean[it.key()] = it.value();
        d.metrics = clean;
        drives.push_back(d);
    }
    return drives;
}

}  // namespace hdmon
