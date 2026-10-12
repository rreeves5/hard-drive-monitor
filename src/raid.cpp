#include "raid.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <regex>

#include "smart.hpp"

namespace hdmon {

// ------------------------------------------------------------------ model
json RaidArray::to_json() const {
    json j = json::object();
    j["id"] = id;
    j["name"] = name;
    j["level"] = level;
    j["backend"] = backend;
    j["state"] = state;
    j["raw_state"] = raw_state;
    j["rebuild_percent"] = rebuild_pct ? json(*rebuild_pct) : json(nullptr);
    j["size_bytes"] = size_bytes ? json(*size_bytes) : json(nullptr);
    json ms = json::array();
    for (auto& m : members) {
        json x = json::object();
        x["device"] = m.device;
        x["role"] = m.role;
        x["slot"] = m.slot ? json(*m.slot) : json(nullptr);
        x["serial"] = m.serial ? json(*m.serial) : json(nullptr);
        x["size_bytes"] = m.size_bytes ? json(*m.size_bytes) : json(nullptr);
        x["state_text"] = m.state_text;
        x["drive_key"] = m.drive_key.empty() ? json(nullptr) : json(m.drive_key);
        ms.push_back(x);
    }
    j["members"] = ms;
    j["volumes"] = volumes;
    j["notes"] = notes;
    j["source"] = source;
    return j;
}

std::string state_status(const std::string& s) {
    if (s == "OK") return "OK";
    if (s == "DEGRADED" || s == "REBUILDING") return "WARNING";
    if (s == "FAILED") return "CRITICAL";
    return "UNKNOWN";
}

static int roll_rank(const std::string& s) {   // UNKNOWN < OK < WARNING < CRITICAL
    if (s == "OK") return 1;
    if (s == "WARNING") return 2;
    if (s == "CRITICAL") return 3;
    return 0;
}

static std::string strip_dev(const std::string& s) { return starts_with(s, "/dev/") ? s.substr(5) : s; }

std::string md_member_disk(const std::string& member) {
    std::string m = strip_dev(member);
    std::smatch r;
    static const std::regex nvme(R"(^(nvme\d+)n\d+(p\d+)?$)"), mmc(R"(^(mmcblk\d+)(p\d+)?$)"),
        mac(R"(^(disk\d+)(s\d+)*$)"), sd(R"(^((?:sd|hd|vd|xvd)[a-z]+)\d*$)");
    if (std::regex_match(m, r, nvme) || std::regex_match(m, r, mmc) || std::regex_match(m, r, mac) || std::regex_match(m, r, sd))
        return r[1];
    return m;
}

const Result* member_result(const ArrayMember& m, const std::vector<Result>& results) {
    if (m.drive_key.empty()) return nullptr;
    for (auto& r : results) if (r.first.key() == m.drive_key) return &r;
    return nullptr;
}

std::string array_status(const RaidArray& a, const std::vector<Result>& results) {
    std::string worst = state_status(a.state);
    for (auto& m : a.members) {
        const Result* r = member_result(m, results);
        if (r && roll_rank(r->second.status) > roll_rank(worst)) worst = r->second.status;
    }
    return worst;
}

std::string state_text(const RaidArray& a) {
    std::string s = a.state == "OK" ? "OK" : a.state == "DEGRADED" ? "Degraded" : a.state == "REBUILDING" ? "Rebuilding"
                  : a.state == "FAILED" ? "Failed" : "State unknown";
    if (a.state == "REBUILDING" && a.rebuild_pct) s += " " + fmt_fixed(*a.rebuild_pct, 1) + "%";
    return s;
}

std::string state_label(const RaidArray& a, const std::vector<Result>& results, bool brief) {
    if (a.state != "UNKNOWN") return state_text(a);
    std::string worst;
    for (auto& m : a.members) {
        const Result* r = member_result(m, results);
        if (!r) continue;
        std::string st = member_status(m, r);
        if (worst.empty() || roll_rank(st) > roll_rank(worst)) worst = st;
    }
    if (worst.empty()) return brief ? "State unknown" : "State unknown (controller state not readable)";
    std::string w = worst == "OK" ? "OK" : worst == "WARNING" ? "Warning" : worst == "CRITICAL" ? "Critical" : "unknown";
    return "Worst disk " + w + (brief ? "" : " (controller state not readable)");
}

std::string member_status(const ArrayMember& m, const Result* r) {
    std::string smart = r ? r->second.status : "UNKNOWN";
    if (m.role == "failed" || m.role == "missing") return "CRITICAL";
    if (m.role == "rebuilding") return roll_rank(smart) > roll_rank("WARNING") ? smart : "WARNING";
    if (m.role == "spare") return r && smart != "UNKNOWN" ? smart : "OK";
    return smart;
}

std::string member_role_text(const ArrayMember& m) {
    if (m.role == "failed") return "Failed (removed from array)";
    if (m.role == "missing") return "Missing from array";
    if (m.role == "rebuilding") return "Rebuilding";
    if (m.role == "spare") return "Spare";
    if (m.role == "journal") return "Journal";
    return "";
}

std::string backend_label(const std::string& b) {
    static const std::map<std::string, std::string> L = {
        {"mdadm", "Linux md"}, {"storage-spaces", "Storage Spaces"}, {"intel-rst", "Intel RST"},
        {"raid-volume", "RAID volume"}, {"appleraid", "AppleRAID"}, {"megaraid", "MegaRAID"}, {"aacraid", "Adaptec"},
        {"areca", "Areca"}, {"3ware", "3ware"}, {"cciss", "HP Smart Array"}, {"hpt", "HighPoint"}, {"csmi", "Intel RST (CSMI)"}};
    auto it = L.find(b);
    return it == L.end() ? b : it->second;
}

std::set<std::string> member_keys(const std::vector<RaidArray>& arrays) {
    std::set<std::string> s;
    for (auto& a : arrays) for (auto& m : a.members) if (!m.drive_key.empty()) s.insert(m.drive_key);
    return s;
}

void link_members(std::vector<RaidArray>& arrays, const std::vector<Result>& results) {
    for (auto& a : arrays) {
        for (auto& m : a.members) {
            if (!m.drive_key.empty()) continue;
            std::string ns = norm_serial(m.serial.value_or(""));
            const Drive* hit = nullptr;
            if (!ns.empty())
                for (auto& r : results) {
                    std::string ds = norm_serial(r.first.serial.value_or(""));
                    if (!ds.empty() && (ds == ns || ends_with(ds, ns) || ends_with(ns, ds))) { hit = &r.first; break; }
                }
            if (!hit && !m.device.empty()) {
                std::string disk = md_member_disk(m.device);
                for (auto& r : results) {
                    const Drive& d = r.first;
                    if (d.dev_type && is_raid_dev_type(*d.dev_type)) continue;   // controller members only via hardware_arrays
                    if (m.device == d.device || disk == d.short_name() || disk == strip_dev(d.device)) { hit = &d; break; }
                }
            }
            if (hit) m.drive_key = hit->key();
        }
    }
}

json arrays_json(const std::vector<RaidArray>& arrays, const std::vector<Result>& results) {
    json out = json::array();
    for (auto& a : arrays) {
        json j = a.to_json();
        j["status"] = array_status(a, results);
        j["state_text"] = state_text(a);
        for (size_t i = 0; i < a.members.size(); ++i) {
            const Result* r = member_result(a.members[i], results);
            j["members"][i]["status"] = r ? json(r->second.status) : json(nullptr);
            j["members"][i]["health"] = r && r->second.health ? json(*r->second.health) : json(nullptr);
            j["members"][i]["model"] = r && r->first.model ? json(*r->first.model) : json(nullptr);
        }
        out.push_back(j);
    }
    return out;
}

// ------------------------------------------------------------------ Linux md: /proc/mdstat
static int md_redundancy(const std::string& level, int n) {
    if (level == "raid1") return n - 1;
    if (level == "raid4" || level == "raid5") return 1;
    if (level == "raid6") return 2;
    if (level == "raid10") return std::max(1, n / 2);
    return 0;   // raid0 / linear
}

static void md_finish_state(RaidArray& a, int raid_disks, int working, const std::string& action, std::optional<double> pct,
                            bool any_failed) {
    std::string lvl = a.level;
    if (a.raw_state == "inactive") a.state = "FAILED";
    else if (raid_disks > 0 && working >= 0 && working < raid_disks) {
        int missing = raid_disks - working;
        a.state = missing > md_redundancy(lvl, raid_disks) ? "FAILED" : "DEGRADED";
    } else if (any_failed && md_redundancy(lvl, std::max(raid_disks, 1)) == 0) a.state = "FAILED";
    else a.state = "OK";
    if (a.state != "FAILED" && (action == "recovery" || action == "recover" || action == "resync" || action == "reshape")) {
        a.state = "REBUILDING";
        a.rebuild_pct = pct;
        if (action == "resync") a.notes.push_back("initial / unclean-shutdown resync in progress");
        if (action == "reshape") a.notes.push_back("reshape in progress");
    } else if (action == "check" || action == "repair") {
        a.notes.push_back(std::string("scrub (") + action + ") in progress" + (pct ? " " + fmt_fixed(*pct, 1) + "%" : ""));
    }
}

std::vector<RaidArray> parse_mdstat(const std::string& text) {
    std::vector<RaidArray> out;
    auto lines = split_lines(text);
    static const std::regex head(R"(^(md[\w/]+)\s*:\s*(active|inactive)\s*(.*)$)");
    static const std::regex levelre(R"(^(raid\d+|linear|multipath|faulty)$)");
    static const std::regex mem(R"(^([\w\-/.]+)\[(\d+)\]((?:\([A-Z]\))*)$)");
    static const std::regex blocks(R"((\d+)\s+blocks)"), counts(R"(\[(\d+)/(\d+)\])");
    static const std::regex prog(R"((recovery|resync|reshape|check|repair)\s*=\s*([\d.]+)%)"),
        pend(R"((recovery|resync|reshape|check|repair)\s*=\s*(DELAYED|PENDING))");
    for (size_t i = 0; i < lines.size(); ++i) {
        std::smatch m;
        if (!std::regex_match(lines[i], m, head)) continue;
        RaidArray a;
        a.name = m[1];
        a.backend = "mdadm";
        a.id = "mdadm:" + a.name;
        a.raw_state = m[2];
        auto toks = split(trim(m[3]), ' ');
        bool any_failed = false;
        for (auto& t0 : toks) {
            std::string t = trim(t0);
            if (t.empty() || (t.front() == '(' && t.back() == ')')) continue;   // (auto-read-only), (read-only)
            std::smatch mm;
            if (a.level.empty() && std::regex_match(t, levelre)) { a.level = t; continue; }
            if (std::regex_match(t, mm, mem)) {
                ArrayMember am;
                am.device = "/dev/" + mm[1].str();
                std::string fl = mm[3];
                if (contains(fl, "(F)")) { am.role = "failed"; any_failed = true; }
                else if (contains(fl, "(S)")) am.role = "spare";
                else if (contains(fl, "(J)")) am.role = "journal";
                else if (contains(fl, "(R)")) am.role = "rebuilding";
                am.state_text = fl;
                a.members.push_back(am);
            }
        }
        int raid_disks = -1, working = -1;
        std::string action;
        std::optional<double> pct;
        for (size_t k = i + 1; k < lines.size() && !trim(lines[k]).empty() && !std::regex_match(lines[k], m, head); ++k) {
            const std::string& l = lines[k];
            std::smatch x;
            if (std::regex_search(l, x, blocks)) a.size_bytes = std::stoll(x[1]) * 1024;
            if (std::regex_search(l, x, counts)) { raid_disks = std::stoi(x[1]); working = std::stoi(x[2]); }
            if (std::regex_search(l, x, prog)) { action = x[1]; pct = std::stod(x[2]); }
            else if (std::regex_search(l, x, pend)) { action = x[1]; a.notes.push_back(action + " " + lower(x[2].str())); }
        }
        if (a.level.empty()) a.level = a.raw_state == "inactive" ? "unknown" : "";
        // members that are not failed/spare/journal but beyond the working count are being rebuilt
        if ((action == "recovery") && raid_disks > 0) {
            int active_seen = 0;
            for (auto& am : a.members) if (am.role == "active") ++active_seen;
            if (active_seen > working)   // the newest active member (highest descriptor) is the rebuild target
                for (auto it = a.members.begin(); it != a.members.end(); ++it)
                    if (it->role == "active") { it->role = "rebuilding"; break; }
        }
        md_finish_state(a, raid_disks, working, action, pct, any_failed);
        if (a.raw_state == "active" && raid_disks > 0) a.raw_state += " [" + std::to_string(raid_disks) + "/" + std::to_string(working) + "]";
        out.push_back(a);
    }
    return out;
}

// ------------------------------------------------------------------ mdadm --detail
static std::string kv(const std::string& text, const std::string& key) {
    for (auto& l : split_lines(text)) {
        std::string t = trim(l);
        if (starts_with(t, key) ) {
            auto c = t.find(':');
            if (c != std::string::npos && trim(t.substr(0, c)) == key) return trim(t.substr(c + 1));
        }
    }
    return "";
}

RaidArray parse_mdadm_detail(const std::string& text, const std::string& name_in) {
    RaidArray a;
    a.backend = "mdadm";
    a.name = name_in;
    for (auto& l : split_lines(text)) {
        std::string t = trim(l);
        if (starts_with(t, "/dev/") && ends_with(t, ":")) { a.name = strip_dev(t.substr(0, t.size() - 1)); break; }
    }
    a.level = kv(text, "Raid Level");
    std::string size = kv(text, "Array Size");
    if (!size.empty()) try { a.size_bytes = std::stoll(size) * 1024; } catch (...) {}
    std::string st = kv(text, "State");
    a.raw_state = st;
    std::string uuid = kv(text, "UUID");
    a.id = "mdadm:" + (uuid.empty() ? a.name : uuid);
    std::optional<double> pct;
    std::string action;
    for (auto [k, act] : {std::pair<const char*, const char*>{"Rebuild Status", "recovery"}, {"Resync Status", "resync"},
                          {"Reshape Status", "reshape"}, {"Check Status", "check"}}) {
        std::string v = kv(text, k);
        if (!v.empty()) { try { pct = std::stod(v); } catch (...) {} action = act; break; }
    }
    std::string ls = lower(st);
    if (contains(ls, "recovering") && action.empty()) action = "recovery";
    if (contains(ls, "resyncing") && action.empty()) action = "resync";
    int raid_disks = -1, active = -1;
    try { raid_disks = std::stoi(kv(text, "Raid Devices")); } catch (...) {}
    try { active = std::stoi(kv(text, "Active Devices")); } catch (...) {}
    static const std::regex row(R"(^\s*(\d+|-)\s+(\d+)\s+(\d+)\s+(\d+|-)\s+(.+?)\s+(/dev/\S+)\s*$)"),
        removed(R"(^\s*(\d+|-)\s+\d+\s+\d+\s+(\d+|-)\s+removed\s*$)");
    bool table = false, any_failed = false;
    for (auto& l : split_lines(text)) {
        if (contains(l, "RaidDevice State")) { table = true; continue; }
        if (!table) continue;
        std::smatch m;
        if (std::regex_match(l, m, row)) {
            ArrayMember am;
            am.device = m[6];
            am.state_text = trim(m[5]);
            std::string s = lower(am.state_text);
            if (m[4] != "-") am.slot = std::stoi(m[4]);
            if (contains(s, "faulty")) { am.role = "failed"; any_failed = true; }
            else if (contains(s, "rebuilding")) am.role = "rebuilding";
            else if (contains(s, "spare")) am.role = "spare";
            else if (contains(s, "journal")) am.role = "journal";
            else am.role = "active";
            a.members.push_back(am);
        } else if (std::regex_match(l, m, removed)) {
            ArrayMember am;
            am.device = "";
            am.role = "missing";
            am.state_text = "removed";
            if (m[2] != "-") am.slot = std::stoi(m[2]);
            a.members.push_back(am);
        }
    }
    if (contains(ls, "inactive")) a.raw_state = "inactive";
    md_finish_state(a, raid_disks, active, action, pct, any_failed);
    if (contains(ls, "failed") || contains(ls, "broken")) a.state = "FAILED";
    a.raw_state = st;
    return a;
}

void merge_md_detail(RaidArray& a, const RaidArray& d) {
    if (!d.level.empty()) a.level = d.level;
    if (d.size_bytes) a.size_bytes = d.size_bytes;
    if (!starts_with(d.id, "mdadm:md")) a.id = d.id;
    if (!d.members.empty()) a.members = d.members;   // detail knows slots, rebuilding and removed members
    if (d.state == "FAILED" || roll_rank(state_status(d.state)) > roll_rank(state_status(a.state))) a.state = d.state;
    if (d.rebuild_pct) a.rebuild_pct = d.rebuild_pct;
    if (!d.raw_state.empty()) a.raw_state = d.raw_state;
}

// ------------------------------------------------------------------ Linux md: sysfs (native, no mdadm)
static std::string sysread(const std::string& p) { return trim(read_file(p)); }

std::vector<RaidArray> detect_md_sysfs(const std::string& sys_root, const std::string& mdstat_path) {
    namespace fs = std::filesystem;
    std::vector<RaidArray> arrays = file_exists(mdstat_path) ? parse_mdstat(read_file(mdstat_path)) : std::vector<RaidArray>{};
    std::error_code ec;
    fs::path block = fs::path(sys_root) / "block";
    if (!fs::is_directory(block, ec)) return arrays;
    std::vector<std::string> names;
    for (auto& e : fs::directory_iterator(block, ec)) {
        std::string n = e.path().filename().string();
        if (starts_with(n, "md") && fs::is_directory(e.path() / "md", ec)) names.push_back(n);
    }
    std::sort(names.begin(), names.end());
    for (auto& n : names) {
        fs::path md = block / n / "md";
        auto it = std::find_if(arrays.begin(), arrays.end(), [&](const RaidArray& a) { return a.name == n; });
        if (it == arrays.end()) { RaidArray a; a.name = n; a.backend = "mdadm"; a.id = "mdadm:" + n; arrays.push_back(a); it = arrays.end() - 1; }
        RaidArray& a = *it;
        std::string level = sysread((md / "level").string());
        if (!level.empty()) a.level = level;
        std::string uuid = sysread((md / "uuid").string());
        if (!uuid.empty()) a.id = "mdadm:" + uuid;
        std::string size = sysread((block / n / "size").string());
        if (!size.empty()) try { a.size_bytes = std::stoll(size) * 512; } catch (...) {}
        std::string astate = sysread((md / "array_state").string());
        int raid_disks = -1, degraded = 0;
        try { raid_disks = std::stoi(sysread((md / "raid_disks").string())); } catch (...) {}
        try { degraded = std::stoi(sysread((md / "degraded").string())); } catch (...) {}
        std::string action = sysread((md / "sync_action").string());
        std::optional<double> pct;
        std::string done = sysread((md / "sync_completed").string());   // "123 / 456" sectors
        static const std::regex frac(R"((\d+)\s*/\s*(\d+))");
        std::smatch fm;
        if (std::regex_search(done, fm, frac) && std::stod(fm[2]) > 0)
            pct = std::floor(std::stod(fm[1]) / std::stod(fm[2]) * 1000.0) / 10.0;
        std::vector<ArrayMember> members;
        bool any_failed = false;
        for (auto& e : fs::directory_iterator(md, ec)) {
            std::string dn = e.path().filename().string();
            if (!starts_with(dn, "dev-")) continue;
            ArrayMember m;
            fs::path blk = e.path() / "block";
            std::error_code ec2;
            fs::path target = fs::read_symlink(blk, ec2);
            m.device = "/dev/" + (ec2 ? dn.substr(4) : target.filename().string());
            std::string st = sysread((e.path() / "state").string());
            m.state_text = st;
            std::string slot = sysread((e.path() / "slot").string());
            if (!slot.empty() && slot != "none") try { m.slot = std::stoi(slot); } catch (...) {}
            if (contains(st, "faulty")) { m.role = "failed"; any_failed = true; }
            else if (contains(st, "journal")) m.role = "journal";
            else if (contains(st, "in_sync")) m.role = "active";
            else if (m.slot && (action == "recover" || action == "reshape")) m.role = "rebuilding";
            else if (contains(st, "spare")) m.role = "spare";
            else m.role = "active";
            members.push_back(m);
        }
        std::sort(members.begin(), members.end(), [](const ArrayMember& x, const ArrayMember& y) {
            return x.slot.value_or(999) != y.slot.value_or(999) ? x.slot.value_or(999) < y.slot.value_or(999) : x.device < y.device; });
        if (!members.empty()) a.members = members;
        a.notes.clear();
        a.rebuild_pct.reset();
        a.raw_state = astate.empty() ? a.raw_state : astate;
        if (astate == "inactive" || astate == "clear") a.raw_state = "inactive";
        std::string act = action == "idle" || action == "frozen" ? "" : action;
        md_finish_state(a, raid_disks, raid_disks >= 0 ? raid_disks - degraded : -1, act, pct, any_failed);
        if (astate == "broken") a.state = "FAILED";
        a.raw_state = astate + (degraded ? ", " + std::to_string(degraded) + " missing" : "") + (act.empty() ? "" : ", " + act);
        a.source = "sysfs";
    }
    // mount points of the arrays (and their partitions)
    for (auto& line : split_lines(read_file("/proc/mounts"))) {
        auto f = split(line, ' ');
        if (f.size() < 2 || !starts_with(f[0], "/dev/md")) continue;
        std::string dev = f[0].substr(5);
        for (auto& a : arrays)
            if (dev == a.name || starts_with(dev, a.name + "p")) a.volumes.push_back(replace_all(f[1], "\\040", " "));
    }
    return arrays;
}

// ------------------------------------------------------------------ Windows Storage Spaces
static std::vector<json> as_list(const json& v) {
    std::vector<json> out;
    if (v.is_null()) return out;
    if (v.is_array()) for (auto& x : v) out.push_back(x);
    else out.push_back(v);
    return out;
}

static std::string vd_state(const json& vd, const std::vector<json>& jobs, std::optional<double>& pct) {
    std::string hs = jstr(vd, "HealthStatus").value_or("");
    std::string os = lower(jstr(vd, "OperationalStatus").value_or(""));
    std::string name = jstr(vd, "FriendlyName").value_or("");
    for (auto& j : jobs) {
        std::string js = lower(jstr(j, "JobState").value_or(""));
        std::string jn = lower(jstr(j, "Name").value_or(""));
        std::string target = jstr(j, "VirtualDisk").value_or(jstr(j, "Target").value_or(""));
        bool running = js == "running" || js == "new" || js == "suspended";
        bool repair = contains(jn, "repair") || contains(jn, "regenerat") || contains(jn, "rebalance") || contains(jn, "optimiz");
        if (running && repair && (target.empty() || target == name)) {
            if (auto p = jint(j, "PercentComplete")) pct = static_cast<double>(*p);
            return "REBUILDING";
        }
    }
    if (hs == "Unhealthy" || contains(os, "detached") || contains(os, "lost communication") || contains(os, "no redundancy") ||
        contains(os, "stopped"))
        return "FAILED";
    if (contains(os, "in service")) return "REBUILDING";
    if (hs == "Warning" || contains(os, "degraded") || contains(os, "incomplete")) return "DEGRADED";
    if (hs == "Healthy" && (os.empty() || os == "ok")) return "OK";
    return hs == "Healthy" ? "OK" : "UNKNOWN";
}

std::vector<RaidArray> parse_storage_spaces(const json& inv) {
    std::vector<RaidArray> out;
    auto pools = as_list(jget(inv, "StoragePools"));
    auto vds = as_list(jget(inv, "VirtualDisks"));
    auto pds = as_list(jget(inv, "PhysicalDisks"));
    auto jobs = as_list(jget(inv, "StorageJobs"));
    for (auto& p : pools) {
        if (truthy(jget(p, "IsPrimordial"))) continue;
        std::string pname = jstr(p, "FriendlyName").value_or("Storage pool");
        RaidArray a;
        a.backend = "storage-spaces";
        a.name = "Pool \"" + pname + "\"";
        a.id = "storage-spaces:" + jstr(p, "ObjectId").value_or(pname);
        a.size_bytes = jint(p, "Size");
        std::vector<std::string> levels, states;
        std::string worst = "OK";
        auto rank = [](const std::string& s) { return s == "FAILED" ? 4 : s == "REBUILDING" ? 3 : s == "DEGRADED" ? 2 : s == "OK" ? 1 : 0; };
        int nvd = 0;
        for (auto& vd : vds) {
            if (jstr(vd, "PoolFriendlyName").value_or(pname) != pname) continue;
            ++nvd;
            std::optional<double> pct;
            std::string st = vd_state(vd, jobs, pct);
            std::string res = jstr(vd, "ResiliencySettingName").value_or("?");
            std::string vname = jstr(vd, "FriendlyName").value_or("?");
            if (std::find(levels.begin(), levels.end(), res) == levels.end()) levels.push_back(res);
            states.push_back(vname + ": " + jstr(vd, "HealthStatus").value_or("?") + " / " +
                             jstr(vd, "OperationalStatus").value_or("?") + " (" + res + ")");
            if (rank(st) > rank(worst)) { worst = st; a.rebuild_pct = pct; }
            else if (st == "REBUILDING" && pct) a.rebuild_pct = pct;
        }
        std::string phs = jstr(p, "HealthStatus").value_or("");
        std::string pos = lower(jstr(p, "OperationalStatus").value_or(""));
        if (phs == "Unhealthy" || contains(pos, "read-only")) worst = rank(worst) >= 4 ? worst : "FAILED";
        else if ((phs == "Warning" || contains(pos, "degraded")) && rank(worst) < 2) worst = "DEGRADED";
        a.state = nvd == 0 && phs.empty() ? "UNKNOWN" : worst;
        a.level = levels.empty() ? "pool" : join(levels, "+");
        a.raw_state = "pool " + jstr(p, "HealthStatus").value_or("?") + " / " + jstr(p, "OperationalStatus").value_or("?");
        a.notes = states;
        for (auto& d : pds) {
            if (jstr(d, "PoolFriendlyName").value_or("") != pname) continue;
            ArrayMember m;
            auto num = jint(d, "DeviceId");
            if (!num) { std::string s = jstr(d, "DeviceId").value_or(""); try { if (!s.empty()) num = std::stoll(s); } catch (...) {} }
            m.device = num ? "PhysicalDrive" + std::to_string(*num) : jstr(d, "FriendlyName").value_or("?");
            std::string sn = trim(jstr(d, "SerialNumber").value_or(""));
            if (!sn.empty()) m.serial = sn;
            m.size_bytes = jint(d, "Size");
            std::string usage = lower(jstr(d, "Usage").value_or(""));
            std::string hs = jstr(d, "HealthStatus").value_or("");
            std::string os = lower(jstr(d, "OperationalStatus").value_or(""));
            m.state_text = jstr(d, "HealthStatus").value_or("") + " / " + jstr(d, "OperationalStatus").value_or("") +
                           (usage.empty() ? "" : " / " + jstr(d, "Usage").value_or(""));
            if (contains(os, "lost communication") || contains(os, "removing") || contains(os, "missing")) m.role = "missing";
            else if (usage == "retired" || hs == "Unhealthy" || contains(os, "failed")) m.role = "failed";
            else if (usage == "hot spare") m.role = "spare";
            else if (usage == "journal") m.role = "journal";
            else if (a.state == "REBUILDING" && contains(os, "in service")) m.role = "rebuilding";
            else m.role = "active";
            a.members.push_back(m);
        }
        a.source = "Storage Spaces";
        out.push_back(a);
    }
    return out;
}

// ------------------------------------------------------------------ macOS AppleRAID (diskutil appleRAID list)
std::vector<RaidArray> parse_appleraid(const std::string& text) {
    std::vector<RaidArray> out;
    RaidArray* cur = nullptr;
    bool in_members = false;
    static const std::regex member(R"(^\s*(\d+|-)\s+(disk\d+(?:s\d+)?|-)\s+([0-9A-Fa-f-]{36})\s+(.+?)\s+(\d+)?\s*$)");
    static const std::regex pctre(R"((\d+(?:\.\d+)?)\s*%)"), bytes(R"(\((\d+) Bytes\))");
    for (auto& l : split_lines(text)) {
        std::string t = trim(l);
        if (starts_with(t, "Name:")) {
            out.emplace_back();
            cur = &out.back();
            cur->backend = "appleraid";
            cur->name = trim(t.substr(5));
            in_members = false;
            continue;
        }
        if (!cur) continue;
        auto field = [&](const char* k) -> std::optional<std::string> {
            std::string key = std::string(k) + ":";
            if (starts_with(t, key)) return trim(t.substr(key.size()));
            return std::nullopt;
        };
        if (auto v = field("Unique ID")) { cur->id = "appleraid:" + *v; continue; }
        if (auto v = field("Type")) { cur->level = *v; continue; }
        if (auto v = field("Status")) { cur->raw_state = *v; continue; }
        if (auto v = field("Size")) { std::smatch m; if (std::regex_search(*v, m, bytes)) cur->size_bytes = std::stoll(m[1]); continue; }
        if (auto v = field("Device Node")) { cur->volumes.push_back("/dev/" + *v); continue; }
        if (auto v = field("Rebuild")) { cur->notes.push_back("rebuild: " + *v); continue; }
        if (starts_with(t, "#") && contains(t, "DevNode")) { in_members = true; continue; }
        std::smatch m;
        if (in_members && std::regex_match(l, m, member)) {
            ArrayMember am;
            am.device = m[2] == "-" ? "" : "/dev/" + m[2].str();
            if (m[1] != "-") am.slot = std::stoi(m[1]);
            am.serial = std::nullopt;
            am.state_text = trim(m[4]);
            if (m[5].matched) am.size_bytes = std::stoll(m[5]);
            std::string s = lower(am.state_text);
            if (contains(s, "rebuild")) {
                am.role = "rebuilding";
                std::smatch pm;
                if (std::regex_search(am.state_text, pm, pctre)) cur->rebuild_pct = std::stod(pm[1]);
            } else if (contains(s, "spare")) am.role = "spare";
            else if (contains(s, "fail")) am.role = "failed";
            else if (contains(s, "offline") || contains(s, "missing") || am.device.empty()) am.role = "missing";
            else am.role = "active";
            cur->members.push_back(am);
        }
    }
    for (auto& a : out) {
        if (a.id.empty()) a.id = "appleraid:" + a.name;
        std::string s = lower(a.raw_state);
        bool rebuilding = std::any_of(a.members.begin(), a.members.end(), [](const ArrayMember& m) { return m.role == "rebuilding"; });
        if (contains(s, "offline") || contains(s, "fail")) a.state = "FAILED";
        else if (rebuilding || contains(s, "rebuild")) a.state = "REBUILDING";
        else if (contains(s, "degraded")) a.state = "DEGRADED";
        else if (contains(s, "online")) a.state = "OK";
        else a.state = "UNKNOWN";
        if (a.state == "REBUILDING" && !a.rebuild_pct) {
            std::smatch pm;
            if (std::regex_search(a.raw_state, pm, pctre)) a.rebuild_pct = std::stod(pm[1]);
        }
        a.source = "diskutil appleRAID";
    }
    return out;
}

// ------------------------------------------------------------------ macOS AppleRAID via the IOKit registry
std::vector<RaidArray> parse_appleraid_ioreg(const json& snap) {
    std::vector<RaidArray> out;
    const json& sets = jget(snap, "sets");
    const json& members = jget(snap, "members");
    if (!sets.is_array()) return out;
    const std::string MISSING_UUID = "FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF", DELETED_UUID = "00000000-0000-0000-0000-000000000000";
    for (auto& st : sets) {
        RaidArray a;
        a.backend = "appleraid";
        std::string uuid = jstr(st, "AppleRAID-SetUUID").value_or("");
        a.name = jstr(st, "AppleRAID-SetName").value_or(uuid);
        a.id = "appleraid:" + (uuid.empty() ? a.name : uuid);
        a.level = jstr(st, "AppleRAID-LevelName").value_or("");
        a.raw_state = jstr(st, "AppleRAID-SetStatus").value_or("");
        if (auto sz = jint(st, "_size")) a.size_bytes = *sz;
        if (auto b = jstr(st, "_bsd"); b && !b->empty()) a.volumes.push_back("/dev/" + *b);
        if (truthy(jget(st, "AppleRAID-AutoRebuild"))) a.notes.push_back("rebuild: automatic");
        std::vector<std::string> spares;
        if (jget(st, "AppleRAID-Spares").is_array()) for (auto& x : jget(st, "AppleRAID-Spares")) spares.push_back(to_str(x));
        // Members the kernel has attached, by UUID.
        std::map<std::string, const json*> live;
        if (members.is_array())
            for (auto& m : members)
                if (jstr(m, "AppleRAID-SetUUID").value_or("") == uuid) live[jstr(m, "AppleRAID-MemberUUID").value_or("")] = &m;
        auto add = [&](const std::string& muuid, int idx, bool spare_slot) {
            if (muuid == DELETED_UUID) return;
            ArrayMember am;
            am.serial = std::nullopt;
            if (!spare_slot) am.slot = idx;
            auto it = live.find(muuid);
            if (muuid == MISSING_UUID || it == live.end()) {
                am.role = spare_slot ? "spare" : "missing";   // an absent spare does not hurt the array
                am.state_text = spare_slot ? "Spare (not attached)" : "Missing";
                a.members.push_back(am);
                return;
            }
            const json& m = *it->second;
            if (auto b = jstr(m, "_bsd"); b && !b->empty()) am.device = "/dev/" + *b;
            if (auto sz = jint(m, "_size")) am.size_bytes = *sz;
            if (auto i = jint(m, "AppleRAID-MemberIndex"); i && !spare_slot) am.slot = static_cast<int>(*i);
            am.state_text = jstr(m, "AppleRAID-MemberStatus").value_or(spare_slot ? "Standby" : "");
            std::string s = lower(am.state_text);
            if (s == "rebuilding") {
                am.role = "rebuilding";
                auto done = jint(m, "AppleRAID-Rebuild-Progress");
                if (done && am.size_bytes && *am.size_bytes > 0) a.rebuild_pct = 100.0 * static_cast<double>(*done) / static_cast<double>(*am.size_bytes);
            } else if (s == "standby" || spare_slot) am.role = "spare";
            else if (s == "failed") am.role = "failed";
            else if (s == "missing" || s == "offline") am.role = "missing";
            else am.role = "active";
            a.members.push_back(am);
        };
        int idx = 0;
        if (jget(st, "AppleRAID-Members").is_array())
            for (auto& x : jget(st, "AppleRAID-Members")) add(to_str(x), idx++, false);
        for (auto& sp : spares) add(sp, 0, true);
        std::string s = lower(a.raw_state);
        bool rebuilding = std::any_of(a.members.begin(), a.members.end(), [](const ArrayMember& m) { return m.role == "rebuilding"; });
        if (s == "offline" || s == "failed") a.state = "FAILED";
        else if (rebuilding) a.state = "REBUILDING";
        else if (s == "degraded") a.state = "DEGRADED";
        else if (s == "online") a.state = "OK";
        else a.state = "UNKNOWN";
        a.source = "IOKit AppleRAID";
        out.push_back(a);
    }
    return out;
}

// ------------------------------------------------------------------ Intel RST CLI (rstcli64 --information --volume)
std::vector<RaidArray> parse_rstcli(const std::string& text) {
    std::vector<RaidArray> out;
    RaidArray* cur = nullptr;
    ArrayMember* mem = nullptr;
    bool disks = false;
    for (auto& l : split_lines(text)) {
        std::string t = trim(l);
        if (starts_with(t, "--VOLUME INFORMATION")) { disks = false; mem = nullptr; continue; }
        if (starts_with(t, "--DISKS IN VOLUME")) { disks = true; mem = nullptr; continue; }
        auto c = t.find(':');
        if (c == std::string::npos) continue;
        std::string k = trim(t.substr(0, c)), v = trim(t.substr(c + 1));
        if (!disks) {
            if (k == "Name") { out.emplace_back(); cur = &out.back(); cur->backend = "intel-rst"; cur->name = v; cur->id = "intel-rst:" + v; }
            if (!cur) continue;
            if (k == "Raid Level") cur->level = "RAID " + v;
            else if (k == "State") cur->raw_state = v;
            else if (k == "Size") {
                std::smatch m;
                static const std::regex sz(R"(([\d.]+)\s*(GB|TB|MB))");
                if (std::regex_search(v, m, sz)) {
                    double n = std::stod(m[1]);
                    double mul = m[2] == "TB" ? 1e12 : m[2] == "GB" ? 1e9 : 1e6;
                    cur->size_bytes = static_cast<int64_t>(n * mul);
                }
            } else if (k == "Rebuild Progress" || k == "Percent Complete" || k == "Progress") {
                try { cur->rebuild_pct = std::stod(v); } catch (...) {}
            }
        } else if (cur) {
            if (k == "ID") { cur->members.emplace_back(); mem = &cur->members.back(); mem->device = "RST port " + v; }
            if (!mem) continue;
            if (k == "Serial Number") mem->serial = v;
            else if (k == "State") {
                mem->state_text = v;
                std::string s = lower(v);
                mem->role = contains(s, "fail") ? "failed" : contains(s, "missing") || contains(s, "offline") ? "missing"
                          : contains(s, "rebuild") ? "rebuilding" : contains(s, "spare") ? "spare" : "active";
            }
        }
    }
    for (auto& a : out) {
        std::string s = lower(a.raw_state);
        a.state = contains(s, "fail") ? "FAILED" : contains(s, "rebuild") || contains(s, "initializ") ? "REBUILDING"
                : contains(s, "degrad") ? "DEGRADED" : (s == "normal" || contains(s, "normal")) ? "OK" : "UNKNOWN";
        a.source = "Intel RST CLI";
    }
    return out;
}

// ------------------------------------------------------------------ hardware controllers (smartctl members)
bool is_raid_dev_type(const std::string& t) {
    static const std::regex re(R"(^(?:sat\+)?(megaraid|aacraid|areca|3ware|cciss|hpt|sssraid|jmb39x[^,]*),.+)");
    return std::regex_match(t, re);
}

static std::string raid_backend_of(const Drive& d) {
    if (d.dev_type && is_raid_dev_type(*d.dev_type)) {
        std::string t = starts_with(*d.dev_type, "sat+") ? d.dev_type->substr(4) : *d.dev_type;
        return t.substr(0, t.find(','));
    }
    if (contains(d.device, "csmi")) return "csmi";
    return "";
}

static std::string controller_of(const Drive& d) {
    if (contains(d.device, "csmi")) {   // /dev/csmi0,1 -> csmi0
        std::string n = strip_dev(d.device);
        return n.substr(0, n.find(','));
    }
    return d.device;
}

std::vector<RaidArray> hardware_arrays(const std::vector<Result>& results) {
    std::vector<RaidArray> out;
    std::map<std::string, size_t> idx;
    for (auto& [d, a] : results) {
        std::string be = raid_backend_of(d);
        if (be.empty()) continue;
        std::string ctrl = controller_of(d);
        std::string key = be + ":" + ctrl;
        if (!idx.count(key)) {
            RaidArray ra;
            ra.backend = be;
            ra.id = "hw:" + key;
            ra.name = backend_label(be) + " " + (be == "csmi" ? ctrl : strip_dev(ctrl));
            ra.level = "hardware RAID";
            ra.state = "UNKNOWN";
            ra.raw_state = "logical drive state not visible through SMART";
            ra.notes.push_back("Member disks read with smartctl -d " + be + ",N. The controller's logical-drive state needs the "
                               "vendor tool (storcli / perccli / arcconf / ssacli / tw_cli / cli64); status here is the worst member.");
            ra.source = "smartctl";
            idx[key] = out.size();
            out.push_back(ra);
        }
        ArrayMember m;
        m.device = d.dev_type && is_raid_dev_type(*d.dev_type) ? *d.dev_type : strip_dev(d.device);
        m.serial = d.serial;
        m.size_bytes = d.capacity_bytes;
        m.drive_key = d.key();
        m.role = "active";
        out[idx[key]].members.push_back(m);
    }
    return out;
}

std::string disk_label(const Drive& d) {
    if (d.dev_type && is_raid_dev_type(*d.dev_type)) return starts_with(*d.dev_type, "sat+") ? d.dev_type->substr(4) : *d.dev_type;
    if (contains(d.device, "csmi")) return strip_dev(d.device);
    return d.short_name();
}

bool is_controller_volume(const Drive& d) {
    static const std::regex re(R"((LSI|AVAGO|BROADCOM|MEGARAID|PERC|\bMR\d{4}|LOGICAL VOLUME|SMART ARRAY|ADAPTEC|\bASR\d|ARECA|3WARE|AMCC|HIGHPOINT|VIRTUAL DISK|RAID\s*\d*\s*VOLUME))",
                               std::regex::icase);
    if (d.smart_available && d.kind != "Unknown") return false;
    return std::regex_search(d.model.value_or(""), re);
}

// ------------------------------------------------------------------ fixtures
bool is_raid_fixture_json(const json& data) {
    if (!data.is_object()) return false;
    if (data.contains("StoragePools")) return true;
    return data.contains("devices") && data["devices"].is_array() && !data.contains("device");   // smartctl --scan
}

std::vector<RaidArray> load_raid_fixtures(const std::vector<std::string>& paths) {
    namespace fs = std::filesystem;
    std::vector<std::string> files;
    for (auto& p : paths) {
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            for (auto& e : fs::directory_iterator(p, ec)) if (e.is_regular_file()) files.push_back(e.path().string());
        } else files.push_back(p);
    }
    std::sort(files.begin(), files.end());
    std::vector<RaidArray> out, details;
    for (auto& f : files) {
        std::string b = lower(basename(f));
        if (!file_exists(f)) continue;
        auto tag = [&](std::vector<RaidArray> v) { for (auto& a : v) { a.source = "file:" + basename(f); out.push_back(a); } };
        if (starts_with(b, "mdstat")) tag(parse_mdstat(read_file(f)));
        else if (starts_with(b, "mdadm-detail")) { auto d = parse_mdadm_detail(read_file(f)); d.source = "file:" + basename(f); details.push_back(d); }
        else if (starts_with(b, "diskutil-appleraid")) tag(parse_appleraid(read_file(f)));
        else if (starts_with(b, "rstcli")) tag(parse_rstcli(read_file(f)));
        else if (ends_with(b, ".json")) {
            json j = json::parse(read_file(f), nullptr, false);
            if (j.is_object() && j.contains("StoragePools")) tag(parse_storage_spaces(j));
        }
    }
    for (auto& d : details) {
        auto it = std::find_if(out.begin(), out.end(), [&](const RaidArray& a) { return a.backend == "mdadm" && a.name == d.name; });
        if (it != out.end()) merge_md_detail(*it, d);
        else out.push_back(d);
    }
    return out;
}

}  // namespace hdmon
