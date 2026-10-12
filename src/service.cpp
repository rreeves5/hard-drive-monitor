#include "service.hpp"
#include "dbus.hpp"
#include "osnative.hpp"
#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <regex>
#include <stdexcept>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace hdmon {

const char* SERVICE_LABEL = "com.hard-drive-monitor.check";
const char* TASK_NAME = "hard-drive-monitor";
const char* CRON_MARK = "# hard-drive-monitor";

int64_t parse_interval(const std::string& text) {
    std::string t = lower(trim(text));
    if (!t.empty() && std::all_of(t.begin(), t.end(), [](unsigned char c) { return std::isdigit(c); }))
        return std::stoll(t);
    static const std::regex re(R"((\d+(?:\.\d+)?)\s*([dhms]))");
    std::string compact, joined;
    for (char c : t) if (!std::isspace(static_cast<unsigned char>(c))) compact += c;
    double secs = 0;
    bool any = false;
    for (auto it = std::sregex_iterator(t.begin(), t.end(), re); it != std::sregex_iterator(); ++it) {
        any = true;
        std::string n = (*it)[1], u = (*it)[2];
        joined += n + u;
        double mult = u == "d" ? 86400 : u == "h" ? 3600 : u == "m" ? 60 : 1;
        secs += std::stod(n) * mult;
    }
    if (!any || joined != compact)
        throw std::invalid_argument("bad interval '" + text + "' (use e.g. 1h, 30m, 90s, 2h30m)");
    auto s = static_cast<int64_t>(secs);
    if (s <= 0) throw std::invalid_argument("interval must be positive");
    return s;
}

std::vector<std::string> program_args(std::optional<OS> os) {
    std::string me = self_exe_path();
    if (os.value_or(current_os()) == OS::Windows) {
        // Prefer the GUI-subsystem twin so the scheduled run doesn't flash a console window.
        std::string name = basename(me);
        if (starts_with(lower(name), "hdmon") && !starts_with(lower(name), "hdmonw")) {
            std::string w = path_join(dirname(me), "hdmonw" + name.substr(5));   // hdmon.exe -> hdmonw.exe
            if (file_exists(w)) return {w};
        }
    }
    return {me};
}

static std::string quote(const std::string& a) {
    if (a.find(' ') == std::string::npos && a.find('"') == std::string::npos) return a;
    return "\"" + replace_all(a, "\"", "\\\"") + "\"";
}
static std::string cmd_line(std::vector<std::string> argv) {
    argv.push_back("check");
    argv.push_back("--quiet");
    std::vector<std::string> q;
    for (auto& a : argv) q.push_back(quote(a));
    return join(q, " ");
}

std::map<std::string, std::string> systemd_units(const std::vector<std::string>& argv, int64_t interval_s) {
    std::string service =
        "[Unit]\nDescription=Hard Drive Monitor SMART drive health check\n\n"
        "[Service]\nType=oneshot\nExecStart=" + cmd_line(argv) + "\nNice=10\nIOSchedulingClass=idle\n";
    std::string timer =
        "[Unit]\nDescription=Run Hard Drive Monitor (hdmon) periodically\n\n"
        "[Timer]\nOnStartupSec=2min\nOnUnitActiveSec=" + std::to_string(interval_s) + "s\n"
        "Persistent=true\nRandomizedDelaySec=60\n\n[Install]\nWantedBy=timers.target\n";
    return {{"hard-drive-monitor.service", service}, {"hard-drive-monitor.timer", timer}};
}

std::vector<std::string> cron_lines(const std::vector<std::string>& argv, int64_t interval_s) {
    std::string cmd = cmd_line(argv);
    int64_t minutes = std::max<int64_t>(1, interval_s / 60);
    std::string when;
    if (minutes % 60 == 0) {
        int64_t hours = minutes / 60;
        when = hours == 1 ? "7 * * * *" : "7 */" + std::to_string(hours) + " * * *";
    } else {
        when = "*/" + std::to_string(std::min<int64_t>(minutes, 59)) + " * * * *";
    }
    return {std::string("@reboot sleep 120; ") + cmd + " " + CRON_MARK, when + " " + cmd + " " + CRON_MARK};
}

static std::string xml_escape(const std::string& t) {
    return replace_all(replace_all(replace_all(replace_all(t, "&", "&amp;"), "<", "&lt;"), ">", "&gt;"), "\"", "&quot;");
}

