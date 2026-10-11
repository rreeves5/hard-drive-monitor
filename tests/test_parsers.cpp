// Port of tests/test_parsers.py
#include "helpers.hpp"
#include "smart.hpp"

using namespace hdmon;
using th::load;

TEST_SUITE("parsers") {
TEST_CASE("fixtures are marked synthetic") {
    for (auto n : {"hdd_healthy.json", "hdd_failing.json", "ssd_sata_healthy.json", "nvme_healthy.json", "nvme_worn.json",
                   "later/hdd_failing.json", "windows/windows_inventory.json"}) {
        INFO(n);
        CHECK(th::has(load(n)["_synthetic"].get<std::string>(), "SYNTHETIC"));
    }
}

TEST_CASE("ATA healthy HDD") {
    Drive d = parse_smartctl(load("hdd_healthy.json"));
    CHECK(d.device == "/dev/sda");
    CHECK(d.kind == "HDD");
    CHECK(d.model == std::optional<std::string>("WDC WD40EFRX-68N32N0"));
    CHECK(d.serial == std::optional<std::string>("WD-SYNTH00000001"));
    CHECK(d.capacity_bytes == std::optional<int64_t>(4000787030016LL));
    CHECK(d.temperature_c == std::optional<int64_t>(34));
    CHECK(d.power_on_hours == std::optional<int64_t>(23456));
    CHECK(d.power_cycles == std::optional<int64_t>(312));
    CHECK(d.smart_passed == std::optional<bool>(true));
    for (auto k : {"reallocated_sectors", "pending_sectors", "offline_uncorrectable", "udma_crc_errors"}) {
        INFO(k);
        CHECK(d.metric(k) == std::optional<int64_t>(0));
    }
    CHECK_FALSE(d.metrics.contains("wear_percent_used"));
    CHECK(d.self_test["extended_minutes"] == 470);
    CHECK(d.self_test_log.size() == 2);
}

TEST_CASE("ATA failing HDD") {
    Drive d = parse_smartctl(load("hdd_failing.json"));
    CHECK(d.metric("reallocated_sectors") == std::optional<int64_t>(120));
    CHECK(d.metric("pending_sectors") == std::optional<int64_t>(16));
    CHECK(d.metric("offline_uncorrectable") == std::optional<int64_t>(16));
    CHECK(d.metric("reported_uncorrectable") == std::optional<int64_t>(3));
    CHECK(d.metric("command_timeout") == std::optional<int64_t>(1));   // Seagate packed raw -> low 16 bits
    CHECK_FALSE(d.self_test_log[0]["passed"].get<bool>());
    CHECK(d.self_test_log[0]["lba"] == 1052763384);
}

TEST_CASE("SATA SSD wear from device statistics") {
    Drive d = parse_smartctl(load("ssd_sata_healthy.json"));
    CHECK(d.kind == "SSD");
    CHECK(d.metric("wear_percent_used") == std::optional<int64_t>(6));
    CHECK(d.metrics["wear_source"] == "device statistics");
}

TEST_CASE("SATA SSD wear from attribute fallback") {
    json data = load("ssd_sata_healthy.json");
    data.erase("ata_device_statistics");
    Drive d = parse_smartctl(data);
    CHECK(d.metric("wear_percent_used") == std::optional<int64_t>(6));
    CHECK(th::has(d.metrics["wear_source"].get<std::string>(), "177"));
}

TEST_CASE("temperature from attribute 194") {
    json data = load("hdd_healthy.json");
    data.erase("temperature");
    CHECK(parse_smartctl(data).temperature_c == std::optional<int64_t>(34));
}

TEST_CASE("when_failed now") {
    json data = load("hdd_failing.json");
    data["ata_smart_attributes"]["table"][2]["when_failed"] = "now";
    CHECK(parse_smartctl(data).metrics["failing_now"] == json::array({"5 Reallocated_Sector_Ct"}));
}

TEST_CASE("raw counts") {
    CHECK(ata_raw_count(188, json{{"value", 4295032833LL}}) == 1);
    CHECK(ata_raw_count(5, json{{"value", 12}}) == 12);
    bool ok = true;
    ata_raw_count(5, json::object(), &ok);
    CHECK_FALSE(ok);
}

TEST_CASE("NVMe healthy") {
    Drive d = parse_smartctl(load("nvme_healthy.json"));
    CHECK(d.kind == "NVMe");
    CHECK(d.metric("wear_percent_used") == std::optional<int64_t>(3));
    CHECK(d.metric("media_errors") == std::optional<int64_t>(0));
    CHECK(d.metric("critical_warning") == std::optional<int64_t>(0));
    CHECK(d.metric("available_spare") == std::optional<int64_t>(100));
    CHECK(d.metric("unsafe_shutdowns") == std::optional<int64_t>(14));
    CHECK(d.capacity_bytes == std::optional<int64_t>(1000204886016LL));
    CHECK(d.self_test["short_minutes"] == 2);
}

TEST_CASE("NVMe worn") {
    Drive d = parse_smartctl(load("nvme_worn.json"));
    CHECK(d.metric("wear_percent_used") == std::optional<int64_t>(62));
    CHECK(d.metric("available_spare") == std::optional<int64_t>(64));
    CHECK(d.metric("warning_temp_minutes") == std::optional<int64_t>(95));
    CHECK(d.metrics["data_written_tb"].get<double>() > 300);
}

TEST_CASE("unsupported device") {
    Drive d = parse_smartctl(json{{"device", {{"name", "/dev/sdx"}, {"protocol", "SCSI"}}}, {"smart_support", {{"available", false}}}});
    CHECK_FALSE(d.smart_available);
}

TEST_CASE("smartctl version parsing") {
    CHECK(parse_version("smartctl 7.4 2023-08-01 r5530 [x86_64-linux]") == std::optional<std::pair<int, int>>({7, 4}));
    CHECK_FALSE(parse_version("garbage").has_value());
}

TEST_CASE("install hints") {
    CHECK(th::has(install_hint(OS::MacOS), "brew install smartmontools"));
    CHECK(th::has(install_hint(OS::Windows), "winget install smartmontools.smartmontools"));
    CHECK(th::has(install_hint(OS::Linux), "apt install smartmontools"));
}

TEST_CASE("drive key prefers serial") {
    CHECK(ends_with(parse_smartctl(load("hdd_healthy.json")).key(), "WD-SYNTH00000001"));
}
}
