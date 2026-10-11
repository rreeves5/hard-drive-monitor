#include "collect.hpp"

#include <algorithm>
#include <memory>
#include <regex>
#include <set>

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

std::vector<Result> collect(const json& cfg, const std::vector<std::string>& from_json, json& meta) {
    meta = json::object();
    meta["os"] = os_name(current_os());
    std::set<std::string> ignore;
    const json& ig = jget(jget(cfg, "ignore"), "devices");
    if (ig.is_array()) for (auto& x : ig) ignore.insert(to_str(x));
    auto keep = [&](std::vector<Drive> ds) {
        ds.erase(std::remove_if(ds.begin(), ds.end(), [&](const Drive& d) { return ignore.count(d.device) > 0; }), ds.end());
        return ds;
    };
    if (!from_json.empty()) {
        auto drives = load_json_files(from_json);
        meta["source"] = "json-files";
        return score(keep(drives), cfg);
    }
    auto sc = get_smartctl(cfg);
    OS os = current_os();
    if (!sc) {
        if (os == OS::Windows) {
            auto drives = parse_windows_physical(run_powershell_json(PS_PHYSICAL));
            if (!drives.empty()) {
                meta["source"] = "windows-storage";
                meta["warning"] = "smartctl not found - using Windows Storage counters. For full SMART data: " + install_hint();
                return score(keep(drives), cfg);
            }
        }
        throw SmartctlMissing("smartctl (smartmontools) not found. Install it with:\n  " + install_hint());
    }
    auto ver = sc->version();
    meta["source"] = "smartctl";
    meta["smartctl"] = sc->path;
    meta["smartctl_version"] = ver ? json(std::to_string(ver->first) + "." + std::to_string(ver->second)) : json();
    if (ver && *ver < std::make_pair(7, 0))
        throw SmartctlMissing("smartctl " + std::to_string(ver->first) + "." + std::to_string(ver->second) +
                              " is too old (JSON output needs 7.0+). Upgrade with:\n  " + install_hint());
    std::vector<Drive> drives;
    for (auto& r : discover(sc.get()))
        if (!ignore.count(r.name)) drives.push_back(read_drive(*sc, r));
    if (os == OS::Windows) apply_windows_letters(drives, run_powershell_json(PS_LETTERS));
    if (sc->use_sudo) meta["sudo"] = true;
    return score(drives, cfg);
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