std::string launchd_plist(const std::vector<std::string>& argv, int64_t interval_s, const std::string& log_dir) {
    std::string log = xml_escape(log_dir + "/hard-drive-monitor.log");
    std::string s =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n<dict>\n"
        "\t<key>EnvironmentVariables</key>\n\t<dict>\n\t\t<key>PATH</key>\n"
        "\t\t<string>/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/bin:/bin:/usr/sbin:/sbin</string>\n"
        "\t</dict>\n"
        "\t<key>Label</key>\n\t<string>" + std::string(SERVICE_LABEL) + "</string>\n"
        "\t<key>LowPriorityIO</key>\n\t<true/>\n"
        "\t<key>ProcessType</key>\n\t<string>Background</string>\n"
        "\t<key>ProgramArguments</key>\n\t<array>\n";
    std::vector<std::string> args = argv;
    args.push_back("check");
    args.push_back("--quiet");
    for (auto& a : args) s += "\t\t<string>" + xml_escape(a) + "</string>\n";
    s += "\t</array>\n"
         "\t<key>RunAtLoad</key>\n\t<true/>\n"
         "\t<key>StandardErrorPath</key>\n\t<string>" + log + "</string>\n"
         "\t<key>StandardOutPath</key>\n\t<string>" + log + "</string>\n"
         "\t<key>StartInterval</key>\n\t<integer>" + std::to_string(interval_s) + "</integer>\n"
         "</dict>\n</plist>\n";
    return s;
}

std::string windows_task_xml(const std::vector<std::string>& argv, int64_t interval_s, bool highest, const std::string& user) {
    int64_t minutes = std::max<int64_t>(1, interval_s / 60);
    std::string period = minutes % 60 == 0 ? "PT" + std::to_string(minutes / 60) + "H" : "PT" + std::to_string(minutes) + "M";
    std::vector<std::string> a(argv.begin() + 1, argv.end());
    a.push_back("check");
    a.push_back("--quiet");
    for (auto& x : a) if (x.find(' ') != std::string::npos) x = "\"" + x + "\"";
    std::string args = join(a, " ");
    std::string user_xml = user.empty() ? "" : "<UserId>" + xml_escape(user) + "</UserId>";
    return std::string("<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n") +
        "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
        "  <RegistrationInfo>\n"
        "    <Description>Hard Drive Monitor: hourly SMART health check (hdmon check). Remove with: hdmon uninstall-service</Description>\n"
        "  </RegistrationInfo>\n"
        "  <Triggers>\n"
        "    <LogonTrigger>\n"
        "      <Enabled>true</Enabled>\n"
        "      " + user_xml + "\n"
        "      <Delay>PT2M</Delay>\n"
        "    </LogonTrigger>\n"
        "    <TimeTrigger>\n"
        "      <Enabled>true</Enabled>\n"
        "      <StartBoundary>2026-01-01T00:00:00</StartBoundary>\n"
        "      <Repetition>\n"
        "        <Interval>" + period + "</Interval>\n"
        "        <StopAtDurationEnd>false</StopAtDurationEnd>\n"
        "      </Repetition>\n"
        "    </TimeTrigger>\n"
        "  </Triggers>\n"
        "  <Principals>\n"
        "    <Principal id=\"Author\">\n"
        "      " + user_xml + "\n"
        "      <LogonType>InteractiveToken</LogonType>\n"
        "      <RunLevel>" + (highest ? "HighestAvailable" : "LeastPrivilege") + "</RunLevel>\n"
        "    </Principal>\n"
        "  </Principals>\n"
        "  <Settings>\n"
        "    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n"
        "    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\n"
        "    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n"
        "    <StartWhenAvailable>true</StartWhenAvailable>\n"
        "    <ExecutionTimeLimit>PT30M</ExecutionTimeLimit>\n"
        "    <Priority>7</Priority>\n"
        "    <Enabled>true</Enabled>\n"
        "  </Settings>\n"
        "  <Actions Context=\"Author\">\n"
        "    <Exec>\n"
        "      <Command>" + xml_escape(argv[0]) + "</Command>\n"
        "      <Arguments>" + xml_escape(args) + "</Arguments>\n"
        "    </Exec>\n"
        "  </Actions>\n"
        "</Task>\n";
}

std::string utf16le_with_bom(const std::string& s) {
    std::string out = "\xFF\xFE";
    auto put = [&](uint32_t u) { out += static_cast<char>(u & 0xFF); out += static_cast<char>((u >> 8) & 0xFF); };
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp;
        int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; n = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; n = 3; }
        else { cp = c & 0x07; n = 4; }
        for (int k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        i += n;
        if (cp >= 0x10000) { cp -= 0x10000; put(0xD800 + (cp >> 10)); put(0xDC00 + (cp & 0x3FF)); }
        else put(cp);
    }
    return out;
}

