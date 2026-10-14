// Port of tests/test_history_config_service.py (+ e-mail/curl config and UTF-16 task XML checks)
#include <algorithm>

#include "helpers.hpp"
#include "alerts.hpp"
#include "collect.hpp"
#include "config.hpp"
#include "service.hpp"

using namespace hdmon;

TEST_SUITE("history") {
TEST_CASE("records and flags increase") {
    th::TempDir tmp;
    auto first = score(load_json_files({th::fix()}), default_config());
    auto later = score(load_json_files({th::fix("later/hdd_failing.json")}), default_config());
    History h(tmp.file("h.db"));
    auto c1 = h.record(first, "test", "2026-10-10T12:00:00+00:00");
    CHECK(c1.size() == 5);
    for (auto& [k, v] : c1) CHECK(v.empty());
    auto c2 = h.record(first, "test", "2026-10-10T13:00:00+00:00");
    for (auto& [k, v] : c2) CHECK(v.empty());
    auto c3 = h.record(later, "test", "2026-10-10T14:00:00+00:00");
    std::string key = later[0].first.key();
    std::map<std::string, std::pair<int64_t, int64_t>> got;
    for (auto& c : c3[key]) {
        auto o = opt_int(c["old"]), n = opt_int(c["new"]);
        if (o && n) got[c["metric"].get<std::string>()] = {*o, *n};
    }
    CHECK(got["reallocated_sectors"] == std::make_pair<int64_t, int64_t>(120, 152));
    CHECK(got["pending_sectors"] == std::make_pair<int64_t, int64_t>(16, 21));
    auto alerts = build_alerts(later, c3);
    REQUIRE(alerts.size() == 1);
    CHECK_FALSE(th::has(alerts[0].body, "\n"));                 // minimal: one line per drive
    CHECK(th::has(alerts[0].title, "Hard Drive Monitor"));
    json summ = h.summary();
    json sdb;
    for (auto& s : summ) if (s["drive_key"] == key) sdb = s;
    REQUIRE(sdb.is_object());
    CHECK(sdb["n"] == 3);
    CHECK_FALSE(sdb["changes_since_last"].empty());
    CHECK(h.drives().size() == 5);
}

TEST_CASE("no alert without worsening") {
    CHECK(compare_readings(json{{"status", "WARNING"}, {"health", 50}, {"metrics", {{"reallocated_sectors", 5}}}},
                           "WARNING", 50, json{{"reallocated_sectors", 5}}).empty());
    json ch = compare_readings(json{{"status", "OK"}, {"health", 90}, {"metrics", json::object()}}, "WARNING", 80,
                               json{{"wear_percent_used", 5}});
    std::vector<std::string> alerting;
    for (auto& c : ch) if (c["alert"].get<bool>()) alerting.push_back(c["metric"].get<std::string>());
    CHECK(alerting == std::vector<std::string>{"status"});
    ch = compare_readings(json{{"status", "OK"}, {"health", 90}, {"metrics", {{"wear_percent_used", 4}}}}, "OK", 90,
                          json{{"wear_percent_used", 5}});
    for (auto& c : ch) CHECK_FALSE(c["alert"].get<bool>());   // wear is a trend, not an alert
}
}

TEST_SUITE("config") {
TEST_CASE("simple TOML parser reads the sample") {
    json c = parse_simple_toml(SAMPLE_CONFIG);
    CHECK(c["email"]["port"] == 587);
    CHECK(c["email"]["to"] == json::array({"you@example.com"}));
    CHECK(c["alerts"]["desktop"] == true);
    CHECK(c["smartctl"]["sudo"] == "auto");
    CHECK(c["ignore"]["devices"] == json::array());
}

TEST_CASE("load merges defaults") {
    th::TempDir tmp;
    std::string p = tmp.file("c.toml");
    write_file(p, "[thresholds]\nhdd_temp_warn = 45 # comment\n[email]\nto = [\"a@b.c\", \"d@e.f\"]\n");
    json c = load_config(p);
    CHECK(c["thresholds"]["hdd_temp_warn"] == 45);
    CHECK(c["thresholds"]["hdd_temp_crit"] == 55);
    CHECK(c["email"]["to"] == json::array({"a@b.c", "d@e.f"}));
}

TEST_CASE("e-mail via curl: message and config") {
    json cfg = default_config();
    cfg["email"] = {{"enabled", true}, {"host", "smtp.example.com"}, {"port", 587}, {"from", "hdmon@example.com"},
                    {"to", {"a@b.c", "d@e.f"}}, {"username", "user"}};
    std::string msg = build_email_message(cfg, "Subj", "line1\nline2", "host1");
    CHECK(th::has(msg, "To: a@b.c, d@e.f\r\n"));
    CHECK(th::has(msg, "Subject: Subj\r\n"));
    CHECK(th::has(msg, "line1\r\nline2\r\n\r\n-- Hard Drive Monitor on host1"));
    std::string conf = build_curl_config(cfg, "/tmp/m.eml", "p\"w");
    CHECK(th::has(conf, "url = \"smtp://smtp.example.com:587\"\n"));
    CHECK(th::has(conf, "ssl-reqd\n"));
    CHECK(th::has(conf, "mail-rcpt = \"d@e.f\"\n"));
    CHECK(th::has(conf, "user = \"user:p\\\"w\"\n"));
    cfg["email"]["ssl"] = true;
    cfg["email"].erase("port");
    conf = build_curl_config(cfg, "/tmp/m.eml", "");
    CHECK(th::has(conf, "url = \"smtps://smtp.example.com:465\"\n"));
    CHECK_FALSE(th::has(conf, "ssl-reqd"));
}
}

