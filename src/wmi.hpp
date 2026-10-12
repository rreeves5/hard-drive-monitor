// Windows storage inventory through WMI (COM, root\Microsoft\Windows\Storage MSFT_* classes).
// PowerShell (Get-PhysicalDisk & co.) remains only as a fallback when COM/WMI is unavailable.
// On other OSes these return an empty object.
#pragma once
#include <string>

#include "util.hpp"

namespace hdmon {

// {"StoragePools":[...], "VirtualDisks":[...], "PhysicalDisks":[...], "StorageJobs":[...]}; how = "WMI" | "PowerShell"
json windows_storage_spaces(std::string& how);
// Same shape as PS_PHYSICAL: {"PhysicalDisks":[{..., "Reliability":{...}}], "Partitions":[...], "Disks":[...]}
json windows_physical_inventory(std::string& how);

// Value tables used to turn MSFT_* numeric enums into the strings PowerShell prints (exposed for tests).
std::string msft_health(int v);
std::string msft_operational(int v);
std::string msft_usage(int v);
std::string msft_media(int v);
std::string msft_bus(int v);
std::string msft_job_state(int v);

}  // namespace hdmon