// ------------------------------------------------------------------ helpers

static bool systemd_user_ok() {
    if (dbus::systemd_user_available()) return true;   // native: D-Bus to org.freedesktop.systemd1
    if (!which("systemctl")) return false;
    auto r = run_cmd({"systemctl", "--user", "show-environment"}, 15);
    return r.started && r.rc == 0;
}
static std::optional<std::vector<std::string>> crontab_read() {
    if (!which("crontab")) return std::nullopt;
    auto r = run_cmd({"crontab", "-l"}, 30);
    if (r.rc != 0) return std::vector<std::string>{};
    return split_lines(r.out);
}
static bool crontab_write(const std::vector<std::string>& lines) {
    std::string in = lines.empty() ? "" : join(lines, "\n") + "\n";
    return run_cmd({"crontab", "-"}, 30, &in).rc == 0;
}
static bool run_logged(const std::vector<std::string>& cmd, const Out& out) {
    out("  $ " + join(cmd, " "));
    auto r = run_cmd(cmd, 120);
    if (!r.started) { out("    failed: " + r.err); return false; }
    if (!trim(r.out).empty()) out("    " + replace_all(trim(r.out), "\n", "\n    "));
    if (r.rc != 0) out("    failed (exit " + std::to_string(r.rc) + "): " + trim(r.err));
    return r.rc == 0;
}
static std::string systemd_user_dir() {
    std::string x = get_env("XDG_CONFIG_HOME");
    if (x.empty()) x = path_join(home_dir(), ".config");
    return path_join(path_join(x, "systemd"), "user");
}


// ------------------------------------------------------------------ install / uninstall

static int install_linux(const std::vector<std::string>& argv, int64_t interval_s, bool enable, bool system, bool dry_run, const Out& out) {
    auto units = systemd_units(argv, interval_s);
    if (system) {
        out("System-wide (root) units are NOT installed automatically. To install them yourself:\n");
        for (auto& [name, text] : units) out("# /etc/systemd/system/" + name + "\n" + text);
        out("Then run:\n  sudo cp hard-drive-monitor.service hard-drive-monitor.timer /etc/systemd/system/\n"
            "  sudo systemctl daemon-reload && sudo systemctl enable --now hard-drive-monitor.timer\n"
            "Note: a root service writes history to /root/.local/share/hard-drive-monitor and cannot show desktop\n"
            "notifications in your session; configure e-mail alerts for it.");
        return 0;
    }
    if (enable && !dry_run && !systemd_user_ok()) {
        auto cur = crontab_read();
        if (!cur) {
            out("No systemd user session and no crontab found, so nothing was scheduled.\n"
                "Run `hdmon watch --interval 1h` from your session startup instead, or schedule\n"
                "`" + join(argv, " ") + " check --quiet` with your system's scheduler.");
            return 1;
        }
        std::vector<std::string> lines;
        for (auto& l : *cur) if (!contains(l, CRON_MARK)) lines.push_back(l);
        auto cl = cron_lines(argv, interval_s);
        lines.insert(lines.end(), cl.begin(), cl.end());
        out("No systemd user session found; using your crontab instead:");
        for (auto& l : cl) out("  " + l);
        if (!crontab_write(lines)) { out("  failed to write crontab"); return 1; }
        out("Scheduled via cron (at boot + every interval). Turn off with: hdmon uninstall-service");
        return 0;
    }
    std::string d = systemd_user_dir();
    for (auto& [name, text] : units) {
        out("--- " + path_join(d, name) + " ---\n" + text);
        if (!dry_run) { make_dirs(d); write_file(path_join(d, name), text); }
    }
    out(std::string(dry_run ? "Would write" : "Wrote") + " " + path_join(d, "hard-drive-monitor.service") + " and " +
        path_join(d, "hard-drive-monitor.timer"));
    std::vector<std::vector<std::string>> cmds = {{"systemctl", "--user", "daemon-reload"},
                                                  {"systemctl", "--user", "enable", "--now", "hard-drive-monitor.timer"}};
    if (enable && !dry_run) {
        std::string err;
        if (dbus::systemd_user_enable({"hard-drive-monitor.timer"}, "hard-drive-monitor.timer", err)) {
            out("  systemd (D-Bus): Reload, EnableUnitFiles + StartUnit hard-drive-monitor.timer");
        } else {
            out("  systemd D-Bus call failed (" + err + "); falling back to systemctl");
            for (auto& c : cmds) if (!run_logged(c, out)) return 1;
        }
        out("Scheduled: hourly + 2 min after you log in (systemd user timer, no root).\n"
            "  Check:    systemctl --user list-timers hard-drive-monitor.timer\n"
            "  Turn off: hdmon uninstall-service   (or: systemctl --user disable --now hard-drive-monitor.timer)\n"
            "  User timers only run while you are logged in; to run them without a login:\n"
            "            sudo loginctl enable-linger $USER");
    } else {
        out("To enable:\n  " + join(cmds[0], " ") + "\n  " + join(cmds[1], " "));
    }
    out("smartctl normally needs root: add the sudoers rule from README.md (sudo = \"auto\" in config.toml).");
    return 0;
}