TEST_SUITE("service") {
TEST_CASE("interval parsing") {
    CHECK(parse_interval("1h") == 3600);
    CHECK(parse_interval("2h30m") == 9000);
    CHECK(parse_interval("90s") == 90);
    CHECK(parse_interval("600") == 600);
    CHECK_THROWS_AS(parse_interval("soon"), std::invalid_argument);
}

TEST_CASE("systemd units") {
    auto u = systemd_units({"/usr/bin/hdmon"}, 3600);
    CHECK(th::has(u["hard-drive-monitor.service"], "ExecStart=/usr/bin/hdmon check --quiet"));
    CHECK(th::has(u["hard-drive-monitor.timer"], "OnUnitActiveSec=3600s"));
    CHECK(th::has(u["hard-drive-monitor.timer"], "OnStartupSec=2min"));
    CHECK(th::has(u["hard-drive-monitor.timer"], "WantedBy=timers.target"));
}

TEST_CASE("launchd plist") {
    std::string p = launchd_plist({"/opt/homebrew/bin/hdmon"}, 3600, "/tmp");
    CHECK(th::has(p, "<key>Label</key>\n\t<string>com.hard-drive-monitor.check</string>"));
    CHECK(th::has(p, "<key>StartInterval</key>\n\t<integer>3600</integer>"));
    CHECK(th::has(p, "<array>\n\t\t<string>/opt/homebrew/bin/hdmon</string>\n\t\t<string>check</string>\n\t\t<string>--quiet</string>\n\t</array>"));
    CHECK(th::has(p, "<key>RunAtLoad</key>\n\t<true/>"));
}

TEST_CASE("Windows task XML") {
    std::string x = windows_task_xml({"C:\\Program Files\\hdmon\\hdmonw.exe", "--config", "C:\\a b\\c.toml"}, 3600, true, "PC\\richard");
    CHECK(th::has(x, "<LogonTrigger>"));
    CHECK(th::has(x, "<Delay>PT2M</Delay>"));
    CHECK(th::has(x, "<Interval>PT1H</Interval>"));
    CHECK(th::has(x, "<RunLevel>HighestAvailable</RunLevel>"));
    CHECK(th::has(x, "<UserId>PC\\richard</UserId>"));
    CHECK(th::has(x, "<Command>C:\\Program Files\\hdmon\\hdmonw.exe</Command>"));
    CHECK(th::has(x, "<Arguments>--config &quot;C:\\a b\\c.toml&quot; check --quiet</Arguments>"));
    std::string low = windows_task_xml({"hdmon.exe"}, 1800, false, "");
    CHECK(th::has(low, "<RunLevel>LeastPrivilege</RunLevel>"));
    CHECK(th::has(low, "<Interval>PT30M</Interval>"));
    std::string u16 = utf16le_with_bom("<a>é</a>");
    CHECK(u16 == std::string("\xFF\xFE<\0a\0>\0\xE9\0<\0/\0a\0>\0", 18));
}

TEST_CASE("cron fallback") {
    auto lines = cron_lines({"/home/r/.local/bin/hdmon"}, 3600);
    CHECK(starts_with(lines[0], "@reboot sleep 120; /home/r/.local/bin/hdmon check --quiet"));
    CHECK(starts_with(lines[1], "7 * * * * "));
    for (auto& l : lines) CHECK(ends_with(l, "# hard-drive-monitor"));
    CHECK(starts_with(cron_lines({"x"}, 1800)[1], "*/30 "));
}

TEST_CASE("scheduled program is this executable") {
    auto argv = program_args();
    REQUIRE(argv.size() == 1);
    CHECK(file_exists(argv[0]));
}
}

#include <regex>
TEST_SUITE("util") {
TEST_CASE("ISO timestamps carry a numeric UTC offset on every OS") {
    CHECK(std::regex_match(local_now_iso(), std::regex(R"(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d[+-]\d\d:\d\d)")));
    CHECK(std::regex_match(utc_now_iso(), std::regex(R"(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\+00:00)")));
}
TEST_CASE("Python-compatible number formatting") {
    CHECK(py_round(2.5) == 2);
    CHECK(py_round(3.5) == 4);
    CHECK(with_commas(1234567) == "1,234,567");
    CHECK(fmt_fixed(4.0007, 1) == "4.0");
    CHECK(to_str(json(nullptr)) == "None");
    CHECK(to_str(json(true)) == "True");
}
}
