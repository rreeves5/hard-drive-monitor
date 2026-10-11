#include "gui_model.hpp"

#include <algorithm>

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

GuiSettings load_gui_settings(const json& cfg) {
    GuiSettings s;
    const json& g = jget(cfg, "gui");
    const json& a = jget(g, "alert_on_danger");
    if (a.is_boolean()) s.alert_on_danger = a.get<bool>();
    if (auto n = opt_int(jget(g, "scan_interval_minutes"))) s.scan_interval_minutes = static_cast<int>(std::clamp<int64_t>(*n, 1, 1440));
    const json& u = jget(cfg, "update");
    s.auto_check = truthy(jget(u, "auto_check"));
    s.auto_install = truthy(jget(u, "auto_install"));
    return s;
}

bool save_gui_settings(const std::string& path, const GuiSettings& s, std::string* err) {
    auto b = [](bool v) { return std::string(v ? "true" : "false"); };
    int n = std::clamp(s.scan_interval_minutes, 1, 1440);
    return set_config_value(path, "gui", "alert_on_danger", b(s.alert_on_danger), err) &&
           set_config_value(path, "gui", "scan_interval_minutes", std::to_string(n), err) &&
           set_config_value(path, "update", "auto_check", b(s.auto_check), err) &&
           set_config_value(path, "update", "auto_install", b(s.auto_install), err);
}

}  // namespace hdmon::gui