static std::string uid_str() {
#ifdef _WIN32
    return "0";
#else
    return std::to_string(getuid());
#endif
}

static int install_macos(const std::vector<std::string>& argv, int64_t interval_s, bool enable, bool system, bool dry_run, const Out& out) {
    std::string log_dir = path_join(path_join(home_dir(), "Library"), "Logs");
    std::string text = launchd_plist(argv, interval_s, log_dir);
    std::string label = SERVICE_LABEL;
    if (system) {
        out("System-wide LaunchDaemon is NOT installed automatically. Save this as /Library/LaunchDaemons/" + label + ".plist:\n");
        out(text);
        out("Then: sudo chown root:wheel /Library/LaunchDaemons/" + label + ".plist && "
            "sudo launchctl bootstrap system /Library/LaunchDaemons/" + label + ".plist");
        return 0;
    }
    std::string dir = path_join(path_join(home_dir(), "Library"), "LaunchAgents");
    std::string path = path_join(dir, label + ".plist");
    out("--- " + path + " ---\n" + text);
    if (!dry_run) { make_dirs(dir); write_file(path, text); }
    out(std::string(dry_run ? "Would write " : "Wrote ") + path);
    std::string uid = uid_str();
    std::vector<std::string> cmd = {"launchctl", "bootstrap", "gui/" + uid, path};
    if (enable && !dry_run) {
        std::string err;
        if (osn::launchd_submit_plist(path, err)) {
            out("  launchd (ServiceManagement SMJobSubmit): loaded " + label + " for this session");
        } else {
            out("  SMJobSubmit failed (" + err + "); falling back to launchctl");
            run_logged({"launchctl", "bootout", "gui/" + uid + "/" + label}, [](const std::string&) {});
            if (!run_logged(cmd, out) && !run_logged({"launchctl", "load", "-w", path}, out)) return 1;
        }
        out("Scheduled: at login + hourly (launchd agent, runs as you).\n"
            "  Check:    launchctl print gui/" + uid + "/" + label + "\n"
            "  Turn off: hdmon uninstall-service");
    } else {
        out("To enable:\n  " + join(cmd, " "));
    }
    out("Log: " + log_dir + "/hard-drive-monitor.log. Internal Apple SSDs usually work without sudo; "
        "for other disks add the sudoers rule from README.md.");
    return 0;
}

static int install_windows(const std::vector<std::string>& argv, int64_t interval_s, bool enable, bool highest, bool dry_run, const Out& out) {
    if (!highest && is_windows_admin()) highest = true;   // elevated terminal: smartctl can read drives
    std::string uname = get_env("USERNAME"), dom = get_env("USERDOMAIN");
    std::string user = (!uname.empty() && !dom.empty()) ? dom + "\\" + uname : uname;
    std::string xml = windows_task_xml(argv, interval_s, highest, user);
    out(std::string("Task Scheduler task '") + TASK_NAME + "': at logon (+2 min) and every " + std::to_string(interval_s / 60) +
        " min, run level " + (highest ? "HIGHEST" : "normal (limited)") + "\n  " + join(argv, " ") + " check --quiet");
    if (!highest)
        out("Note: smartctl needs Administrator rights on Windows. Re-run from an elevated (Run as Administrator)\n"
            "terminal:  hdmon install-service   so the task runs with highest privileges. Without it the task\n"
            "falls back to the Windows Storage counters, or reports drives as unreadable.");
    if (dry_run || !enable) { out(xml); return 0; }
    std::string err;
    if (osn::task_register_xml(TASK_NAME, xml, err)) {
        out("  Task Scheduler (COM ITaskService): registered \\" + std::string(TASK_NAME));
        out(std::string("Scheduled. Check: Task Scheduler > ") + TASK_NAME + "   Turn off: hdmon uninstall-service");
        return 0;
    }
    out("  Task Scheduler COM call failed (" + err + "); falling back to schtasks");
    auto xml_path = (fs::temp_directory_path() / ("hdmon-task-" + std::to_string(std::rand()) + ".xml")).string();
    write_file(xml_path, utf16le_with_bom(xml));
    bool ok = run_logged({"schtasks", "/Create", "/TN", TASK_NAME, "/XML", xml_path, "/F"}, out);
    std::error_code ec;
    fs::remove(xml_path, ec);
    if (ok) out(std::string("Scheduled. Check: schtasks /Query /TN ") + TASK_NAME + "   Turn off: hdmon uninstall-service");
    return ok ? 0 : 1;
}

