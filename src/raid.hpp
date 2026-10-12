// RAID arrays: Linux md, Windows Storage Spaces, Intel RST, macOS AppleRAID and hardware RAID controllers
// (member disks via smartctl -d megaraid,N / aacraid / areca / 3ware / cciss / hpt / csmi).
#pragma once
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "history.hpp"

namespace hdmon {

struct ArrayMember {
    std::string device;              // as the backend reports it: /dev/sdb1, disk2s2, PhysicalDisk serial, megaraid,0
    std::string role = "active";     // active | spare | failed | rebuilding | missing | journal
    std::optional<int> slot;
    std::optional<std::string> serial;
    std::optional<int64_t> size_bytes;
    std::string state_text;          // raw member state from the backend
    std::string drive_key;           // Drive::key() of the matched physical disk ("" = no SMART data for it)
};

struct RaidArray {
    std::string id;                  // stable: backend:uuid-or-name
    std::string name;                // md0, Pool "Data", MegaRAID /dev/bus/0
    std::string level;               // raid1, Mirror, Parity, RAID 5, hardware
    std::string backend;             // mdadm | storage-spaces | intel-rst | appleraid | megaraid | aacraid | areca | 3ware | cciss | hpt | csmi
    std::string state = "UNKNOWN";   // OK | DEGRADED | REBUILDING | FAILED | UNKNOWN
    std::string raw_state;
    std::optional<double> rebuild_pct;
    std::optional<int64_t> size_bytes;
    std::vector<ArrayMember> members;
    std::vector<std::string> volumes;   // mount points / letters of the array itself
    std::vector<std::string> notes;
    std::string source;              // live backend or fixture file
    json to_json() const;
};

// ---- health rollup
std::string state_status(const std::string& state);        // OK->OK, DEGRADED/REBUILDING->WARNING, FAILED->CRITICAL
std::string array_status(const RaidArray& a, const std::vector<Result>& results);
std::string state_text(const RaidArray& a);                 // "Degraded", "Rebuilding 45.0%", ...
// state_text, except an array whose own state is not readable (hardware controllers) says so honestly:
// "Worst disk OK (controller state not readable)".
std::string state_label(const RaidArray& a, const std::vector<Result>& results, bool brief = false);   // brief: "Worst disk OK"
// A member's status inside its array. The role wins when it is worse than SMART:
//   failed / missing -> CRITICAL ("Failed (removed from array)" / "Missing from array"), whatever SMART says
//   rebuilding       -> at least WARNING ("Rebuilding")
//   spare            -> its SMART status, OK when there is none ("Spare")
//   active           -> its SMART status (UNKNOWN without SMART data)
std::string member_status(const ArrayMember& m, const Result* r);
std::string member_role_text(const ArrayMember& m);         // "", "Failed (removed from array)", "Spare", ...
std::string backend_label(const std::string& backend);      // "Linux md", "Storage Spaces", ...
const Result* member_result(const ArrayMember& m, const std::vector<Result>& results);
std::set<std::string> member_keys(const std::vector<RaidArray>& arrays);   // drive keys shown under an array
void link_members(std::vector<RaidArray>& arrays, const std::vector<Result>& results);
json arrays_json(const std::vector<RaidArray>& arrays, const std::vector<Result>& results);

// ---- parsers (pure; fixture-tested)
std::string md_member_disk(const std::string& member);   // sdb1 -> sdb, nvme0n1p2 -> nvme0, disk2s2 -> disk2
std::vector<RaidArray> parse_mdstat(const std::string& text);
RaidArray parse_mdadm_detail(const std::string& text, const std::string& name = "");
void merge_md_detail(RaidArray& from_mdstat, const RaidArray& detail);
std::vector<RaidArray> parse_storage_spaces(const json& inv);
std::vector<RaidArray> parse_appleraid(const std::string& text);   // diskutil appleRAID list (fallback)
// IOKit registry snapshot of AppleRAID objects (keys from Apple's AppleRAIDUserLib.h):
// {"sets":[{AppleRAID-SetName, -SetUUID, -LevelName, -SetStatus, -Members, -Spares, "_bsd", "_size"}],
//  "members":[{AppleRAID-MemberUUID, -SetUUID, -MemberStatus, -MemberIndex, -Rebuild-Progress, "_bsd", "_size"}]}
std::vector<RaidArray> parse_appleraid_ioreg(const json& snapshot);
json appleraid_ioreg_snapshot(std::string& err);   // macOS live (IOKit); {} elsewhere
std::vector<RaidArray> parse_rstcli(const std::string& text);
bool is_raid_dev_type(const std::string& dev_type);        // megaraid,N aacraid,H,L,ID areca,N 3ware,N cciss,N hpt,L/M/N
std::vector<RaidArray> hardware_arrays(const std::vector<Result>& results);
bool is_controller_volume(const Drive& d);
std::string disk_label(const Drive& d);                    // sdb, nvme0, megaraid,0 (controller members)

// ---- live detection (native first; tools only as fallback)
std::vector<RaidArray> detect_md_sysfs(const std::string& sys_root = "/sys", const std::string& mdstat_path = "/proc/mdstat");
std::vector<RaidArray> detect_live_software_arrays(json& meta);   // md / Storage Spaces / AppleRAID / RST on this OS

// ---- fixtures: text/JSON RAID sources found in --from-json paths
std::vector<RaidArray> load_raid_fixtures(const std::vector<std::string>& paths);
bool is_raid_fixture_json(const json& data);

}  // namespace hdmon
