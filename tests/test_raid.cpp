// RAID: parsers for every backend (fixtures), md sysfs, hardware grouping, health rollup, history + alerts, CLI.
#include <fstream>

#include "helpers.hpp"
#include "alerts.hpp"
#include "collect.hpp"
#include "history.hpp"
#include "raid.hpp"
#include "smart.hpp"
#include "wmi.hpp"

using namespace hdmon;

namespace {
std::string rtext(const std::string& n) { return read_file(th::fix("raid/" + n)); }
const RaidArray& by_name(const std::vector<RaidArray>& v, const std::string& n) {
    for (auto& a : v) if (a.name == n) return a;
    FAIL("no array " << n);
    return v.front();
}
Result drive_result(const std::string& dev, const std::string& serial, const std::string& status, int health = 100) {
    Drive d;
    d.device = dev;
    d.serial = serial;
    d.model = "TEST " + serial;
    d.smart_available = true;
    Assessment a;
    a.status = status;
    a.health = health;
    return {d, a};
}
void write(const std::filesystem::path& p, const std::string& s) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p) << s << "\n";
}
}  // namespace

TEST_SUITE("raid") {
TEST_CASE("md member names map to whole disks") {
    CHECK(md_member_disk("sdb1") == "sdb");
    CHECK(md_member_disk("/dev/sdb1") == "sdb");
    CHECK(md_member_disk("nvme0n1p3") == "nvme0");
    CHECK(md_member_disk("sdh") == "sdh");
    CHECK(md_member_disk("disk2s2") == "disk2");
}

TEST_CASE("mdstat clean: both arrays OK") {
    auto v = parse_mdstat(rtext("mdstat-clean.txt"));
    REQUIRE(v.size() == 2);
    for (auto& a : v) {
        INFO(a.name);
        CHECK(a.state == "OK");
        CHECK(a.backend == "mdadm");
        CHECK(state_status(a.state) == "OK");
    }
    auto& md1 = by_name(v, "md1");
    CHECK(md1.level == "raid5");
    CHECK(md1.members.size() == 3);
    CHECK(md1.size_bytes == std::optional<int64_t>(3906764800LL * 1024));
}

TEST_CASE("mdstat degraded: raid1 missing one is DEGRADED, raid5 missing two is FAILED, inactive is FAILED") {
    auto v = parse_mdstat(rtext("mdstat-degraded.txt"));
    REQUIRE(v.size() == 3);
    auto& md0 = by_name(v, "md0");
    CHECK(md0.state == "DEGRADED");
    CHECK(state_status(md0.state) == "WARNING");
    REQUIRE(md0.members.size() == 2);
    int failed = 0;
    for (auto& m : md0.members) failed += m.role == "failed";
    CHECK(failed == 1);
    CHECK(by_name(v, "md1").state == "FAILED");
    CHECK(state_status("FAILED") == "CRITICAL");
    CHECK(by_name(v, "md127").state == "FAILED");
}

TEST_CASE("mdstat recovering: REBUILDING with percent; a scrub (check) is not degraded") {
    auto v = parse_mdstat(rtext("mdstat-recovering.txt"));
    auto& md0 = by_name(v, "md0");
    CHECK(md0.state == "REBUILDING");
    REQUIRE(md0.rebuild_pct);
    CHECK(*md0.rebuild_pct == doctest::Approx(37.5));
    CHECK(state_text(md0) == "Rebuilding 37.5%");
    CHECK(state_status(md0.state) == "WARNING");
    auto& md2 = by_name(v, "md2");
    CHECK(md2.state == "OK");
    CHECK(th::has(json(md2.notes).dump(), "check"));
}

TEST_CASE("mdadm --detail parses and merges into the mdstat view") {
    RaidArray d = parse_mdadm_detail(rtext("mdadm-detail-md0.txt"));
    CHECK(d.name == "md0");
    CHECK(d.level == "raid1");
    CHECK(d.state == "REBUILDING");
    CHECK(th::has(d.id, "3b2f6c1e"));
    REQUIRE(d.members.size() == 2);
    CHECK(d.members[1].device == "/dev/sdd1");
    CHECK(d.members[1].role == "rebuilding");
    auto v = parse_mdstat(rtext("mdstat-recovering.txt"));
    RaidArray md0 = by_name(v, "md0");
    merge_md_detail(md0, d);
    CHECK(th::has(md0.id, "3b2f6c1e"));
    CHECK(md0.state == "REBUILDING");
}

TEST_CASE("md from a fake sysfs tree: degraded with a faulty member, then recovering") {
    th::TempDir t;
    auto sys = t.path / "sys";
    auto md = sys / "block" / "md0" / "md";
    write(sys / "block" / "md0" / "size", "4188160");
    write(md / "level", "raid1");
    write(md / "uuid", "8c0bf89c:fa9c582b:8d17bf9a:f12ab206");
    write(md / "array_state", "clean");
    write(md / "raid_disks", "2");
    write(md / "degraded", "1");
    write(md / "sync_action", "idle");
    write(md / "sync_completed", "none");
    write(md / "dev-sdc" / "state", "in_sync");
    write(md / "dev-sdc" / "slot", "0");
    write(md / "dev-sdd" / "state", "faulty");
    write(md / "dev-sdd" / "slot", "none");
    std::string nomdstat = t.file("no-mdstat");
    auto v = detect_md_sysfs(sys.string(), nomdstat);
    REQUIRE(v.size() == 1);
    CHECK(v[0].name == "md0");
    CHECK(v[0].state == "DEGRADED");
    CHECK(v[0].source == "sysfs");
    CHECK(v[0].size_bytes == std::optional<int64_t>(4188160LL * 512));
    CHECK(th::has(v[0].id, "8c0bf89c"));
    REQUIRE(v[0].members.size() == 2);
    CHECK(v[0].members[0].device == "/dev/sdc");
    CHECK(v[0].members[0].role == "active");
    CHECK(v[0].members[1].role == "failed");

    write(md / "dev-sdd" / "state", "spare");
    write(md / "dev-sdd" / "slot", "1");
    write(md / "sync_action", "recover");
    write(md / "sync_completed", "524288 / 4188160");
    v = detect_md_sysfs(sys.string(), nomdstat);
    CHECK(v[0].state == "REBUILDING");
    REQUIRE(v[0].rebuild_pct);
    CHECK(*v[0].rebuild_pct == doctest::Approx(12.5));
    CHECK(v[0].members[1].role == "rebuilding");

    write(md / "sync_action", "idle");
    write(md / "degraded", "0");
    write(md / "dev-sdd" / "state", "in_sync");
    v = detect_md_sysfs(sys.string(), nomdstat);
    CHECK(v[0].state == "OK");
}

TEST_CASE("Storage Spaces: healthy, degraded, repairing") {
    auto healthy = parse_storage_spaces(th::load("raid/storage-spaces-healthy.json"));
    REQUIRE(healthy.size() == 1);
    CHECK(healthy[0].state == "OK");
    CHECK(healthy[0].backend == "storage-spaces");
    CHECK_FALSE(healthy[0].members.empty());
    auto degraded = parse_storage_spaces(th::load("raid/storage-spaces-degraded.json"));
    REQUIRE(degraded.size() == 1);
    CHECK(degraded[0].state == "DEGRADED");
    auto rep = parse_storage_spaces(th::load("raid/storage-spaces-repairing.json"));
    REQUIRE(rep.size() == 1);
    CHECK(rep[0].state == "REBUILDING");
    REQUIRE(rep[0].rebuild_pct);
    CHECK(*rep[0].rebuild_pct == doctest::Approx(42));
}

TEST_CASE("MSFT_* enum values map to Storage cmdlet strings") {
    CHECK(msft_health(0) == "Healthy");
    CHECK(msft_health(1) == "Warning");
    CHECK(msft_health(2) == "Unhealthy");
    CHECK(msft_operational(2) == "OK");
    CHECK(msft_operational(3) == "Degraded");
    CHECK(msft_media(4) == "SSD");
    CHECK(msft_media(3) == "HDD");
    CHECK(msft_bus(17) == "NVMe");
    CHECK(msft_job_state(4) == "Running");
}

TEST_CASE("macOS AppleRAID: degraded mirror rebuilding 45%, online stripe") {
    auto v = parse_appleraid(rtext("diskutil-appleraid.txt"));
    REQUIRE(v.size() == 2);
    auto& b = by_name(v, "Backup");
    CHECK(b.backend == "appleraid");
    CHECK(b.state == "REBUILDING");
    REQUIRE(b.rebuild_pct);
    CHECK(*b.rebuild_pct == doctest::Approx(45));
    REQUIRE(b.members.size() == 2);
    CHECK(b.members[1].role == "rebuilding");
    auto& s = by_name(v, "Scratch");
    CHECK(s.state == "OK");
    CHECK(s.members.size() == 2);
}

TEST_CASE("macOS AppleRAID from the IOKit registry: rebuilding mirror, missing slot, spare, online stripe") {
    auto v = parse_appleraid_ioreg(th::load("raid/ioreg-appleraid.json"));
    REQUIRE(v.size() == 2);
    auto& b = by_name(v, "Backup");
    CHECK(b.backend == "appleraid");
    CHECK(b.id == "appleraid:8E3A8C3D-5B1F-4C7A-9F2E-1D6B0A4C7E21");
    CHECK(b.level == "Mirror");
    CHECK(b.state == "REBUILDING");
    CHECK(b.source == "IOKit AppleRAID");
    REQUIRE(b.rebuild_pct);
    CHECK(*b.rebuild_pct == doctest::Approx(45.0).epsilon(0.001));
    CHECK(b.volumes == std::vector<std::string>{"/dev/disk6"});
    REQUIRE(b.members.size() == 4);
    CHECK(b.members[0].device == "/dev/disk4s2");
    CHECK(b.members[0].role == "active");
    CHECK(b.members[1].role == "rebuilding");
    CHECK(b.members[2].role == "missing");
    CHECK(b.members[2].device.empty());
    CHECK(b.members[3].role == "spare");
    CHECK(b.members[3].device == "/dev/disk10s2");
    CHECK(member_status(b.members[2], nullptr) == "CRITICAL");
    CHECK(member_status(b.members[3], nullptr) == "OK");
    auto& s = by_name(v, "Scratch");
    CHECK(s.state == "OK");
    CHECK(s.members.size() == 2);
    CHECK(s.size_bytes == std::optional<int64_t>(1000070512640));
    // same arrays as the diskutil text fallback, by id
    auto d = parse_appleraid(rtext("diskutil-appleraid.txt"));
    CHECK(by_name(d, "Backup").id == b.id);
    CHECK(parse_appleraid_ioreg(json::object()).empty());
}

TEST_CASE("Intel RST CLI: degraded volume with a failed member") {
    auto v = parse_rstcli(rtext("rstcli-volume.txt"));
    REQUIRE(v.size() == 1);
    CHECK(v[0].name == "Volume0");
    CHECK(v[0].backend == "intel-rst");
    CHECK(v[0].state == "DEGRADED");
    REQUIRE(v[0].members.size() == 2);
    CHECK(v[0].members[1].role == "failed");
    CHECK(v[0].members[1].serial == std::optional<std::string>("WD-WCC6Y0SYNTH2"));
}

TEST_CASE("smartctl --scan-open finds RAID controller members") {
    CHECK(is_raid_dev_type("megaraid,0"));
    CHECK(is_raid_dev_type("sat+megaraid,2"));
    CHECK(is_raid_dev_type("aacraid,0,0,1"));
    CHECK(is_raid_dev_type("areca,3"));
    CHECK(is_raid_dev_type("3ware,1"));
    CHECK(is_raid_dev_type("cciss,0"));
    CHECK(is_raid_dev_type("hpt,1/1/1"));
    CHECK_FALSE(is_raid_dev_type("sat"));
    CHECK_FALSE(is_raid_dev_type("nvme"));
    CHECK_FALSE(is_raid_dev_type("megaraid"));
    auto refs = parse_scan(th::load("raid/smartctl-scan-megaraid.json"));
    int members = 0;
    for (auto& r : refs) members += r.dev_type && is_raid_dev_type(*r.dev_type);
    CHECK(members == 4);
}

TEST_CASE("rollup: worst of array state and member health") {
    RaidArray a;
    a.name = "md0";
    a.backend = "mdadm";
    a.state = "DEGRADED";
    ArrayMember m1, m2;
    m1.device = "/dev/sdb1";
    m2.device = "/dev/sdd1";
    m2.role = "failed";
    a.members = {m1, m2};
    std::vector<Result> rs = {drive_result("/dev/sdb", "S1", "CRITICAL", 20), drive_result("/dev/sdd", "S2", "OK")};
    std::vector<RaidArray> v = {a};
    link_members(v, rs);
    CHECK(v[0].members[0].drive_key == rs[0].first.key());
    CHECK(array_status(v[0], rs) == "CRITICAL");           // degraded + critical member
    rs[0].second.status = "OK";
    CHECK(array_status(v[0], rs) == "WARNING");            // degraded alone
    v[0].state = "REBUILDING";
    CHECK(array_status(v[0], rs) == "WARNING");
    v[0].state = "FAILED";
    CHECK(array_status(v[0], rs) == "CRITICAL");
    v[0].state = "OK";
    CHECK(array_status(v[0], rs) == "OK");
    rs[1].second.status = "WARNING";
    CHECK(array_status(v[0], rs) == "WARNING");            // member warning shows on the array
    v[0].state = "UNKNOWN";
    rs[1].second.status = "OK";
    CHECK(array_status(v[0], rs) == "OK");                 // hardware arrays: state unknown -> worst member
    auto keys = member_keys(v);
    CHECK(keys.count(rs[0].first.key()) == 1);
}

TEST_CASE("hardware arrays group controller members; labels are megaraid,N") {
    auto results = score(load_json_files({th::fix("raid/sample")}), json::object());
    auto hw = hardware_arrays(results);
    REQUIRE(hw.size() == 1);
    CHECK(hw[0].backend == "megaraid");
    CHECK(hw[0].state == "UNKNOWN");
    CHECK(hw[0].members.size() == 2);
    for (auto& r : results)
        if (r.first.dev_type && is_raid_dev_type(*r.first.dev_type)) CHECK(th::has(disk_label(r.first), "megaraid,"));
}

TEST_CASE("array state transitions") {
    auto ev = [](const json& prev, const std::string& s) {
        json c = compare_array_states(prev, s);
        return c.empty() ? std::string("-") : c[0]["event"].get<std::string>() + (c[0]["alert"].get<bool>() ? "!" : "");
    };
    json none = json::object();
    auto st = [](const char* s) { return json{{"state", s}}; };
    CHECK(ev(none, "OK") == "-");
    CHECK(ev(none, "DEGRADED") == "degraded!");
    CHECK(ev(none, "FAILED") == "failed!");
    CHECK(ev(none, "REBUILDING") == "rebuilding!");
    CHECK(ev(st("OK"), "OK") == "-");
    CHECK(ev(st("OK"), "DEGRADED") == "degraded!");
    CHECK(ev(st("DEGRADED"), "REBUILDING") == "rebuilding");
    CHECK(ev(st("OK"), "REBUILDING") == "rebuild_started!");
    CHECK(ev(st("REBUILDING"), "OK") == "rebuild_complete!");
    CHECK(ev(st("DEGRADED"), "OK") == "rebuild_complete!");
    CHECK(ev(st("DEGRADED"), "FAILED") == "failed!");
    CHECK(ev(st("FAILED"), "DEGRADED") == "degraded");
}

TEST_CASE("history records array state and alerts on degrade and rebuild complete") {
    th::TempDir t;
    History h(t.file("h.db"));
    RaidArray a;
    a.id = "mdadm:test";
    a.name = "md0";
    a.backend = "mdadm";
    a.level = "raid1";
    a.state = "OK";
    std::vector<Result> rs;
    Changes ch;
    std::vector<RaidArray> arrs = {a};
    h.record(rs, "test", "2026-10-11T11:00:00Z", &arrs, &ch);
    CHECK(ch.count(a.id) == 0);
    arrs[0].state = "DEGRADED";
    ch.clear();
    h.record(rs, "test", "2026-10-11T12:00:00Z", &arrs, &ch);
    REQUIRE(ch.count(a.id) == 1);
    auto al = build_array_alerts(arrs, rs, ch);
    REQUIRE(al.size() == 1);
    CHECK(al[0].title == "Hard Drive Monitor");
    CHECK(th::has(al[0].body, "md0"));
    CHECK(th::has(al[0].body, "Degraded"));
    CHECK(al[0].severity == "WARNING");
    CHECK(h.last_array_reading(a.id)["state"] == "DEGRADED");

    arrs[0].state = "REBUILDING";
    arrs[0].rebuild_pct = 50.0;
    ch.clear();
    h.record(rs, "test", "2026-10-11T13:00:00Z", &arrs, &ch);
    CHECK(build_array_alerts(arrs, rs, ch).empty());       // degraded -> rebuilding: no new alert

    arrs[0].state = "OK";
    arrs[0].rebuild_pct.reset();
    ch.clear();
    h.record(rs, "test", "2026-10-11T14:00:00Z", &arrs, &ch);
    al = build_array_alerts(arrs, rs, ch);
    REQUIRE(al.size() == 1);
    CHECK(th::has(al[0].body, "Rebuild complete"));
    CHECK(al[0].severity == "OK");
    CHECK(h.array_readings(a.id).size() == 4);
}

TEST_CASE("CLI: status, raid and JSON on the RAID sample") {
    std::string sample = th::fix("raid/sample");
    auto r = th::run({"status", "--from-json", sample, "--color", "never"});
    INFO(r.out);
    CHECK(th::has(r.out, "md0"));
    CHECK(th::has(r.out, "  ├ sdb"));
    CHECK(th::has(r.out, "(failed)"));
    CHECK(th::has(r.out, "megaraid,0"));
    // raw disks under an array are not listed again at top level
    size_t count = 0;
    for (size_t p = r.out.find("/dev/sdb "); p != std::string::npos; p = r.out.find("/dev/sdb ", p + 1)) ++count;
    CHECK(count == 0);

    auto l = th::run({"list", "--from-json", sample, "--color", "never"});
    CHECK(l.out == r.out);

    auto rr = th::run({"raid", "--from-json", sample, "--color", "never"});
    CHECK(rr.rc == 2);
    CHECK(th::has(rr.out, "Degraded"));
    CHECK(th::has(rr.out, "Linux md"));

    auto j = th::run({"status", "--json", "--from-json", sample});
    json js = json::parse(j.out);
    REQUIRE(js["arrays"].is_array());
    bool found = false;
    for (auto& a : js["arrays"])
        if (a["name"] == "md0") {
            found = true;
            CHECK(a["state"] == "DEGRADED");
            CHECK(a["status"] == "CRITICAL");
        }
    CHECK(found);

    auto none = th::run({"raid", "--from-json", th::fix("hdd_healthy.json"), "--color", "never"});
    CHECK(none.rc == 0);
    CHECK(th::has(none.out, "No RAID arrays"));
}

TEST_CASE("member status: role wins over SMART when it is worse") {
    Result ok = drive_result("/dev/sdb", "S1", "OK"), crit = drive_result("/dev/sdc", "S2", "CRITICAL", 10),
           warn = drive_result("/dev/sdd", "S3", "WARNING", 70);
    ArrayMember m;
    m.role = "failed";
    CHECK(member_status(m, &ok) == "CRITICAL");
    CHECK(member_status(m, nullptr) == "CRITICAL");
    CHECK(member_role_text(m) == "Failed (removed from array)");
    m.role = "missing";
    CHECK(member_status(m, nullptr) == "CRITICAL");
    CHECK(member_role_text(m) == "Missing from array");
    m.role = "rebuilding";
    CHECK(member_status(m, &ok) == "WARNING");
    CHECK(member_status(m, &crit) == "CRITICAL");
    CHECK(member_role_text(m) == "Rebuilding");
    m.role = "spare";
    CHECK(member_status(m, &ok) == "OK");
    CHECK(member_status(m, nullptr) == "OK");
    CHECK(member_status(m, &warn) == "WARNING");
    CHECK(member_role_text(m) == "Spare");
    m.role = "active";
    CHECK(member_status(m, &warn) == "WARNING");
    CHECK(member_status(m, nullptr) == "UNKNOWN");
    CHECK(member_role_text(m).empty());
}

TEST_CASE("state label: unreadable controller state is rolled up from the disks and said honestly") {
    std::vector<Result> rs = {drive_result("/dev/sdb", "S1", "OK"), drive_result("/dev/sdc", "S2", "WARNING", 80)};
    RaidArray a;
    a.name = "MegaRAID bus/0";
    a.backend = "megaraid";
    a.state = "UNKNOWN";
    ArrayMember m1, m2;
    m1.drive_key = rs[0].first.key();
    m2.drive_key = rs[1].first.key();
    a.members = {m1};
    CHECK(state_label(a, rs) == "Worst disk OK (controller state not readable)");
    CHECK(state_label(a, rs, true) == "Worst disk OK");
    a.members = {m1, m2};
    CHECK(state_label(a, rs) == "Worst disk Warning (controller state not readable)");
    CHECK(array_status(a, rs) == "WARNING");
    a.members.clear();
    CHECK(state_label(a, rs) == "State unknown (controller state not readable)");
    a.state = "DEGRADED";
    CHECK(state_label(a, rs) == "Degraded");
}

TEST_CASE("CLI: failed member shows CRITICAL with its role, controller label is honest") {
    auto r = th::run({"raid", "--from-json", th::fix("raid/sample"), "--color", "never"});
    CHECK(th::has(r.out, "Failed (removed from array)  CRITICAL"));
    CHECK(th::has(r.out, "Worst disk OK (controller state not readable)"));
    auto s = th::run({"status", "--from-json", th::fix("raid/sample"), "--color", "never"});
    CHECK(th::has(s.out, "member /dev/sdd1: Failed (removed from array)"));
    bool failed_row_critical = false;
    for (auto& line : split_lines(s.out))
        if (th::has(line, "sdd (failed)")) failed_row_critical = th::has(line, "CRITICAL");
    CHECK(failed_row_critical);
}
}
