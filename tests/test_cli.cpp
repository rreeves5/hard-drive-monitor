// Port of tests/test_cli.py (in-process via run_cli)
#include "helpers.hpp"
#include "alerts.hpp"

using namespace hdmon;
using th::run;

TEST_SUITE("cli") {
TEST_CASE("status text") {
    auto r = run({"status", "--from-json", th::fix()});
    CHECK(r.rc == 0);
    CHECK(th::has(r.out, "CRITICAL"));
    CHECK(th::has(r.out, "120 reallocated sectors"));
    CHECK(th::has(r.out, "5 drive(s)"));
}

TEST_CASE("status json") {
    auto r = run({"status", "--from-json", th::fix(), "--json"});
    json data = json::parse(r.out);
    std::map<std::string, std::string> st;
    for (auto& d : data["drives"]) st[d["drive"]["device"]] = d["assessment"]["status"];
    std::map<std::string, std::string> want = {{"/dev/sda", "OK"}, {"/dev/sdb", "CRITICAL"}, {"/dev/sdc", "OK"},
                                               {"/dev/nvme0", "OK"}, {"/dev/nvme1", "WARNING"}};
    CHECK(st == want);
}

TEST_CASE("detail") {
    auto r = run({"detail", "nvme1", "--from-json", th::fix()});
    CHECK(r.rc == 0);
    CHECK(th::has(r.out, "percentage_used"));
    r = run({"detail", "/dev/sdb", "--from-json", th::fix(), "--json"});
    CHECK(json::parse(r.out)["drive"]["metrics"]["pending_sectors"] == 16);
    r = run({"detail", "nope", "--from-json", th::fix()});
    CHECK(r.rc == 3);
}

TEST_CASE("check -> history flow") {
    th::TempDir tmp;
    std::string db = tmp.file("h.db");
    auto r = run({"check", "--from-json", th::fix(), "--record", "--db", db, "--no-alert", "--quiet"});
    CHECK(r.rc == 2);   // worst = CRITICAL
    r = run({"check", "--from-json", th::fix("later"), "--record", "--db", db, "--no-alert", "--json"});
    CHECK(json::parse(r.out)["alerts"].size() == 1);
    r = run({"history", "--db", db});
    CHECK(r.rc == 0);
    CHECK(th::has(r.out, "Worse since the previous run"));
    r = run({"history", "--db", db, "--json"});
    CHECK(json::parse(r.out)["drives"].size() == 5);
}

TEST_CASE("--from-json does not record by default") {
    th::TempDir tmp;
    run({"status", "--from-json", th::fix(), "--db", tmp.file("h.db")});
    CHECK_FALSE(file_exists(tmp.file("h.db")));
}

TEST_CASE("selftest results from json") {
    auto r = run({"selftest", "sdb", "--results", "--from-json", th::fix()});
    CHECK(r.rc == 0);
    CHECK(th::has(r.out, "read failure"));
    r = run({"selftest", "sdb", "short", "--from-json", th::fix()});
    CHECK(r.rc == 3);
}

TEST_CASE("windows inventory file") {
    auto r = run({"status", "--from-json", th::fix("windows/windows_inventory.json")});
    CHECK(th::has(r.out, "PhysicalDrive1 (D:)"));
    CHECK(th::has(r.out, "PhysicalDrive2 (G:)"));
    r = run({"detail", "G:", "--from-json", th::fix("windows/windows_inventory.json")});
    CHECK(th::has(r.out, "WDC WD20EARX"));
}

TEST_CASE("install-service dry run") {
    auto r = run({"install-service", "--dry-run", "--json"});
    CHECK(r.rc == 0);
    CHECK(json::parse(r.out)["ok"] == true);
}

TEST_CASE("version and usage errors") {
    auto r = run({"--version"});
    CHECK(th::has(r.out, "Hard Drive Monitor"));
    CHECK(run({}).rc == 2);
    CHECK(run({"bogus"}).rc == 2);
    CHECK(run({"status", "--nope"}).rc == 2);
    CHECK(run({"detail"}).rc == 2);
    r = run({"serve"});
    CHECK(r.rc == 2);
    CHECK(th::has(r.err, "not available in the native build"));
    CHECK(run({"status", "-h"}).rc == 0);
}

TEST_CASE("init-config writes once") {
    th::TempDir tmp;
    std::string p = tmp.file("config.toml");
    CHECK(run({"init-config", "--config", p}).rc == 0);
    CHECK(file_exists(p));
    CHECK(run({"init-config", "--config", p}).rc == 1);
    CHECK(run({"init-config", "--config", p, "--force"}).rc == 0);
}
}

TEST_CASE("[alerts] warn_drop_percent: WARNING at health <= 100 - drop, never lowers CRITICAL") {
    th::TempDir tmp;
    std::string cfg = tmp.file("c.toml");
    auto status_of = [&](const char* dev) {
        json j = json::parse(run({"status", "--from-json", th::fix(), "--config", cfg, "--json"}).out);
        for (auto& d : j["drives"]) if (d["drive"]["device"] == dev) return d["assessment"]["status"].get<std::string>();
        return std::string();
    };
    write_file(cfg, "[alerts]\nwarn_drop_percent = 7\n");   // sdc is 94%: warn at <= 93 -> OK
    CHECK(status_of("/dev/sdc") == "OK");
    write_file(cfg, "[alerts]\nwarn_drop_percent = 6\n");   // 94 <= 94 -> WARNING
    CHECK(status_of("/dev/sdc") == "WARNING");
    CHECK(status_of("/dev/sdb") == "CRITICAL");
    write_file(cfg, "[alerts]\nwarn_drop_percent = 99\n");
    CHECK(status_of("/dev/sdb") == "CRITICAL");
}

TEST_CASE("minimal alert line and [alerts] min_level") {
    Drive d;
    d.device = "\\\\.\\PhysicalDrive0";
    d.volumes = {"C:"};
    d.model = "NX-2TB";
    Assessment a{88, 100, "WARNING", {}};
    CHECK(alert_line(d, a) == "C: NX-2TB, Warning, 88%");
    std::vector<Alert> al{{"WARNING", "Hard Drive Monitor", "C: NX-2TB, Warning, 88%"},
                          {"CRITICAL", "Hard Drive Monitor", "D: X, Critical, 20%"}};
    CHECK(filter_alerts(json::object(), al).size() == 2);
    CHECK(filter_alerts(json{{"alerts", {{"min_level", "critical"}}}}, al).size() == 1);
    CHECK(alert_text(al) == "C: NX-2TB, Warning, 88%\nD: X, Critical, 20%");
}
