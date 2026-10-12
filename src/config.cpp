#include "config.hpp"

#include <cctype>
#include <filesystem>

namespace hdmon {

std::string home_dir() {
#ifdef _WIN32
    std::string h = get_env("USERPROFILE");
    return h.empty() ? "C:\\" : h;
#else
    std::string h = get_env("HOME");
    return h.empty() ? "/tmp" : h;
#endif
}

std::string data_dir() {
    switch (current_os()) {
        case OS::Windows: {
            std::string b = get_env("LOCALAPPDATA");
            if (b.empty()) b = path_join(path_join(home_dir(), "AppData"), "Local");
            return path_join(b, "hard-drive-monitor");
        }
        case OS::MacOS:
            return path_join(path_join(path_join(home_dir(), "Library"), "Application Support"), "hard-drive-monitor");
        default: {
            std::string b = get_env("XDG_DATA_HOME");
            if (b.empty()) b = path_join(path_join(home_dir(), ".local"), "share");
            return path_join(b, "hard-drive-monitor");
        }
    }
}
std::string default_db() { return path_join(data_dir(), "history.db"); }
std::string config_path() {
    if (current_os() == OS::Windows) {
        std::string b = get_env("APPDATA");
        if (b.empty()) b = path_join(path_join(home_dir(), "AppData"), "Roaming");
        return path_join(path_join(b, "hard-drive-monitor"), "config.toml");
    }
    std::string b = get_env("XDG_CONFIG_HOME");
    if (b.empty()) b = path_join(home_dir(), ".config");
    return path_join(path_join(b, "hard-drive-monitor"), "config.toml");
}

const char* SAMPLE_CONFIG = R"(# Hard Drive Monitor (hdmon) configuration
# Location: ~/.config/hard-drive-monitor/config.toml  (Windows: %APPDATA%\hard-drive-monitor\config.toml)

[smart]
# Where SMART data comes from:
#   "auto"     - read it natively through the OS (default); use smartctl only for drives the native
#                read cannot handle, and only if smartmontools happens to be installed
#   "native"   - native only, never run smartctl
#   "smartctl" - smartmontools only (needs it installed)
source = "auto"

[raid]
# Show RAID arrays (Linux md, Windows Storage Spaces, Intel RST, macOS AppleRAID, hardware RAID cards).
enabled = true
# Extra controller members for smartctl when --scan-open does not find them, e.g.
# smartctl_devices = ["/dev/sg0 -d cciss,0", "/dev/sda -d hpt,1/1/1"]
smartctl_devices = []

[smartctl]
# Optional fallback only: hdmon does not need smartmontools.
# path = "/usr/sbin/smartctl"   # leave unset to auto-detect
# "auto": if smartctl gets "permission denied", retry once with `sudo -n` (needs a sudoers rule)
sudo = "auto"

[alerts]
desktop = true

[email]
# Sent with the `curl` command-line tool (built into Windows 10+, macOS and most Linux distros).
enabled = false
host = "smtp.gmail.com"
port = 587
starttls = true
ssl = false
username = "you@example.com"
# Prefer an environment variable over a password in this file:
password_env = "HDMON_SMTP_PASSWORD"
# password = ""
from = "you@example.com"
to = ["you@example.com"]

[thresholds]
hdd_temp_warn = 50
hdd_temp_crit = 55
ssd_temp_warn = 60
ssd_temp_crit = 70
nvme_temp_warn = 70
nvme_temp_crit = 80

[ignore]
devices = []

[update]
# Self-update from GitHub releases (github.com/rreeves5/hard-drive-monitor). OFF by default.
# `hdmon update --check` / `hdmon update` always work by hand, whatever these say.
# auto_check: the scheduled `hdmon check` looks for a new release at most once a day and only
#             shows a desktop notification - it never installs anything.
auto_check = false
)";

json default_config() {
    json c = json::object();
    c["smart"] = {{"source", "auto"}};
    c["raid"] = {{"enabled", true}, {"smartctl_devices", json::array()}};
    c["smartctl"] = {{"path", ""}, {"sudo", "auto"}};
    c["alerts"] = {{"desktop", true}};
    c["email"] = {{"enabled", false}, {"host", ""}, {"port", 587}, {"starttls", true}, {"ssl", false},
                  {"username", ""}, {"password", ""}, {"password_env", "HDMON_SMTP_PASSWORD"},
                  {"from", ""}, {"to", json::array()}};
    c["thresholds"] = {{"hdd_temp_warn", 50}, {"hdd_temp_crit", 55}, {"ssd_temp_warn", 60},
                       {"ssd_temp_crit", 70}, {"nvme_temp_warn", 70}, {"nvme_temp_crit", 80}};
    c["ignore"] = {{"devices", json::array()}};
    c["update"] = {{"auto_check", false}};   // opt-in only, notify only
    return c;
}