int install_service(int64_t interval_s, bool enable, bool system, bool highest, bool dry_run, const Out& out) {
    auto argv = program_args();
    switch (current_os()) {
        case OS::Linux: return install_linux(argv, interval_s, enable, system, dry_run, out);
        case OS::MacOS: return install_macos(argv, interval_s, enable, system, dry_run, out);
        default: return install_windows(argv, interval_s, enable, highest, dry_run, out);
    }
}

int uninstall_service(bool system, const Out& out) {
    OS os = current_os();
    if (os == OS::Linux) {
        if (system) {
            out("Remove the root units yourself:\n  sudo systemctl disable --now hard-drive-monitor.timer\n"
                "  sudo rm /etc/systemd/system/hard-drive-monitor.{service,timer} && sudo systemctl daemon-reload");
            return 0;
        }
        std::string d = systemd_user_dir();
        bool bus = systemd_user_ok();
        if (bus && file_exists(path_join(d, "hard-drive-monitor.timer"))) {
            std::string err;
            if (dbus::systemd_user_disable({"hard-drive-monitor.timer"}, "hard-drive-monitor.timer", err))
                out("  systemd (D-Bus): StopUnit + DisableUnitFiles hard-drive-monitor.timer");
            else {
                out("  systemd D-Bus call failed (" + err + "); falling back to systemctl");
                run_logged({"systemctl", "--user", "disable", "--now", "hard-drive-monitor.timer"}, out);
            }
        }
        for (const char* name : {"hard-drive-monitor.timer", "hard-drive-monitor.service"}) {
            std::string p = path_join(d, name);
            if (file_exists(p)) { std::error_code ec; fs::remove(p, ec); out("Removed " + p); }
        }
        if (bus) {
            std::string err;
            if (!dbus::systemd_user_reload(err)) run_logged({"systemctl", "--user", "daemon-reload"}, out);
        }
        auto cur = crontab_read();
        if (cur && std::any_of(cur->begin(), cur->end(), [](const std::string& l) { return contains(l, CRON_MARK); })) {
            std::vector<std::string> keep;
            for (auto& l : *cur) if (!contains(l, CRON_MARK)) keep.push_back(l);
            crontab_write(keep);
            out("Removed hard-drive-monitor lines from your crontab");
        }
        out("Hourly check removed.");
        return 0;
    }
    if (os == OS::MacOS) {
        std::string label = SERVICE_LABEL;
        if (system) {
            out("sudo launchctl bootout system/" + label + " && sudo rm /Library/LaunchDaemons/" + label + ".plist");
            return 0;
        }
        std::string path = path_join(path_join(path_join(home_dir(), "Library"), "LaunchAgents"), label + ".plist");
        std::string err;
        if (osn::launchd_remove(label, err)) {
            out("  launchd (ServiceManagement SMJobRemove): unloaded " + label);
        } else {
            out("  SMJobRemove failed (" + err + "); falling back to launchctl");
            if (!run_logged({"launchctl", "bootout", "gui/" + uid_str() + "/" + label}, out))
                run_logged({"launchctl", "unload", "-w", path}, out);
        }
        if (file_exists(path)) { std::error_code ec; fs::remove(path, ec); out("Removed " + path); }
        return 0;
    }
    std::string err;
    if (osn::task_delete(TASK_NAME, err)) { out(std::string("Removed scheduled task ") + TASK_NAME + " (Task Scheduler COM)"); return 0; }
    out("  Task Scheduler COM call failed (" + err + "); falling back to schtasks");
    return run_logged({"schtasks", "/Delete", "/TN", TASK_NAME, "/F"}, out) ? 0 : 1;
}

}  // namespace hdmon
