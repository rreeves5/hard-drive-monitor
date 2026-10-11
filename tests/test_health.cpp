// Port of tests/test_health.py
#include <algorithm>

#include "helpers.hpp"
#include "health.hpp"

using namespace hdmon;
using th::load;

namespace {
std::pair<Drive, Assessment> A(const std::string& name, const json& patch = json::object()) {
    Drive d = parse_smartctl(load(name));
    for (auto it = patch.begin(); it != patch.end(); ++it) d.metrics[it.key()] = it.value();
    return {d, assess(d)};
}
bool any_reason(const Assessment& a, const std::string& s) {
    auto r = a.reasons();
    return std::any_of(r.begin(), r.end(), [&](const std::string& x) { return th::has(x, s); });
}
}  // namespace

TEST_SUITE("health") {
TEST_CASE("healthy HDD") {
    auto [d, a] = A("hdd_healthy.json");
    CHECK(a.health == std::optional<int>(100));
    CHECK(a.performance == std::optional<int>(100));
    CHECK(a.status == "OK");
    CHECK(a.reasons().empty());
}

TEST_CASE("failing HDD is critical with reasons") {
    auto [d, a] = A("hdd_failing.json");
    CHECK(a.health == std::optional<int>(10));   // 100*.99^120*.97^16*.97^16*.98^3*.995 = 10.58
    CHECK(a.status == "CRITICAL");
    std::string text = join(a.reasons(), " ");
    CHECK(th::has(text, "120 reallocated sectors"));
    CHECK(th::has(text, "remapping bad spots"));
    CHECK(th::has(text, "16 pending sectors"));
    CHECK(a.performance == std::optional<int>(62));
}

TEST_CASE("few reallocated is warning") {
    auto [d, a] = A("hdd_healthy.json", {{"reallocated_sectors", 12}});
    CHECK(a.health == std::optional<int>(88));
    CHECK(a.status == "WARNING");
    auto r = a.reasons();
    CHECK(std::find(r.begin(), r.end(), "12 reallocated sectors - the drive is remapping bad spots to spare area") != r.end());
}

TEST_CASE("CRC is cable warning without health loss") {
    auto [d, a] = A("hdd_healthy.json", {{"udma_crc_errors", 5}});
    CHECK(a.health == std::optional<int>(100));
    CHECK(a.performance == std::optional<int>(95));
    CHECK(a.status == "WARNING");
    CHECK(any_reason(a, "cable"));
}

TEST_CASE("SMART failed caps health") {
    Drive d = parse_smartctl(load("hdd_healthy.json"));
    d.smart_passed = false;
    Assessment a = assess(d);
    CHECK(*a.health <= 5);
    CHECK(a.status == "CRITICAL");
}

TEST_CASE("attribute failing now") {
    auto [d, a] = A("hdd_healthy.json", {{"failing_now", {"5 Reallocated_Sector_Ct"}}});
    CHECK(*a.health <= 10);
    CHECK(a.status == "CRITICAL");
}

TEST_CASE("healthy NVMe") {
    auto [d, a] = A("nvme_healthy.json");
    CHECK(a.health == std::optional<int>(97));
    CHECK(a.status == "OK");
}

TEST_CASE("worn NVMe") {
    auto [d, a] = A("nvme_worn.json");
    CHECK(a.health == std::optional<int>(38));
    CHECK(a.status == "WARNING");
    REQUIRE_FALSE(a.reasons().empty());
    CHECK(th::has(a.reasons()[0], "62%"));
    CHECK(*a.performance < 100);
}

TEST_CASE("NVMe critical warning bits") {
    auto [d, a] = A("nvme_healthy.json", {{"critical_warning", 0x08}});
    CHECK(a.status == "CRITICAL");
    CHECK(*a.health <= 20);
    CHECK(any_reason(a, "READ-ONLY"));
    auto [d2, a2] = A("nvme_healthy.json", {{"critical_warning", 0x02}});
    CHECK(a2.status == "WARNING");
}

TEST_CASE("NVMe spare below threshold") {
    auto [d, a] = A("nvme_healthy.json", {{"available_spare", 5}});
    CHECK(a.status == "CRITICAL");
    CHECK(*a.health <= 10);
}

TEST_CASE("NVMe media errors") {
    CHECK(A("nvme_healthy.json", {{"media_errors", 12}}).second.status == "CRITICAL");
    CHECK(A("nvme_healthy.json", {{"media_errors", 1}}).second.status == "WARNING");
}

TEST_CASE("wear 100 is critical") {
    auto [d, a] = A("ssd_sata_healthy.json", {{"wear_percent_used", 100}});
    CHECK(a.health == std::optional<int>(0));
    CHECK(a.status == "CRITICAL");
}

TEST_CASE("temperature") {
    Drive d = parse_smartctl(load("hdd_healthy.json"));
    d.temperature_c = 52;
    CHECK(assess(d).status == "WARNING");
    d.temperature_c = 58;
    Assessment a = assess(d);
    CHECK(a.status == "CRITICAL");
    CHECK(a.health == std::optional<int>(100));   // temperature never lowers Health
    d.temperature_c = 52;
    CHECK(assess(d, json{{"hdd_temp_warn", 55}, {"hdd_temp_crit", 60}}).status == "OK");
}

TEST_CASE("unknown when no SMART") {
    Drive d;
    d.device = "/dev/vda";
    d.smart_available = false;
    d.notes = {"virtio disk"};
    Assessment a = assess(d);
    CHECK(a.status == "UNKNOWN");
    CHECK_FALSE(a.health.has_value());
}
}