static json parse_value(std::string raw) {
    raw = trim(raw);
    if (raw.empty()) return "";
    if (raw[0] == '"') {
        std::string out;
        for (size_t i = 1; i < raw.size(); ++i) {
            char c = raw[i];
            if (c == '\\' && i + 1 < raw.size()) {
                char n = raw[++i];
                switch (n) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case '\\': out += '\\'; break;
                    case '"': out += '"'; break;
                    default: out += '\\'; out += n;
                }
            } else if (c == '"') {
                return out;
            } else {
                out += c;
            }
        }
        return out;
    }
    if (raw[0] == '\'') {
        size_t end = raw.find('\'', 1);
        return raw.substr(1, end == std::string::npos ? std::string::npos : end - 1);
    }
    if (raw[0] == '[') {
        size_t end = raw.rfind(']');
        std::string inner = raw.substr(1, end == std::string::npos ? std::string::npos : end - 1);
        json items = json::array();
        std::string buf;
        char q = 0;
        for (char ch : inner) {
            if (q) { buf += ch; if (ch == q) q = 0; }
            else if (ch == '"' || ch == '\'') { q = ch; buf += ch; }
            else if (ch == ',') { if (!trim(buf).empty()) items.push_back(parse_value(buf)); buf.clear(); }
            else buf += ch;
        }
        if (!trim(buf).empty()) items.push_back(parse_value(buf));
        return items;
    }
    raw = trim(raw.substr(0, raw.find('#')));
    if (raw == "true") return true;
    if (raw == "false") return false;
    std::string digits = replace_all(raw, "_", "");
    try {
        size_t pos = 0;
        long long v = std::stoll(digits, &pos);
        if (pos == digits.size()) return v;
    } catch (...) {}
    try {
        size_t pos = 0;
        double v = std::stod(raw, &pos);
        if (pos == raw.size()) return v;
    } catch (...) {}
    return raw;
}

json parse_simple_toml(const std::string& text) {
    json out = json::object();
    json* section = &out;
    for (auto& line : split_lines(text)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;
        if (s.front() == '[' && s.back() == ']') {
            section = &out;
            for (auto& part : split(trim(s.substr(1, s.size() - 2)), '.')) {
                std::string k = trim(part);
                if (!section->contains(k) || !(*section)[k].is_object()) (*section)[k] = json::object();
                section = &(*section)[k];
            }
            continue;
        }
        auto eq = s.find('=');
        if (eq != std::string::npos) {
            std::string key = trim(s.substr(0, eq));
            if (key.size() >= 2 && key.front() == '"' && key.back() == '"') key = key.substr(1, key.size() - 2);
            (*section)[key] = parse_value(s.substr(eq + 1));
        }
    }
    return out;
}

json merge_config(const json& base, const json& over) {
    json out = base;
    if (!over.is_object()) return out;
    for (auto it = over.begin(); it != over.end(); ++it) {
        if (it.value().is_object() && out.contains(it.key()) && out[it.key()].is_object())
            out[it.key()] = merge_config(out[it.key()], it.value());
        else
            out[it.key()] = it.value();
    }
    return out;
}

json load_config(const std::string& path) {
    std::string p = path.empty() ? config_path() : path;
    if (file_exists(p)) return merge_config(default_config(), parse_simple_toml(read_file(p)));
    return default_config();
}

std::string write_sample_config(const std::string& path, bool force, std::string* err) {
    std::string p = path.empty() ? config_path() : path;
    if (file_exists(p) && !force) { if (err) *err = p + " already exists (use --force to overwrite)"; return ""; }
    make_dirs(dirname(p));
    if (!write_file(p, SAMPLE_CONFIG)) { if (err) *err = "cannot write " + p; return ""; }
    std::error_code ec;
    std::filesystem::permissions(p, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, ec);  // may hold SMTP credentials
    return p;
}

}  // namespace hdmon
