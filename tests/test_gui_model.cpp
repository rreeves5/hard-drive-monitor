// hdmon-gui logic without a toolkit: tray colour, tooltip, names, details rows, settings, autostart files.
#include "helpers.hpp"
#include "autostart.hpp"
#include "collect.hpp"
#include "config.hpp"
#include "gui_model.hpp"
#include "raid.hpp"
#include "smart.hpp"
#include "update.hpp"

using namespace hdmon;
namespace g = hdmon::gui;

namespace {
std::vector<Result> fixture_results() { return score(load_json_files({th::fix()}), default_config()); }
Result fake(const std::string& status, std::optional<int> health, std::vector<std::string> vols = {}, std::string dev = "/dev/sdx") {
    Drive d;
    d.device = dev;
    d.volumes = vols;
    Assessment a;
    a.status = status;
    a.health = health;
    return {d, a};
}
}  // namespace

TEST_SUITE("gui") {
TEST_CASE("tray colour = worst drive, VB order UNKNOWN < OK < WARNING < CRITICAL") {
    CHECK(g::tray_status({}) == "UNKNOWN");
    CHECK(g::tray_status({fake("UNKNOWN", std::nullopt)}) == "UNKNOWN");
    CHECK(g::tray_status({fake("UNKNOWN", std::nullopt), fake("OK", 99)}) == "OK");
    CHECK(g::tray_status({fake("OK", 99), fake("WARNING", 50)}) == "WARNING");
    CHECK(g::tray_status({fake("CRITICAL", 10), fake("WARNING", 50), fake("OK", 100)}) == "CRITICAL");
    CHECK(g::tray_status(fixture_results()) == "CRITICAL");                 // sdb in the fixtures
    CHECK(g::status_color("OK") == "#00FF00");
    CHECK(g::status_color("WARNING") == "#FFFF00");
    CHECK(g::status_color("CRITICAL") == "#FF0000");
    CHECK(g::status_color("UNKNOWN") == "#8A8F98");
    CHECK(g::row_tint("CRITICAL") == "#FF0000");
    CHECK(g::row_tint("UNKNOWN") == "#E4E6EA");
    CHECK(g::status_label("UNKNOWN") == "No SMART data");
}

TEST_CASE("drive names, health/temp text and display order") {
    CHECK(g::drive_name(fake("OK", 1, {"C:"}).first) == "C:");
    CHECK(g::drive_name(fake("OK", 1, {"E:", "F:"}).first) == "E: F:");
    CHECK(g::drive_name(fake("OK", 1, {"/", "/home", "/boot", "/srv"}).first) == "/ /home /boot \u2026");
    CHECK(g::drive_name(fake("OK", 1, {}, "PhysicalDrive2").first) == "Disk 2");
    CHECK(g::drive_name(fake("OK", 1, {}, "/dev/nvme0").first) == "nvme0");
    CHECK(g::health_text(fake("OK", 93).second) == "93%");
    CHECK(g::health_text(fake("UNKNOWN", std::nullopt).second) == "n/a");
    Drive d;
    CHECK(g::temp_text(d) == "\u2014");
    d.temperature_c = 38;
    CHECK(g::temp_text(d) == "38 \u00B0C");
    auto sorted = g::sorted_for_display({fake("OK", 1, {}, "PhysicalDrive0"), fake("OK", 1, {"Z:"}), fake("OK", 1, {"C:"})});
    CHECK(g::drive_name(sorted[0].first) == "C:");
    CHECK(g::drive_name(sorted[1].first) == "Z:");
    CHECK(g::drive_name(sorted[2].first) == "Disk 0");
}

TEST_CASE("tooltip lists each drive with its health and fits Windows' 127 characters") {
    std::string t = g::tooltip_text({fake("OK", 93, {"C:"}), fake("CRITICAL", 3, {"D:"}), fake("UNKNOWN", std::nullopt, {"H:"})},
                                    "Beta V 0.2.3");
    CHECK(t == "Hard Drive Monitor Beta V 0.2.3\nC: 93%\nD: 3%\nH: n/a");
    CHECK(g::tooltip_text({}, "Beta V 0.2.3") == "Hard Drive Monitor Beta V 0.2.3\nNo drives found");
    std::vector<Result> many;
    for (char c = 'C'; c <= 'Z'; ++c) many.push_back(fake("OK", 100, {std::string(1, c) + ":"}));
    std::string cut = g::tooltip_text(many, "Beta V 0.2.3", 127);
    CHECK(cut.size() <= 127);
    CHECK(ends_with(cut, "..."));
    CHECK(g::tooltip_text(many, "Beta V 0.2.3", 5000).size() > 127);
    CHECK(g::tooltip_scanning("Beta V 0.2.3") == "Hard Drive Monitor Beta V 0.2.3 - scanning...");
    CHECK(th::has(g::tooltip_failed("Beta V 0.2.3"), "last scan failed"));
}

TEST_CASE("details rows (VB DriveDetails port) from hdmon's data") {
    auto res = fixture_results();
    auto hit = find_drive(res, "sdb");
    REQUIRE(hit);
    auto rows = g::detail_rows(hit->first, hit->second);
    std::map<std::string, std::string> m(rows.begin(), rows.end());
    CHECK(rows.front().first == "Status");
    CHECK(th::has(m["Status"], "Critical (10% health"));
    CHECK(m["Reallocated sectors"] == "120");
    CHECK(rows.back().first == "Notes");
    CHECK(th::has(m["Notes"], "reallocated"));
    auto ok = find_drive(res, "sda");
    auto r2 = g::detail_rows(ok->first, ok->second);
    CHECK(r2.back().second == "No problems found");
}

TEST_CASE("GUI settings: defaults, clamping, save keeps updates off unless chosen") {
    g::GuiSettings d = g::load_gui_settings(default_config());
    CHECK(d.alert_on_danger);
    CHECK(d.scan_interval_minutes == 60);
    CHECK_FALSE(d.auto_check);
    th::TempDir tmp;
    std::string p = tmp.file("c.toml");
    write_file(p, "[gui]\nscan_interval_minutes = 99999\nalert_on_danger = false\n");
    g::GuiSettings s = g::load_gui_settings(load_config(p));
    CHECK(s.scan_interval_minutes == 1440);
    CHECK_FALSE(s.alert_on_danger);
    s.scan_interval_minutes = 15;
    s.auto_check = true;
    REQUIRE(g::save_gui_settings(p, s, nullptr));
    json c = load_config(p);
    CHECK(c["gui"]["scan_interval_minutes"] == 15);
    CHECK(c["update"]["auto_check"] == true);
    CHECK_FALSE(c["update"].contains("auto_install"));
}

TEST_CASE("autostart files") {
    CHECK(desktop_exec_quote("/usr/bin/hdmon-gui") == "/usr/bin/hdmon-gui");
    CHECK(desktop_exec_quote("/home/r/My Apps/hdmon-gui") == "\"/home/r/My Apps/hdmon-gui\"");
    CHECK(desktop_exec_quote("/a/$x\"y") == "\"/a/\\\\$x\\\\\"y\"");   // \$ and \" with the backslash doubled
    CHECK(desktop_exec_quote("50%") == "50%%");
    std::string de = desktop_entry({"/home/r/.local/bin/hdmon-gui"}, "hard-drive-monitor", true);
    CHECK(th::has(de, "[Desktop Entry]\nType=Application\nName=Hard Drive Monitor\n"));
    CHECK(th::has(de, "Exec=/home/r/.local/bin/hdmon-gui\n"));
    CHECK(th::has(de, "Icon=hard-drive-monitor\n"));
    CHECK(th::has(de, "X-GNOME-Autostart-enabled=true\n"));
    CHECK_FALSE(th::has(desktop_entry({"x"}, "i", false), "Autostart"));
    std::string pl = launch_agent_plist({"/Applications/Hard Drive Monitor.app/Contents/MacOS/Hard Drive Monitor"});
    CHECK(th::has(pl, "<string>com.rreeves5.hard-drive-monitor.gui</string>"));
    CHECK(th::has(pl, "<string>/Applications/Hard Drive Monitor.app/Contents/MacOS/Hard Drive Monitor</string>"));
    CHECK(th::has(pl, "<key>RunAtLoad</key>\n  <true/>"));
    CHECK(th::has(launch_agent_plist({"a&b<c"}), "<string>a&amp;b&lt;c</string>"));
    auto args = windows_autostart_task_args("C:\\Users\\Rich\\AppData\\Local\\hard-drive-monitor\\bin\\hdmon-gui.exe");
    CHECK(join(args, "|") == "schtasks|/Create|/TN|Hard Drive Monitor GUI|/TR|\"C:\\Users\\Rich\\AppData\\Local\\hard-drive-monitor\\bin\\hdmon-gui.exe\"|/SC|ONLOGON|/RL|HIGHEST|/F");
    std::string tx = windows_autostart_task_xml("C:\\A&B\\hdmon-gui.exe", "PC\\rich");
    CHECK(th::has(tx, "<LogonTrigger><Enabled>true</Enabled><UserId>PC\\rich</UserId></LogonTrigger>"));
    CHECK(th::has(tx, "<RunLevel>HighestAvailable</RunLevel>"));
    CHECK(th::has(tx, "<Command>C:\\A&amp;B\\hdmon-gui.exe</Command>"));
    CHECK(th::has(tx, "<ExecutionTimeLimit>PT0S</ExecutionTimeLimit>"));
    CHECK(windows_run_value("C:\\x\\hdmon-gui.exe") == "\"C:\\x\\hdmon-gui.exe\"");
    std::string lp = replace_all(autostart_path(OS::Linux), "\\", "/");
    CHECK(ends_with(lp, "autostart/hard-drive-monitor.desktop"));
    CHECK(th::has(autostart_path(OS::MacOS), "LaunchAgents"));
    CHECK(autostart_path(OS::Windows).empty());
}

#ifndef _WIN32
TEST_CASE("set_autostart writes and removes the per-user file (XDG_CONFIG_HOME / HOME redirected)") {
    th::TempDir tmp;
    std::string old_home = get_env("HOME"), old_xdg = get_env("XDG_CONFIG_HOME");
    set_env("HOME", tmp.path.string());
    set_env("XDG_CONFIG_HOME", (tmp.path / "cfg").string());
    CHECK_FALSE(autostart_enabled());
    REQUIRE(set_autostart(true, "/opt/hdmon/hdmon-gui", nullptr));
    CHECK(autostart_enabled());
    std::string body = read_file(autostart_path(current_os()));
    CHECK(th::has(body, "/opt/hdmon/hdmon-gui"));
    REQUIRE(set_autostart(false, "/opt/hdmon/hdmon-gui", nullptr));
    CHECK_FALSE(autostart_enabled());
    set_env("HOME", old_home);
    set_env("XDG_CONFIG_HOME", old_xdg);
}
#endif

TEST_CASE("GUI update assets") {
    CHECK(asset_names(OS::Windows, "x86_64", UpdateFlavor::Gui) == std::vector<std::string>{"hdmon-gui-windows-x86_64.exe"});
    CHECK(asset_names(OS::Linux, "x86_64", UpdateFlavor::Gui) == std::vector<std::string>{"hdmon-gui-linux-x86_64"});
    CHECK(asset_names(OS::MacOS, "arm64", UpdateFlavor::Gui).empty());     // zipped .app: manual download
    CHECK(asset_names(OS::Windows, "x86_64") == std::vector<std::string>{"hdmon-windows-x86_64.exe", "hdmonw-windows-x86_64.exe"});
    CHECK(ends_with(update_state_path("/x/history.db", UpdateFlavor::Gui), "update-state-gui.json"));
    CHECK(ends_with(update_state_path("/x/history.db"), "update-state.json"));
}
}

