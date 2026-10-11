// hdmon-gui logic without a toolkit: tray colour, tooltip, names, details rows, settings, autostart files.
#include "helpers.hpp"
#include "autostart.hpp"
#include "collect.hpp"
#include "config.hpp"
#include "gui_model.hpp"
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
    CHECK(g::status_color("WARNING") == "#FFCC00");
    CHECK(g::status_color("CRITICAL") == "#D0312D");
    CHECK(g::status_color("UNKNOWN") == "#8A8F98");
    CHECK(g::row_tint("CRITICAL") == "#F8CDCB");
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
    CHECK_FALSE(d.auto_install);
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
    CHECK(c["update"]["auto_install"] == false);
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
