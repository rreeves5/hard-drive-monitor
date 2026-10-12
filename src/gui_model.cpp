#include <cctype>
#include "gui_model.hpp"

#include <algorithm>
#include <set>

#include "render.hpp"
#include "update.hpp"

namespace hdmon::gui {

int gui_rank(const std::string& s) {
    if (s == "CRITICAL") return 3;
    if (s == "WARNING") return 2;
    if (s == "OK") return 1;
    return 0;
}

std::string tray_status(const std::vector<Result>& results) {
    std::string w = "UNKNOWN";
    for (auto& r : results)
        if (gui_rank(r.second.status) > gui_rank(w)) w = r.second.status;
    return w;
}

std::string status_color(const std::string& s) {
    if (s == "OK") return palette::STATUS_OK;
    if (s == "WARNING") return palette::STATUS_WARNING;
    if (s == "CRITICAL") return palette::STATUS_CRITICAL;
    return palette::STATUS_UNKNOWN;
}

std::string row_tint(const std::string& s) {
    if (s == "OK") return palette::ROW_OK;
    if (s == "WARNING") return palette::ROW_WARNING;
    if (s == "CRITICAL") return palette::ROW_CRITICAL;
    return palette::ROW_UNKNOWN;
}

std::string status_label(const std::string& s) {
    if (s == "OK") return "OK";
    if (s == "WARNING") return "Warning";
    if (s == "CRITICAL") return "Critical";
    return "No SMART data";
}

std::string drive_name(const Drive& d) {
    if (!d.volumes.empty()) {
        std::vector<std::string> v(d.volumes.begin(), d.volumes.begin() + std::min<size_t>(3, d.volumes.size()));
        return join(v, " ") + (d.volumes.size() > 3 ? " …" : "");
    }
    if (starts_with(d.device, "PhysicalDrive")) return "Disk " + d.device.substr(13);
    std::string s = d.short_name();
    return s.empty() ? d.device : s;
}

std::string health_text(const Assessment& a) { return a.health ? std::to_string(*a.health) + "%" : "n/a"; }

std::string temp_text(const Drive& d) {
    if (!d.temperature_c || *d.temperature_c <= 0) return "\u2014";
    return std::to_string(*d.temperature_c) + " \u00B0C";
}

std::string size_text(const Drive& d) { return d.capacity_bytes ? fmt_size(d.capacity_bytes) : "\u2014"; }

std::vector<Result> sorted_for_display(std::vector<Result> results) {
    auto key = [](const Drive& d) {
        bool letter = !d.volumes.empty() && d.volumes[0].size() == 2 && d.volumes[0][1] == ':';
        return letter ? upper(d.volumes[0]) : "~" + d.device;
    };
    std::stable_sort(results.begin(), results.end(),
                     [&](const Result& a, const Result& b) { return key(a.first) < key(b.first); });
    return results;
}

static std::string clip(const std::string& t, size_t max_len) {
    if (t.size() <= max_len) return t;
    std::string s = t.substr(0, max_len > 3 ? max_len - 3 : 0);
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();   // whole UTF-8 chars
    if (!s.empty() && (static_cast<unsigned char>(s.back()) & 0x80)) s.pop_back();
    return s + "...";
}

std::string tooltip_text(const std::vector<Result>& results, const std::string& version, size_t max_len) {
    std::string t = "Hard Drive Monitor " + version;
    if (results.empty()) t += "\nNo drives found";
    for (auto& r : sorted_for_display(results)) t += "\n" + drive_name(r.first) + " " + health_text(r.second);
    return clip(t, max_len);
}
std::string tooltip_scanning(const std::string& version) { return "Hard Drive Monitor " + version + " - scanning..."; }
std::string tooltip_failed(const std::string& version) { return "Hard Drive Monitor " + version + " - last scan failed"; }

std::vector<std::pair<std::string, std::string>> detail_rows(const Drive& d, const Assessment& a) {
    std::vector<std::pair<std::string, std::string>> rows;
    auto put = [&](const std::string& k, const std::string& v) { rows.emplace_back(k, v); };
    auto num = [&](const std::string& k, const char* metric) {
        if (auto v = d.metric(metric)) put(k, with_commas(*v));
    };
    put("Status", status_label(a.status) + " (" + health_text(a) + " health" +
                      (a.performance ? ", " + std::to_string(*a.performance) + "% performance" : "") + ")");
    if (d.model) put("Model", *d.model);
    if (d.serial) put("Serial", *d.serial);
    put("Device", d.device + (d.kind != "Unknown" ? "  (" + d.kind + ")" : ""));
    if (d.capacity_bytes) put("Size", size_text(d));
    num("Reallocated sectors", "reallocated_sectors");
    num("Pending sectors", "pending_sectors");
    num("Offline uncorrectable", "offline_uncorrectable");
    num("Reported uncorrectable", "reported_uncorrectable");
    num("CRC errors", "udma_crc_errors");
    num("Command timeouts", "command_timeout");
    num("Media errors", "media_errors");
    num("Uncorrected errors", "uncorrected_errors");
    if (auto w = d.metric("wear_percent_used")) put("Wear (rated endurance used)", std::to_string(*w) + "%");
    if (d.power_on_hours) put("Power-on hours", with_commas(*d.power_on_hours));
    if (d.power_cycles) put("Power cycles", with_commas(*d.power_cycles));
    if (d.temperature_c && *d.temperature_c > 0) put("Temperature", temp_text(d));
    auto reasons = a.reasons();
    put("Notes", reasons.empty() ? (d.smart_available ? "No problems found" : join(d.notes, "; "))
                                 : join(reasons, "; "));
    return rows;
}

std::string tray_status(const std::vector<Result>& results, const std::vector<RaidArray>& arrays) {
    std::string w = tray_status(results);
    for (auto& a : arrays) {
        std::string st = array_status(a, results);
        if (gui_rank(st) > gui_rank(w)) w = st;
    }
    return w;
}

static std::vector<Result> top_level(const std::vector<Result>& results, const std::vector<RaidArray>& arrays,
                                     std::vector<int>* idx = nullptr) {
    std::set<std::string> under = member_keys(arrays);
    std::vector<std::pair<Result, int>> v;
    for (size_t i = 0; i < results.size(); ++i)
        if (!under.count(results[i].first.key())) v.push_back({results[i], static_cast<int>(i)});
    std::vector<Result> rs;
    for (auto& p : v) rs.push_back(p.first);
    std::vector<Result> sorted = sorted_for_display(rs);
    if (idx) {   // map back to indices into `results` (sorted_for_display is a stable sort over unique keys)
        idx->clear();
        std::vector<bool> used(v.size(), false);
        for (auto& r : sorted)
            for (size_t k = 0; k < v.size(); ++k)
                if (!used[k] && v[k].first.first.device == r.first.device && v[k].first.first.key() == r.first.key()) {
                    used[k] = true;
                    idx->push_back(v[k].second);
                    break;
                }
    }
    return sorted;
}

std::string tooltip_text(const std::vector<Result>& results, const std::vector<RaidArray>& arrays,
                         const std::string& version, size_t max_len) {
    if (arrays.empty()) return tooltip_text(results, version, max_len);
    std::string t = "Hard Drive Monitor " + version;
    for (auto& r : top_level(results, arrays)) t += "\n" + drive_name(r.first) + " " + health_text(r.second);
    for (auto& a : arrays) t += "\n" + a.name + " " + state_label(a, results);
    return clip(t, max_len);
}

std::vector<DisplayRow> display_rows(const std::vector<Result>& results, const std::vector<RaidArray>& arrays,
                                     const std::set<std::string>& collapsed) {
    std::vector<DisplayRow> rows;
    std::vector<int> idx;
    std::vector<Result> top = top_level(results, arrays, &idx);
    for (size_t i = 0; i < top.size(); ++i) {
        const auto& [d, a] = top[i];
        DisplayRow r;
        r.kind = DisplayRow::DRIVE;
        r.result = idx[i];
        r.status = a.status;
        r.cells[0] = drive_name(d);
        r.cells[1] = d.model.value_or("");
        r.cells[2] = size_text(d);
        r.cells[3] = temp_text(d);
        r.cells[4] = health_text(a);
        rows.push_back(r);
    }
    for (size_t ai = 0; ai < arrays.size(); ++ai) {
        const RaidArray& ar = arrays[ai];
        DisplayRow r;
        r.kind = DisplayRow::ARRAY;
        r.array = static_cast<int>(ai);
        r.bold = true;
        r.expanded = !collapsed.count(ar.id);
        r.status = array_status(ar, results);
        r.cells[0] = std::string(r.expanded ? "\u25BC " : "\u25B6 ") + ar.name;
        r.cells[1] = (ar.level.empty() ? std::string("RAID") : ar.level) + " \u00B7 " + backend_label(ar.backend);
        r.cells[2] = ar.size_bytes ? fmt_size(ar.size_bytes) : "\u2014";
        r.cells[3] = "\u2014";
        r.cells[4] = state_label(ar, results);
        rows.push_back(r);
        if (!r.expanded) continue;
        for (size_t mi = 0; mi < ar.members.size(); ++mi) {
            const ArrayMember& m = ar.members[mi];
            DisplayRow c;
            c.kind = DisplayRow::MEMBER;
            c.array = static_cast<int>(ai);
            c.member = static_cast<int>(mi);
            const Result* mr = member_result(m, results);
            std::string role = m.role == "active" ? "" : " (" + m.role + ")";
            std::string rt = member_role_text(m);
            c.status = member_status(m, mr);   // failed/missing: Critical whatever SMART says; rebuilding: Warning; spare: OK
            if (mr) {
                c.result = static_cast<int>(mr - results.data());
                std::string label = disk_label(mr->first);
                c.cells[0] = "      " + (label.empty() ? drive_name(mr->first) : label) + role;
                c.cells[1] = mr->first.model.value_or("");
                c.cells[2] = size_text(mr->first);
                c.cells[3] = temp_text(mr->first);
                c.cells[4] = rt.empty() ? health_text(mr->second) : rt;
            } else {
                std::string dev = m.device.empty() ? std::string("(missing disk)") : m.device;
                if (starts_with(dev, "/dev/")) dev = dev.substr(5);
                c.cells[0] = "      " + dev + role;
                c.cells[1] = m.serial ? "serial " + *m.serial : "";
                c.cells[2] = m.size_bytes ? fmt_size(m.size_bytes) : "\u2014";
                c.cells[3] = "\u2014";
                c.cells[4] = rt.empty() ? "n/a" : rt;
            }
            rows.push_back(c);
        }
    }
    return rows;
}

std::vector<std::pair<std::string, std::string>> array_detail_rows(const RaidArray& a, const std::vector<Result>& results) {
    std::vector<std::pair<std::string, std::string>> rows;
    auto put = [&](const std::string& k, const std::string& v) { rows.emplace_back(k, v); };
    put("Status", status_label(array_status(a, results)) + " (" + state_label(a, results) + ")");
    put("Name", a.name);
    if (!a.level.empty()) put("Type", a.level);
    put("Backend", backend_label(a.backend));
    if (!a.raw_state.empty()) put("State", a.raw_state);
    if (a.rebuild_pct) put("Rebuild", fmt_fixed(*a.rebuild_pct, 1) + "%");
    if (a.size_bytes) put("Size", fmt_size(a.size_bytes));
    if (!a.volumes.empty()) put("Mounted at", join(a.volumes, " "));
    for (auto& m : a.members) {
        const Result* r = member_result(m, results);
        std::string label = r ? (disk_label(r->first).empty() ? drive_name(r->first) : disk_label(r->first)) : m.device;
        std::string rt = member_role_text(m);
        std::string v = (rt.empty() ? std::string("Active") : rt) + " \u00B7 " + status_label(member_status(m, r)) +
                        (r ? ", SMART " + health_text(r->second) + (r->first.model ? ", " + *r->first.model : "") : ", no SMART data");
        put("Member " + (m.slot ? std::to_string(*m.slot) : std::string("-")) + " " + label, v);
    }
    if (!a.id.empty()) put("Id", a.id);
    if (!a.notes.empty()) put("Notes", join(a.notes, "; "));
    return rows;
}

bool used_smartctl(const std::vector<Result>& results, const json& meta) {
    if (jstr(meta, "source") == std::optional<std::string>("json-files")) return false;
    for (auto& r : results)
        if (starts_with(r.first.source, "smartctl")) return true;
    return false;
}

std::string source_text(const std::vector<Result>& results, const json& meta) {
    if (jstr(meta, "source") == std::optional<std::string>("json-files")) return "sample data (--from-json)";
    int native = 0, sc = 0, other = 0;
    for (auto& r : results) {
        if (starts_with(r.first.source, "native")) ++native;
        else if (starts_with(r.first.source, "smartctl")) ++sc;
        else ++other;
    }
    std::vector<std::string> parts;
    if (native) parts.push_back("native SMART");
    if (sc) parts.push_back("smartctl" + (jstr(meta, "smartctl_version") ? " " + *jstr(meta, "smartctl_version") : std::string()) +
                            (jstr(meta, "smart_source_setting") == std::optional<std::string>("smartctl") ? "" : " (fallback)"));
    if (other) parts.push_back("OS inventory");
    return parts.empty() ? "" : "source: " + join(parts, " + ");
}

GuiSettings load_gui_settings(const json& cfg) {
    GuiSettings s;
    const json& g = jget(cfg, "gui");
    const json& a = jget(g, "alert_on_danger");
    if (a.is_boolean()) s.alert_on_danger = a.get<bool>();
    if (auto n = opt_int(jget(g, "scan_interval_minutes"))) s.scan_interval_minutes = static_cast<int>(std::clamp<int64_t>(*n, 1, 1440));
    const json& u = jget(cfg, "update");
    s.auto_check = truthy(jget(u, "auto_check"));
    return s;
}

bool save_gui_settings(const std::string& path, const GuiSettings& s, std::string* err) {
    auto b = [](bool v) { return std::string(v ? "true" : "false"); };
    int n = std::clamp(s.scan_interval_minutes, 1, 1440);
    return set_config_value(path, "gui", "alert_on_danger", b(s.alert_on_danger), err) &&
           set_config_value(path, "gui", "scan_interval_minutes", std::to_string(n), err) &&
           set_config_value(path, "update", "auto_check", b(s.auto_check), err);
}

std::vector<int> fit_column_widths(const std::vector<int>& mins, const std::vector<int>& header, int avail, size_t slack) {
    const size_t n = mins.size();
    std::vector<int> hdr(n, 0), want(n), w(n);
    long sum_hdr = 0, sum_want = 0;
    for (size_t i = 0; i < n; ++i) {
        if (i < header.size()) hdr[i] = std::max(0, header[i]);
        want[i] = std::max(mins[i], hdr[i]);
        sum_hdr += hdr[i];
        sum_want += want[i];
    }
    if (n == 0) return w;
    if (slack >= n) slack = 0;
    if (sum_hdr >= avail) return hdr;
    if (sum_want <= avail) {
        w = want;
        w[slack] += avail - static_cast<int>(sum_want);
        return w;
    }
    std::vector<bool> fixed(n, false);
    for (;;) {
        long room = avail, scalable = 0;
        for (size_t i = 0; i < n; ++i) (fixed[i] ? room -= hdr[i] : scalable += want[i]);
        bool changed = false;
        for (size_t i = 0; i < n; ++i) {
            if (fixed[i]) { w[i] = hdr[i]; continue; }
            w[i] = scalable > 0 ? static_cast<int>(static_cast<long long>(want[i]) * room / scalable) : hdr[i];
            if (w[i] < hdr[i]) { fixed[i] = true; changed = true; }
        }
        if (!changed) break;
    }
    long total = 0;
    for (int x : w) total += x;
    w[slack] += avail - static_cast<int>(total);     // rounding remainder (always >= 0)
    return w;
}

long parse_pid_file(const std::string& text) {
    size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    long pid = 0;
    size_t start = i;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
        if (pid > 100000000L) return 0;
        pid = pid * 10 + (text[i++] - '0');
    }
    if (i == start || (i < text.size() && text[i] != '\n' && text[i] != '\r' && text[i] != ' ')) return 0;
    return pid;
}

bool is_gui_image(const std::string& image) {
    std::string n = image;
    const std::string del = " (deleted)";
    if (n.size() > del.size() && n.compare(n.size() - del.size(), del.size(), del) == 0) n.resize(n.size() - del.size());
    size_t slash = n.find_last_of("/\\");
    if (slash != std::string::npos) n = n.substr(slash + 1);
    for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return n == "hdmon-gui" || n == "hdmon-gui.exe" || n == "hdmon-gui.old.exe" ||
           n == "hard drive monitor";   // macOS app bundle executable
}

}  // namespace hdmon::gui