TEST_CASE("palette: exact status colours are pinned") {
    namespace p = hdmon::palette;
    auto eq = [](p::Rgb c, int r, int gg, int b) { return c.r == r && c.g == gg && c.b == b; };
    // Tray circle / status
    CHECK(std::string(p::STATUS_OK) == "#00FF00");
    CHECK(std::string(p::STATUS_WARNING) == "#FFFF00");
    CHECK(std::string(p::STATUS_CRITICAL) == "#FF0000");
    CHECK(std::string(p::STATUS_UNKNOWN) == "#8A8F98");
    CHECK(eq(p::to_rgb(p::STATUS_OK), 0, 255, 0));
    CHECK(eq(p::to_rgb(p::STATUS_WARNING), 255, 255, 0));
    CHECK(eq(p::to_rgb(p::ROW_WARNING), 255, 255, 0));
    CHECK(eq(p::to_rgb(p::ROW_OK), 0, 255, 0));
    CHECK(eq(p::to_rgb(p::STATUS_CRITICAL), 255, 0, 0));
    CHECK(eq(p::to_rgb(p::STATUS_UNKNOWN), 138, 143, 152));
    // Row tints, black text
    CHECK(std::string(p::ROW_OK) == "#00FF00");
    CHECK(std::string(p::ROW_WARNING) == "#FFFF00");
    CHECK(std::string(p::ROW_CRITICAL) == "#FF0000");
    CHECK(std::string(p::ROW_UNKNOWN) == "#E4E6EA");
    CHECK(eq(p::to_rgb(p::ROW_CRITICAL), 255, 0, 0));
    CHECK(eq(p::to_rgb(p::ROW_UNKNOWN), 228, 230, 234));
    CHECK(eq(p::to_rgb(p::ROW_TEXT), 0, 0, 0));
    // GUI helpers go through the palette
    CHECK(g::status_color("CRITICAL") == p::STATUS_CRITICAL);
    CHECK(g::row_tint("CRITICAL") == p::ROW_CRITICAL);
    CHECK(g::status_color("") == p::STATUS_UNKNOWN);
    CHECK(g::row_tint("") == p::ROW_UNKNOWN);
    // Tray icon geometry (VB FillEllipse 2,2,28,28 on 32 px)
    CHECK(p::ICON_SIZE == 32);
    CHECK(p::CIRCLE_X == 2);
    CHECK(p::CIRCLE_Y == 2);
    CHECK(p::CIRCLE_D == 28);
}

