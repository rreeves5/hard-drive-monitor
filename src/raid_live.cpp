// Live RAID detection per OS. Native sources first; external tools only where no native API is practical.
#include "raid.hpp"

#include "smart.hpp"
#include "wmi.hpp"

namespace hdmon {

std::vector<RaidArray> detect_live_software_arrays(json& meta) {
    std::vector<RaidArray> out;
    switch (current_os()) {
        case OS::Linux: {
            out = detect_md_sysfs();
            // Fallback only when sysfs has no md directory (very old kernels / containers): mdadm --detail.
            for (auto& a : out) {
                if (a.source == "sysfs" || !which("mdadm")) continue;
                auto r = run_cmd({"mdadm", "--detail", "/dev/" + a.name}, 20);
                if (r.rc == 0) { merge_md_detail(a, parse_mdadm_detail(r.out, a.name)); a.source = "mdadm --detail"; }
            }
            break;
        }
        case OS::Windows: {
            std::string how;
            json inv = windows_storage_spaces(how);
            if (inv.is_object() && inv.contains("StoragePools")) {
                for (auto& a : parse_storage_spaces(inv)) { a.source = "Storage Spaces (" + how + ")"; out.push_back(a); }
            }
            for (const char* exe : {"rstcli64", "rstcli64.exe", "rstcli", "rstcli.exe"}) {
                auto p = which(exe);
                if (!p) continue;
                auto r = run_cmd({*p, "--information", "--volume"}, 30);
                if (r.rc == 0) for (auto& a : parse_rstcli(r.out)) out.push_back(a);
                break;
            }
            break;
        }
        case OS::MacOS: {
            // IOKit registry (AppleRAIDSet / AppleRAIDMember objects); diskutil only if IOKit could not be asked.
            std::string err;
            json snap = appleraid_ioreg_snapshot(err);
            if (err.empty()) {
                for (auto& a : parse_appleraid_ioreg(snap)) out.push_back(a);
            } else {
                auto r = run_cmd({"/usr/sbin/diskutil", "appleRAID", "list"}, 30);
                if (r.rc == 0) for (auto& a : parse_appleraid(r.out)) out.push_back(a);
            }
            break;
        }
    }
    if (!out.empty()) meta["raid_backends"] = [&] {
        json b = json::array();
        for (auto& a : out) if (std::find(b.begin(), b.end(), a.backend) == b.end()) b.push_back(a.backend);
        return b;
    }();
    return out;
}

}  // namespace hdmon
