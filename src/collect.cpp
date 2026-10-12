#include "collect.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <regex>
#include <set>

#include "native_smart.hpp"
#include "wmi.hpp"

namespace hdmon {

std::vector<Result> score(const std::vector<Drive>& drives, const json& cfg) {
    std::vector<Result> out;
    for (auto& d : drives) out.emplace_back(d, assess(d, jget(cfg, "thresholds")));
    return out;
}

static std::unique_ptr<Smartctl> get_smartctl(const json& cfg) {
    const json& sc = jget(cfg, "smartctl");
    auto path = find_smartctl(jstr(sc, "path").value_or(""));
    if (!path) return nullptr;
    json sudo = jget(sc, "sudo");
    if (sudo.is_null()) sudo = "auto";
    return std::make_unique<Smartctl>(*path, sudo);
}

void apply_windows_letters(std::vector<Drive>& drives, const json& inv) {
    auto letters = windows_letter_map(inv);
    for (auto& d : drives) {
        auto n = windows_disk_number(d.device, d.serial, inv);
        if (!n) continue;
        d.notes.push_back("Windows disk number " + std::to_string(*n) + " (\\\\.\\PhysicalDrive" + std::to_string(*n) + ")");
        if (d.volumes.empty()) {
            auto it = letters.find(*n);
            if (it != letters.end()) d.volumes = it->second;
        }
    }
}

std::string smart_source(const json& cfg) {
    std::string s = lower(jstr(jget(cfg, "smart"), "source").value_or("auto"));
    return (s == "native" || s == "smartctl") ? s : "auto";
}

namespace {

std::vector<std::string> ignore_list(const json& cfg) {
    std::vector<std::string> out;
    const json& ig = jget(jget(cfg, "ignore"), "devices");
    if (ig.is_array()) for (auto& x : ig) out.push_back(to_str(x));
    return out;
}
bool ignored(const std::vector<std::string>& ig, const std::string& name) {
    return std::find(ig.begin(), ig.end(), name) != ig.end();
}

std::string family(const std::string& source) {
    if (starts_with(source, "native")) return "native";
    if (starts_with(source, "smartctl")) return "smartctl";
    if (starts_with(source, "windows-storage")) return "windows-storage";
    if (starts_with(source, "file:")) return "json-files";
    return source.empty() ? "none" : source;
}

// smartctl's view of the controller members behind hardware RAID cards (--scan-open) + manual [raid] smartctl_devices.
std::vector<Drive> hardware_members(Smartctl& sc, const json& cfg, const std::vector<std::string>& ig) {
    std::vector<Drive> out;
    std::vector<DeviceRef> refs;
    for (auto& r : scan_refs(&sc, "--scan-open"))
        if ((r.dev_type && is_raid_dev_type(*r.dev_type)) || contains(r.name, "csmi")) refs.push_back(r);
    const json& extra = jget(jget(cfg, "raid"), "smartctl_devices");   // e.g. ["/dev/sg0 -d cciss,0", "/dev/sda -d hpt,1/1/1"]
    if (extra.is_array())
        for (auto& e : extra) {
            std::string t = to_str(e);
            DeviceRef r;
            auto p = t.find(" -d ");
            r.name = trim(t.substr(0, p));
            if (p != std::string::npos) r.dev_type = trim(t.substr(p + 4));
            if (!r.name.empty()) refs.push_back(r);
        }
    for (auto& r : refs) {
        if (ignored(ig, r.name)) continue;
        Drive d = read_drive(sc, r);
        d.source = "smartctl";
        out.push_back(d);
    }
    return out;
}

std::vector<Drive> smartctl_only(const json& /*cfg*/, json& meta, const std::vector<std::string>& ig, Smartctl& sc) {
    std::vector<Drive> drives;
    for (auto& r : discover(&sc))
        if (!ignored(ig, r.name)) drives.push_back(read_drive(sc, r));
    if (current_os() == OS::Windows) {   // drive letters natively (IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS)
        std::map<int64_t, std::vector<std::string>> letters;
        for (auto& nd : native::list_devices())
            letters[std::atoll(nd.name.c_str() + 13)] = nd.volumes;
        json inv = json::object();
        for (auto& d : drives) {
            auto n = windows_disk_number(d.device, d.serial, inv);
            if (!n) continue;
            d.notes.push_back("Windows disk number " + std::to_string(*n) + " (\\\\.\\PhysicalDrive" + std::to_string(*n) + ")");
            if (d.volumes.empty() && letters.count(*n)) d.volumes = letters[*n];
        }
    }
    if (sc.use_sudo) meta["sudo"] = true;
    return drives;
}

}  // namespace

static std::unique_ptr<Smartctl> usable_smartctl(const json& cfg, json& meta, bool required) {
    auto sc = get_smartctl(cfg);
    if (!sc) {
        if (required) throw SmartctlMissing("smartctl (smartmontools) not found and [smart] source = \"smartctl\". Install it with:\n  " +
                                            install_hint() + "\nor remove that setting to use the built-in native SMART reader.");
        return nullptr;
    }
    auto ver = sc->version();
    meta["smartctl"] = sc->path;
    meta["smartctl_version"] = ver ? json(std::to_string(ver->first) + "." + std::to_string(ver->second)) : json();
    if (ver && *ver < std::make_pair(7, 0)) {
        std::string msg = "smartctl " + std::to_string(ver->first) + "." + std::to_string(ver->second) +
                          " is too old (JSON output needs 7.0+)";
        if (required) throw SmartctlMissing(msg + ". Upgrade with:\n  " + install_hint());
        meta["smartctl_ignored"] = msg;
        return nullptr;
    }
    return sc;
}

Scan scan(const json& cfg, const std::vector<std::string>& from_json) {
    Scan s;
    json& meta = s.meta;
    meta["os"] = os_name(current_os());
    auto ig = ignore_list(cfg);
    bool raid_on = !jget(jget(cfg, "raid"), "enabled").is_boolean() || jget(jget(cfg, "raid"), "enabled").get<bool>();
    std::vector<Drive> drives;
    if (!from_json.empty()) {
        for (auto& d : load_json_files(from_json)) if (!ignored(ig, d.device)) drives.push_back(d);
        meta["source"] = "json-files";
        s.results = score(drives, cfg);
        if (raid_on) {
            s.arrays = load_raid_fixtures(from_json);
            for (auto& a : hardware_arrays(s.results)) s.arrays.push_back(a);
        }
    } else {
        std::string mode = smart_source(cfg);
        meta["smart_source_setting"] = mode;
        OS os = current_os();
        std::unique_ptr<Smartctl> sc = mode == "native" ? nullptr : usable_smartctl(cfg, meta, mode == "smartctl");
        std::vector<native::Device> virtuals;
        if (mode == "smartctl") {
            drives = smartctl_only(cfg, meta, ig, *sc);
        } else {
            meta["native_backend"] = native::backend_name();
            std::vector<int64_t> denied_windows;
            bool any_denied = false;
            for (auto& dev : native::list_devices()) {
                if (ignored(ig, dev.name) || ignored(ig, dev.path)) continue;
                if (dev.virtual_disk) { virtuals.push_back(dev); continue; }
                native::ReadResult r = native::read_device(dev);
                if (r.ok) { drives.push_back(native::to_drive(dev, r)); continue; }
                any_denied = any_denied || r.permission_denied;
                if (sc) {   // optional fallback: smartctl on the same device
                    DeviceRef ref;
                    ref.name = os == OS::Windows ? "/dev/pd" + dev.name.substr(13) : dev.name;
                    Drive d = read_drive(*sc, ref);
                    if (d.smart_available || d.model) {
                        d.device = dev.name;
                        if (d.volumes.empty()) d.volumes = dev.volumes;
                        d.source = "smartctl (native read failed: " + r.error + ")";
                        drives.push_back(d);
                        continue;
                    }
                }
                Drive d;
                d.device = dev.name;
                d.model = dev.model;
                d.serial = dev.serial;
                d.capacity_bytes = dev.capacity_bytes;
                d.volumes = dev.volumes;
                if (dev.rotational) d.kind = *dev.rotational ? "HDD" : "SSD";
                if (dev.protocol == "NVMe") d.kind = "NVMe";
                d.protocol = dev.protocol;
                d.smart_available = false;
                d.source = "native";
                d.notes.push_back("Native SMART read failed: " + r.error);
                if (r.permission_denied) d.notes.push_back("Needs elevated rights: " + native::permission_hint());
                else if (r.unsupported)
                    d.notes.push_back("This device/bridge does not pass ATA or NVMe SMART commands natively"
                                      " (SAS disks and some USB bridges); installing smartmontools adds it as a fallback");
                if (os == OS::Windows && r.permission_denied) denied_windows.push_back(std::atoll(dev.name.c_str() + 13));
                drives.push_back(d);
            }
            if (!denied_windows.empty()) {   // Windows without elevation: MSFT_StorageReliabilityCounter via WMI
                std::string how;
                auto fallback = parse_windows_physical(windows_physical_inventory(how));
                for (auto& d : drives) {
                    if (d.smart_available || !starts_with(d.device, "PhysicalDrive")) continue;
                    for (auto& f : fallback)
                        if (f.device == d.device) {
                            f.source = "windows-storage (" + how + ")";
                            f.notes.insert(f.notes.begin(), "Not elevated: native SMART was refused, showing Windows reliability counters");
                            d = f;
                            break;
                        }
                }
                meta["fallback"] = "windows-storage (" + how + ")";
            }
            if (any_denied) meta["warning"] = "Some drives could not be read natively: " + native::permission_hint();
        }
        if (raid_on && sc) for (auto& d : hardware_members(*sc, cfg, ig)) drives.push_back(d);
        s.results = score(drives, cfg);
        if (raid_on) {
            s.arrays = detect_live_software_arrays(meta);
            for (auto& a : hardware_arrays(s.results)) s.arrays.push_back(a);
            // Windows RAID volumes (Intel RST / VMD, other fake-RAID): attach to an RST array or show a placeholder.
            for (auto& v : virtuals) {
                if (v.protocol != "RAID") continue;
                auto it = std::find_if(s.arrays.begin(), s.arrays.end(), [](const RaidArray& a) {
                    return a.backend == "intel-rst" || a.backend == "csmi"; });
                if (it != s.arrays.end()) { it->volumes.insert(it->volumes.end(), v.volumes.begin(), v.volumes.end()); continue; }
                RaidArray a;
                a.backend = icontains(v.model.value_or(""), "intel") ? "intel-rst" : "raid-volume";
                a.id = a.backend + ":" + v.name;
                a.name = (v.model ? *v.model : std::string("RAID volume")) + " (" + v.name + ")";
                a.level = "RAID volume";
                a.size_bytes = v.capacity_bytes;
                a.volumes = v.volumes;
                a.raw_state = "array state not available";
                a.notes.push_back("Motherboard RAID (Intel RST/VMD or similar). Member disks are hidden behind the RAID driver: "
                                  "install smartmontools for their SMART data (smartctl -d csmi) and Intel RST software "
                                  "(rstcli64) for the array state.");
                a.source = "native (bus type RAID)";
                s.arrays.push_back(a);
            }
            // Logical drives exposed by hardware RAID cards are shown as the array, not as a disk.
            std::vector<Result> kept;
            for (auto& r : s.results) {
                if (!is_controller_volume(r.first)) { kept.push_back(r); continue; }
                auto it = std::find_if(s.arrays.begin(), s.arrays.end(), [](const RaidArray& a) { return starts_with(a.id, "hw:"); });
                if (it == s.arrays.end()) {
                    RaidArray a;
                    a.backend = "raid-volume";
                    a.id = "raid-volume:" + r.first.device;
                    a.name = r.first.model.value_or("RAID logical drive") + " (" + r.first.device + ")";
                    a.level = "hardware RAID";
                    a.size_bytes = r.first.capacity_bytes;
                    a.raw_state = "array state not available";
                    a.notes.push_back("Logical drive of a hardware RAID controller. Install smartmontools so the member disks can be "
                                      "read through the controller (smartctl -d megaraid,N / aacraid / areca / 3ware / cciss / hpt).");
                    a.source = "native";
                    s.arrays.push_back(a);
                    it = s.arrays.end() - 1;
                }
                it->volumes.insert(it->volumes.end(), r.first.volumes.begin(), r.first.volumes.end());
                if (!it->size_bytes) it->size_bytes = r.first.capacity_bytes;
            }
            s.results = kept;
        }
    }
    link_members(s.arrays, s.results);
    // Which source fed the drives
    json counts = json::object();
    for (auto& r : s.results) {
        std::string f = family(r.first.source);
        counts[f] = jint(counts, f.c_str()).value_or(0) + 1;
    }
    meta["sources"] = counts;
    if (from_json.empty()) {
        std::vector<std::string> fams;
        for (auto it = counts.begin(); it != counts.end(); ++it) fams.push_back(it.key());
        meta["source"] = fams.empty() ? std::string("native") : join(fams, "+");
    }
    if (!s.arrays.empty()) meta["arrays"] = static_cast<int64_t>(s.arrays.size());
    return s;
}

std::vector<Result> collect(const json& cfg, const std::vector<std::string>& from_json, json& meta) {
    Scan s = scan(cfg, from_json);
    meta = s.meta;
    return s.results;
}

std::optional<Result> find_drive(const std::vector<Result>& results, const std::string& dev) {
    std::string want = trim(dev);
    std::string wl = want;
    while (!wl.empty() && wl.back() == ':') wl.pop_back();
    wl = upper(wl) + ":";
    for (auto& r : results) {
        const Drive& d = r.first;
        if (want == d.device || want == d.short_name() || (d.serial && want == *d.serial)) return r;
        if (want.size() <= 2)
            for (auto& v : d.volumes) if (upper(v) == wl) return r;
    }
    static const std::regex nv(R"((?:/dev/)?(nvme\d+)n\d+$)");
    for (auto& r : results) {
        std::smatch m;
        bool mm = std::regex_match(want, m, nv);
        if (ends_with(r.first.device, "/" + want) || (mm && r.first.short_name() == m[1].str())) return r;
    }
    return std::nullopt;
}

}  // namespace hdmon