TEST_CASE("RAID rows: array bold with arrow and state, members indented, raw disks not repeated") {
    Scan sc = scan(default_config(), {th::fix("raid/sample")});
    REQUIRE_FALSE(sc.arrays.empty());
    auto rows = g::display_rows(sc.results, sc.arrays);
    const g::DisplayRow* md0 = nullptr;
    size_t md0_at = 0;
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].kind == g::DisplayRow::ARRAY && th::has(rows[i].cells[0], "md0")) { md0 = &rows[i]; md0_at = i; }
    REQUIRE(md0);
    CHECK(md0->bold);
    CHECK(md0->expanded);
    CHECK(md0->cells[0] == "\u25BC md0");
    CHECK(md0->cells[4] == "Degraded");
    CHECK(md0->status == "CRITICAL");                 // degraded mirror with a critical member
    CHECK(g::row_tint(md0->status) == "#FF0000");
    REQUIRE(md0_at + 2 < rows.size());
    CHECK(rows[md0_at + 1].kind == g::DisplayRow::MEMBER);
    CHECK(rows[md0_at + 1].cells[0] == "      sdb");
    CHECK(rows[md0_at + 1].status == "CRITICAL");
    CHECK(rows[md0_at + 2].cells[0] == "      sdd (failed)");
    // a failed member is Critical red even though its own SMART is 100%
    CHECK(rows[md0_at + 2].status == "CRITICAL");
    CHECK(g::row_tint(rows[md0_at + 2].status) == "#FF0000");
    CHECK(rows[md0_at + 2].cells[4] == "Failed (removed from array)");
    // the controller's own state is unknown: rolled up from the disks, and the label says so
    for (auto& r : rows)
        if (r.kind == g::DisplayRow::ARRAY && th::has(r.cells[0], "MegaRAID")) {
            CHECK(r.cells[4] == "Worst disk OK (controller state not readable)");
            CHECK(r.status == "OK");
        }
    // sdb/sdd only appear as members
    int sdb = 0;
    for (auto& r : rows) sdb += th::has(r.cells[0], "sdb");
    CHECK(sdb == 1);
    // every drive is shown exactly once (top level or member)
    std::set<int> seen;
    for (auto& r : rows)
        if (r.result >= 0) CHECK(seen.insert(r.result).second);
    CHECK(seen.size() == sc.results.size());

    auto collapsed = g::display_rows(sc.results, sc.arrays, {sc.arrays[0].id});
    CHECK(collapsed.size() == rows.size() - sc.arrays[0].members.size());
    for (auto& r : collapsed)
        if (r.kind == g::DisplayRow::ARRAY && r.array == 0) {
            CHECK_FALSE(r.expanded);
            CHECK(th::has(r.cells[0], "\u25B6 "));
        }
}

