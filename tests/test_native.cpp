// Native SMART decoders: byte-level fixtures (synthetic and real-kernel QEMU captures) must give the same
// model, attributes, health score and status as the smartctl JSON path.
#include <cstring>
#include <fstream>
#include <iterator>

#include "helpers.hpp"
#include "health.hpp"
#include "native_smart.hpp"
#include "smart.hpp"

using namespace hdmon;
namespace nv = hdmon::native;

namespace {
nv::Bytes bin(const std::string& name) {
    std::ifstream f(th::fix("native/" + name), std::ios::binary);
    REQUIRE_MESSAGE(f.good(), name);
    return nv::Bytes(std::istreambuf_iterator<char>(f), {});
}
// Everything the scorer and the UI read; source/dev_type/notes describe the path, not the drive.
json comparable(const Drive& d) {
    json j = d.to_json();
    for (auto k : {"source", "dev_type", "notes"}) j.erase(k);
    return th::plain(j);
}
std::vector<uint8_t> blank_smart_sector() {
    std::vector<uint8_t> s(512, 0);
    s[0] = 0x10;
    return s;
}
std::string temp_raw(std::initializer_list<int> raw6) {
    auto s = blank_smart_sector();
    uint8_t* e = s.data() + 2;
    e[0] = 194; e[1] = 0x22; e[3] = 100; e[4] = 100;
    int i = 0;
    for (int b : raw6) e[5 + i++] = static_cast<uint8_t>(b);
    json j = nv::ata_smart_json(s.data(), nullptr, true);
    return j["ata_smart_attributes"]["table"][0]["raw"]["string"].get<std::string>();
}
}  // namespace

