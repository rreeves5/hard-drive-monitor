// smartctl discovery + JSON parsing (all OSes) and the Windows PowerShell fallback.
#pragma once
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "util.hpp"

namespace hdmon {

struct DeviceRef {
    std::string name;
    std::optional<std::string> dev_type, protocol, note;
    std::string source = "smartctl";
    std::optional<std::string> model, serial, transport;
    std::optional<int64_t> capacity_bytes;
    std::optional<bool> rotational;
    std::vector<std::string> volumes;
};

struct Drive {
    std::string device;
    std::optional<std::string> dev_type, protocol, model, serial, firmware;
    std::optional<int64_t> capacity_bytes;
    std::string kind = "Unknown";   // HDD | SSD | NVMe | Unknown
    std::optional<int64_t> rotation_rate;
    std::optional<std::string> form_factor, interface;
    std::optional<int64_t> temperature_c, temp_limit_warn, temp_limit_crit, power_on_hours, power_cycles;
    bool smart_available = true;
    std::optional<bool> smart_passed;
    json metrics = json::object();
    std::vector<json> ata_attributes;
    json nvme_log = json::object();
    json windows_reliability = json::object();
    json self_test = json::object();
    std::vector<json> self_test_log;
    std::vector<std::string> volumes;
    std::vector<std::string> notes;
    std::string source = "smartctl";

    std::string key() const;         // serial when known, else model+device
    std::string short_name() const;  // "/dev/sda" -> "sda"
    json to_json() const;
    std::optional<int64_t> metric(const char* k) const { return opt_int(jget(metrics, k)); }
};

class SmartctlMissing : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::optional<std::string> find_smartctl(const std::string& configured = "");
std::string install_hint(std::optional<OS> os = std::nullopt);
std::optional<std::pair<int, int>> parse_version(const std::string& text);
std::string messages_text(const json& data);

class Smartctl {
public:
    Smartctl(std::string path, json sudo_mode);
    std::optional<std::pair<int, int>> version();
    // Runs smartctl; data is parsed stdout JSON (null if none). Retries with `sudo -n` in "auto" mode.
    int run(const std::vector<std::string>& args, json& data, std::string& err, int timeout_s = 180);
    std::string path;
    bool use_sudo = false;
    bool permission_problem = false;
private:
    std::string sudo_mode_;
    bool can_sudo() const;
    int exec(const std::vector<std::string>& args, bool sudo, int timeout_s, json& data, std::string& err);
};

int64_t ata_raw_count(int attr_id, const json& raw, bool* ok = nullptr);
Drive parse_smartctl(const json& data, const std::string& device = "");

std::vector<DeviceRef> discover(Smartctl* sc);
Drive read_drive(Smartctl& sc, const DeviceRef& ref);
std::string elevation_hint();
std::vector<Drive> load_json_files(const std::vector<std::string>& paths);

// Windows
extern const char* PS_LETTERS;
extern const char* PS_PHYSICAL;
json run_powershell_json(const std::string& script, int timeout_s = 90);
std::optional<std::string> powershell_exe();
std::string norm_serial(const std::string& s);
std::map<int64_t, std::vector<std::string>> windows_letter_map(const json& inv);
std::optional<int64_t> windows_disk_number(const std::string& device, const std::optional<std::string>& serial,
                                           const json& inv);
std::vector<Drive> parse_windows_physical(const json& inv);

}  // namespace hdmon