TEST_CASE("RAID: tray counts arrays, tooltip lists them, details rows, rebuild text") {
    Result ok = fake("OK", 100, {}, "/dev/sdb");
    ok.first.serial = "A";
    Result ok2 = fake("OK", 100, {}, "/dev/sdc");
    ok2.first.serial = "B";
    std::vector<Result> rs = {ok, ok2};
    RaidArray a;
    a.id = "mdadm:x";
    a.name = "md0";
    a.level = "raid1";
    a.backend = "mdadm";
    a.state = "REBUILDING";
    a.rebuild_pct = 45.0;
    ArrayMember m1, m2;
    m1.device = "/dev/sdb1";
    m1.slot = 0;
    m2.device = "/dev/sdc1";
    m2.slot = 1;
    m2.role = "rebuilding";
    a.members = {m1, m2};
    std::vector<RaidArray> arrs = {a};
    link_members(arrs, rs);
    CHECK(g::tray_status(rs) == "OK");
    CHECK(g::tray_status(rs, arrs) == "WARNING");     // array state counts toward the tray colour
    arrs[0].state = "FAILED";
    CHECK(g::tray_status(rs, arrs) == "CRITICAL");
    arrs[0].state = "REBUILDING";
    std::string tip = g::tooltip_text(rs, arrs, "Beta V 0.2.4", 1000);
    CHECK(th::has(tip, "md0 Rebuilding 45.0%"));
    CHECK_FALSE(th::has(tip, "sdb 100%"));              // members are not listed twice
    auto rows = g::display_rows(rs, arrs);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].cells[4] == "Rebuilding 45.0%");
    CHECK(rows[0].status == "WARNING");
    CHECK(rows[2].cells[0] == "      sdc (rebuilding)");
    auto det = g::array_detail_rows(arrs[0], rs);
    CHECK(det[0].first == "Status");
    CHECK(det[0].second == "Warning (Rebuilding 45.0%)");
    bool member = false;
    for (auto& [k, v] : det) member = member || (k == "Member 1 sdc" && v == "Rebuilding \u00B7 Warning, SMART 100%");
    CHECK(member);

    // a member with no SMART data (e.g. Storage Spaces disk not matched) still shows, failed ones in red
    RaidArray b = a;
    b.id = "x2";
    b.members[1].device = "";
    b.members[1].role = "failed";
    b.members[1].drive_key.clear();
    auto rows2 = g::display_rows({}, {b});
    CHECK(rows2.back().cells[0] == "      (missing disk) (failed)");
    CHECK(rows2.back().status == "CRITICAL");
}