TEST_SUITE("native smart") {
TEST_CASE("ATA string, checksum and RETURN STATUS decoding") {
    auto id = bin("ata_identify.bin");
    CHECK(nv::ata_string(id.data(), 27, 20) == "ST2000DM008-2FR102");
    CHECK(nv::ata_checksum_ok(id.data()));
    id[100] ^= 0x01;
    CHECK_FALSE(nv::ata_checksum_ok(id.data()));
    std::vector<uint8_t> zero(512, 0);
    CHECK(nv::ata_checksum_ok(zero.data()));   // checksum byte 0 = not used

    CHECK(nv::ata_status_from_lba(0x4F, 0xC2) == std::optional<bool>(true));
    CHECK(nv::ata_status_from_lba(0xF4, 0x2C) == std::optional<bool>(false));
    CHECK_FALSE(nv::ata_status_from_lba(0x00, 0x00).has_value());
}

TEST_CASE("SG_IO sense: descriptor 72h with ATA Return descriptor, and fixed 70h") {
    // ATA Status Return descriptor: +9 = LBA mid (7:0), +11 = LBA high (7:0), +13 = status
    uint8_t desc[32] = {0x72, 0x01, 0x00, 0x1D, 0, 0, 0, 14,
                        0x09, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF4, 0x00, 0x2C, 0x00, 0x50};
    uint8_t mid = 0, high = 0;
    REQUIRE(nv::ata_lba_from_sense(desc, sizeof desc, mid, high));
    CHECK(mid == 0xF4);
    CHECK(high == 0x2C);
    CHECK(nv::ata_status_from_lba(mid, high) == std::optional<bool>(false));

    uint8_t fixed[18] = {0x70, 0, 0x01, 0, 0, 0, 0, 10, 0, 0, 0x4F, 0xC2};
    REQUIRE(nv::ata_lba_from_sense(fixed, sizeof fixed, mid, high));
    CHECK(nv::ata_status_from_lba(mid, high) == std::optional<bool>(true));

    uint8_t other[18] = {0x72, 0, 0, 0, 0, 0, 0, 0};
    CHECK_FALSE(nv::ata_lba_from_sense(other, sizeof other, mid, high));
    CHECK_FALSE(nv::ata_lba_from_sense(other, 4, mid, high));
}

TEST_CASE("temperature raw strings follow smartctl's Min/Max heuristics") {
    CHECK(temp_raw({31, 0, 0, 0, 0, 0}) == "31");
    CHECK(temp_raw({31, 0, 31, 31, 0, 0}) == "31 (Min/Max 31/31)");     // QEMU ide-hd layout
    CHECK(temp_raw({41, 0, 24, 48, 0, 0}) == "41 (Min/Max 24/48)");     // 00 00 HH LL xx TT
    CHECK(temp_raw({41, 18, 50, 0, 0, 0}) == "41 (Min/Max 18/50)");     // WDC 00 00 00 HH LL TT
    CHECK(temp_raw({41, 0, 18, 0, 0, 0}) == "41 (0 0 0 18 0)");         // no plausible range
    CHECK(temp_raw({35, 0, 20, 0, 55, 0}) == "35 (Min/Max 20/55)");     // Hitachi xx HH xx LL xx TT
    CHECK(temp_raw({35, 0, 20, 50, 3, 0}) == "35 (Min/Max 20/50 #3)");  // WDC over-temperature counter
}

TEST_CASE("synthetic ATA sectors decode to the same drive and score as smartctl's JSON") {
    auto id = bin("ata_identify.bin"), data = bin("ata_smart_data.bin"), thr = bin("ata_smart_thresholds.bin");
    REQUIRE(id.size() == 512);
    REQUIRE(data.size() == 512);
    REQUIRE(thr.size() == 512);
    CHECK(nv::ata_checksum_ok(data.data()));
    json ref = th::load("hdd_failing.json");
    json j = nv::assemble("/dev/sdb", "ATA", nv::ata_identify_json(id.data()),
                          nv::ata_smart_json(data.data(), thr.data(), true), "sat");
    Drive n = parse_smartctl(j), s = parse_smartctl(ref);
    CHECK(n.model == s.model);
    CHECK(n.serial == s.serial);
    CHECK(n.firmware == s.firmware);
    CHECK(n.capacity_bytes == s.capacity_bytes);
    CHECK(n.kind == s.kind);
    CHECK(n.rotation_rate == s.rotation_rate);
    CHECK(n.temperature_c == s.temperature_c);
    CHECK(n.power_on_hours == s.power_on_hours);
    CHECK(n.power_cycles == s.power_cycles);
    CHECK(n.smart_passed == s.smart_passed);
    CHECK(th::plain(n.metrics) == th::plain(s.metrics));
    REQUIRE(n.ata_attributes.size() == s.ata_attributes.size());
    for (size_t i = 0; i < n.ata_attributes.size(); ++i) {
        json a = n.ata_attributes[i], b = s.ata_attributes[i];
        INFO(b.dump());
        int idn = b["id"].get<int>();
        for (auto k : {"id", "name", "value", "worst", "thresh", "when_failed", "raw_value", "prefailure"})
            CHECK(th::plain(a[k]) == th::plain(b[k]));
        // The synthetic JSON keeps only the low byte of 190/194 in raw.value (its Min/Max string cannot be rebuilt from
        // it), and 188's "1 1 1" is a per-model word split; the real QEMU captures below check raw strings exactly.
        if (idn != 188 && idn != 190 && idn != 194) CHECK(a["raw"] == b["raw"]);
    }
    Assessment an = assess(n), as = assess(s);
    CHECK(an.health == as.health);
    CHECK(an.performance == as.performance);
    CHECK(an.status == as.status);
    CHECK(an.status == "CRITICAL");
}

TEST_CASE("SMART RETURN STATUS failure and attribute-derived verdicts") {
    auto id = bin("ata_identify.bin"), data = bin("ata_smart_data.bin"), thr = bin("ata_smart_thresholds.bin");
    json failed = nv::assemble("/dev/sdb", "ATA", nv::ata_identify_json(id.data()),
                               nv::ata_smart_json(data.data(), thr.data(), false));
    Drive d = parse_smartctl(failed);
    CHECK(d.smart_passed == std::optional<bool>(false));
    CHECK(assess(d).status == "CRITICAL");

    // No RETURN STATUS (some bridges): verdict from prefail attributes at/below threshold, flagged in notes.
    json derived = nv::ata_smart_json(data.data(), thr.data(), std::nullopt);
    CHECK(derived["smart_status"]["source"] == "attributes");
    nv::ReadResult r;
    r.ok = true;
    r.method = "test";
    r.data = nv::assemble("/dev/sdb", "ATA", nv::ata_identify_json(id.data()), derived);
    Drive dd = nv::to_drive(nv::Device{"/dev/sdb", "/dev/sdb", "ATA"}, r);
    CHECK(dd.source == "native (test)");
    CHECK(th::has(json(dd.notes).dump(), "verdict derived from attribute thresholds"));
}

TEST_CASE("synthetic NVMe identify + log page 02h decode like smartctl's JSON") {
    auto id = bin("nvme_identify_ctrl.bin"), log = bin("nvme_smart_log.bin");
    REQUIRE(id.size() == 4096);
    REQUIRE(log.size() == 512);
    json ref = th::load("nvme_worn.json");
    json j = nv::assemble("/dev/nvme1", "NVMe", nv::nvme_identify_json(id.data()), nv::nvme_health_json(log.data()));
    Drive n = parse_smartctl(j), s = parse_smartctl(ref);
    CHECK(n.model == s.model);
    CHECK(n.serial == s.serial);
    CHECK(n.firmware == s.firmware);
    CHECK(n.kind == "NVMe");
    CHECK(n.temperature_c == s.temperature_c);
    CHECK(n.power_on_hours == s.power_on_hours);
    CHECK(n.power_cycles == s.power_cycles);
    CHECK(n.smart_passed == s.smart_passed);
    CHECK(th::plain(n.metrics) == th::plain(s.metrics));
    Assessment an = assess(n), as = assess(s);
    CHECK(an.health == as.health);
    CHECK(an.performance == as.performance);
    CHECK(an.status == as.status);
}

TEST_CASE("NVMe critical warning and 128-bit counters") {
    std::vector<uint8_t> log(512, 0);
    log[0] = 0x04;                                  // reliability degraded
    log[1] = 0x41; log[2] = 0x01;                   // 321 K = 48 C
    for (int i = 0; i < 16; ++i) log[160 + i] = 0xFF;   // media errors: 2^128-1 clamps to int64 max
    json j = nv::nvme_health_json(log.data());
    CHECK(j["smart_status"]["passed"] == false);
    CHECK(j["temperature"]["current"] == 48);
    CHECK(j["nvme_smart_health_information_log"]["media_errors"].get<int64_t>() == std::numeric_limits<int64_t>::max());
}

TEST_CASE("real-kernel captures (QEMU AHCI disks via SG_IO) match smartctl on the same devices") {
    struct Case { const char *prefix, *device, *model, *serial, *kind; };
    for (Case c : {Case{"hdd", "/dev/sdc", "QEMU-HDD-A", "QMHDD0001", "HDD"},
                   Case{"ssd", "/dev/sda", "QEMU-SSD-B", "QMSSD0002", "SSD"}}) {
        INFO(c.prefix);
        std::string p = std::string("qemu/") + c.prefix;
        auto id = bin(p + "-identify.bin"), data = bin(p + "-smart-data.bin"), thr = bin(p + "-smart-thresholds.bin");
        json sc = th::load(std::string("native/qemu/smartctl-") + c.prefix + ".json");
        json j = nv::assemble(c.device, "ATA", nv::ata_identify_json(id.data()),
                              nv::ata_smart_json(data.data(), thr.data(), true), "sat");
        Drive n = parse_smartctl(j), s = parse_smartctl(sc);
        CHECK(*n.model == c.model);
        CHECK(*n.serial == c.serial);
        CHECK(n.kind == c.kind);
        CHECK(comparable(n) == comparable(s));
        Assessment an = assess(n), as = assess(s);
        CHECK(th::plain(an.to_json()) == th::plain(as.to_json()));
    }
}

TEST_CASE("real-kernel capture (QEMU NVMe via NVME_IOCTL_ADMIN_CMD) matches smartctl on the same device") {
    auto id = bin("qemu/nvme-identify.bin"), log = bin("qemu/nvme-log02.bin");
    json sc = th::load("native/qemu/smartctl-nvme.json");
    json j = nv::assemble("/dev/nvme0", "NVMe", nv::nvme_identify_json(id.data()), nv::nvme_health_json(log.data()));
    Drive n = parse_smartctl(j), s = parse_smartctl(sc);
    CHECK(*n.serial == "QMNVME0005");
    json a = comparable(n), b = comparable(s);
    // The native read also decodes WCTEMP/CCTEMP and TNVMCAP from Identify; smartctl 7.4 leaves them out of
    // its JSON for this controller. Everything else must match.
    CHECK(a["temp_limit_warn"] == 70);
    CHECK(a["temp_limit_crit"] == 100);
    for (auto k : {"temp_limit_warn", "temp_limit_crit", "capacity_bytes"}) { a.erase(k); b.erase(k); }
    CHECK(a == b);
    CHECK(assess(n).status == assess(s).status);
    CHECK(assess(n).health == assess(s).health);
}
}
