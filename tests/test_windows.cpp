// Port of tests/test_windows.py (Windows code paths, tested on any OS with synthetic PowerShell JSON)
#include <algorithm>

#include "helpers.hpp"
#include "collect.hpp"
#include "config.hpp"

using namespace hdmon;
using th::load;

TEST_SUITE("windows") {
TEST_CASE("letter map") {
    auto m = windows_letter_map(load("windows/windows_inventory.json"));
    std::map<int64_t, std::vector<std::string>> want = {{0, {"C:"}}, {1, {"D:"}}, {2, {"G:"}}};
    CHECK(m == want);
}

TEST_CASE("fallback parse") {
    auto drives = parse_windows_physical(load("windows/windows_inventory.json"));
    REQUIRE(drives.size() == 3);
    CHECK(drives[0].device == "PhysicalDrive0");
    CHECK(drives[1].device == "PhysicalDrive1");
    CHECK(drives[2].device == "PhysicalDrive2");
    auto &nvme = drives[0], &hdd = drives[1], &usb = drives[2];
    CHECK(nvme.kind == "NVMe");
    CHECK(nvme.volumes == std::vector<std::string>{"C:"});
    CHECK(nvme.metric("wear_percent_used") == std::optional<int64_t>(4));
    CHECK(hdd.kind == "HDD");
    CHECK(hdd.volumes == std::vector<std::string>{"D:"});
    CHECK(hdd.temperature_c == std::optional<int64_t>(44));
    CHECK(hdd.power_on_hours == std::optional<int64_t>(41877));
    CHECK(hdd.metric("uncorrected_errors") == std::optional<int64_t>(7));
    CHECK(usb.volumes == std::vector<std::string>{"G:"});
    CHECK(usb.smart_passed == std::optional<bool>(false));
}

TEST_CASE("fallback scoring") {
    std::map<std::string, Assessment> res;
    for (auto& [d, a] : score(parse_windows_physical(load("windows/windows_inventory.json")), default_config())) res[d.device] = a;
    CHECK(res["PhysicalDrive0"].status == "OK");
    CHECK(res["PhysicalDrive1"].status == "WARNING");
    CHECK(res["PhysicalDrive2"].status == "CRITICAL");
    auto r = res["PhysicalDrive2"].reasons();
    CHECK(std::any_of(r.begin(), r.end(), [](const std::string& x) { return th::has(x, "Unhealthy"); }));
}

TEST_CASE("smartctl device to disk number") {
    json empty = json::object();
    CHECK(windows_disk_number("/dev/sdc", std::nullopt, empty) == std::optional<int64_t>(2));
    CHECK(windows_disk_number("/dev/sdaa", std::nullopt, empty) == std::optional<int64_t>(26));
    CHECK(windows_disk_number("/dev/pd3", std::nullopt, empty) == std::optional<int64_t>(3));
    CHECK(windows_disk_number("/dev/sdb", std::string("0025 3852 91B0 SYN1"), load("windows/windows_inventory.json")) ==
          std::optional<int64_t>(0));
    CHECK(norm_serial("0025_3852_91B0_SYN1.") == "0025385291B0SYN1");
}

TEST_CASE("single-object JSON and letters applied") {
    json inv = load("windows/windows_letters_single.json");
    std::vector<Drive> ds = {parse_smartctl(load("hdd_failing.json"))};
    apply_windows_letters(ds, inv);
    CHECK(ds[0].volumes == std::vector<std::string>{"D:"});
    CHECK(std::any_of(ds[0].notes.begin(), ds[0].notes.end(), [](const std::string& n) { return th::has(n, "PhysicalDrive1"); }));
}

TEST_CASE("no reliability counters") {
    json inv = {{"PhysicalDisks", {{"DeviceId", "5"}, {"FriendlyName", "X"}, {"MediaType", "Unspecified"},
                                   {"BusType", "USB"}, {"HealthStatus", "Unknown"}, {"Reliability", nullptr}}}};
    CHECK_FALSE(parse_windows_physical(inv)[0].smart_available);
}
}