TEST_CASE("source text and smartmontools credit") {
    Result n = fake("OK", 100);
    n.first.source = "native (SG_IO ATA PASS-THROUGH(16))";
    Result s = fake("OK", 100);
    s.first.source = "smartctl (native read failed: x)";
    json meta = {{"smartctl_version", "7.4"}};
    CHECK(g::source_text({n}, meta) == "source: native SMART");
    CHECK_FALSE(g::used_smartctl({n}, meta));
    CHECK(g::source_text({n, s}, meta) == "source: native SMART + smartctl 7.4 (fallback)");
    CHECK(g::used_smartctl({n, s}, meta));
    CHECK(g::source_text({n}, {{"source", "json-files"}}) == "sample data (--from-json)");
}

TEST_CASE("fit_column_widths fills the list exactly, Model takes slack, never below header") {
    using g::fit_column_widths;
    auto sum = [](const std::vector<int>& v) { int t = 0; for (int x : v) t += x; return t; };
    std::vector<int> mins{100, 200, 60, 50, 80}, hdr{40, 45, 35, 40, 50};
    auto w = fit_column_widths(mins, hdr, 900, 1);            // wide: extra goes to Model only
    CHECK(sum(w) == 900);
    CHECK(w == std::vector<int>{100, 610, 60, 50, 80});
    w = fit_column_widths(mins, hdr, 490, 1);                  // exact fit
    CHECK(w == mins);
    w = fit_column_widths(mins, hdr, 300, 1);                  // narrow: proportional shrink, clamped at header
    CHECK(sum(w) == 300);
    for (size_t i = 0; i < w.size(); ++i) CHECK(w[i] >= hdr[i]);
    CHECK(w[1] > w[0]);
    w = fit_column_widths(mins, hdr, 215, 1);                  // only headers fit
    CHECK(sum(w) == 215);
    CHECK(w == std::vector<int>{40, 50, 35, 40, 50});
    CHECK(fit_column_widths(mins, hdr, 100, 1) == hdr);        // too narrow even for headers
    CHECK(fit_column_widths({10, 300}, {80, 20}, 400, 1) == std::vector<int>{80, 320});   // header beats content
}
