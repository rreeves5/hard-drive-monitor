#include <cctype>
#include "alerts.hpp"
#include "osnative.hpp"

#include "raid.hpp"
#include "render.hpp"

#include <cstdio>
#include <filesystem>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace hdmon {

static std::string pretty(const std::string& k) {
    static const std::map<std::string, std::string> P = {
        {"reallocated_sectors", "reallocated sectors"}, {"pending_sectors", "pending sectors"},
        {"offline_uncorrectable", "offline uncorrectable sectors"}, {"reported_uncorrectable", "reported uncorrectable errors"},
        {"command_timeout", "command timeouts"}, {"udma_crc_errors", "UDMA CRC errors"}, {"media_errors", "NVMe media errors"},
        {"uncorrected_errors", "uncorrected errors"}, {"status", "status"}};
    auto it = P.find(k);
    return it == P.end() ? k : it->second;
}

std::string drive_label(const Drive& d) {
    std::string vol = d.volumes.empty() ? "" : " (" + join(d.volumes, " ") + ")";
    return trim(d.device + vol + " " + d.model.value_or(""));
}

static std::string title_case(std::string s) {
    for (size_t i = 0; i < s.size(); ++i) s[i] = static_cast<char>(i ? std::tolower(static_cast<unsigned char>(s[i])) : std::toupper(static_cast<unsigned char>(s[i])));
    return s;
}

// One line per drive: "C: NX-2TB, Warning, 88%".
std::string alert_line(const Drive& d, const Assessment& a) {
    std::string name = trim((d.volumes.empty() ? d.device : d.volumes[0]) + " " + d.model.value_or(""));
    return name + ", " + title_case(a.status) + (a.health ? ", " + std::to_string(*a.health) + "%" : std::string());
}

std::vector<Alert> build_alerts(const std::vector<Result>& results, const Changes& changes) {
    std::vector<Alert> out;
    for (auto& [d, a] : results) {
        auto it = changes.find(d.key());
        if (it == changes.end()) continue;
        bool hit = false;
        for (auto& c : it->second) hit = hit || truthy(jget(c, "alert"));
        if (hit) out.push_back({a.status, "Hard Drive Monitor", alert_line(d, a)});
    }
    return out;
}

std::vector<Alert> build_array_alerts(const std::vector<RaidArray>& arrays, const std::vector<Result>& results,
                                      const Changes& array_changes) {
    std::vector<Alert> out;
    for (auto& a : arrays) {
        auto it = array_changes.find(a.id);
        if (it == array_changes.end()) continue;
        for (auto& c : it->second) {
            if (!truthy(jget(c, "alert"))) continue;
            std::string ev = jstr(c, "event").value_or("");
            std::string state = ev == "rebuild_complete" ? "Rebuild complete" : title_case(a.state);
            if (a.rebuild_pct && a.state == "REBUILDING") state += ", " + fmt_fixed(*a.rebuild_pct, 0) + "%";
            out.push_back({ev == "rebuild_complete" ? std::string("OK") : array_status(a, results), "Hard Drive Monitor",
                           trim(a.name + " " + a.level) + ", " + state});
        }
    }
    return out;
}

std::vector<Alert> filter_alerts(const json& cfg, std::vector<Alert> alerts) {
    if (jstr(jget(cfg, "alerts"), "min_level").value_or("warning") != "critical") return alerts;
    std::vector<Alert> out;
    for (auto& a : alerts)
        if (a.severity == "CRITICAL") out.push_back(a);
    return out;
}

std::string alert_text(const std::vector<Alert>& alerts) {
    std::vector<std::string> l;
    for (auto& a : alerts) l.push_back(a.body);
    return join(l, "\n");
}

static std::string clip(const std::string& s, size_t n) { return u8substr(s, n); }

std::pair<bool, std::string> notify_desktop(const std::string& title, const std::string& body) {
    // Native first (D-Bus / Shell_NotifyIcon / UserNotifications); the helper programs below are the fallback.
    std::string native_why;
    if (osn::notify(title, clip(body, 240), native_why)) return {true, native_why};
    switch (current_os()) {
        case OS::MacOS: {
            auto r = run_cmd({"osascript", "-e", "on run argv", "-e",
                              "display notification (item 2 of argv) with title (item 1 of argv) sound name \"Basso\"",
                              "-e", "end run", title, clip(body, 240)}, 30);
            return {r.rc == 0, r.started ? (trim(r.err).empty() ? "osascript (" + native_why + ")" : trim(r.err)) : "osascript not found"};
        }
        case OS::Windows: {
            auto ps = powershell_exe();
            if (!ps) return {false, native_why + "; PowerShell not found; skipped desktop notification"};
            set_env("HDMON_TITLE", title);
            set_env("HDMON_BODY", clip(body, 240));
            std::string script =
                "$t=$env:HDMON_TITLE; $b=$env:HDMON_BODY;"
                "if (Get-Module -ListAvailable -Name BurntToast) { Import-Module BurntToast;"
                " New-BurntToastNotification -Text $t, $b; 'burnttoast' } else {"
                " Add-Type -AssemblyName System.Windows.Forms; Add-Type -AssemblyName System.Drawing;"
                " $n = New-Object System.Windows.Forms.NotifyIcon; $n.Icon = [System.Drawing.SystemIcons]::Warning;"
                " $n.Visible = $true; $n.ShowBalloonTip(10000, $t, $b, 'Warning'); Start-Sleep -Seconds 10;"
                " $n.Dispose(); 'balloon' }";
            auto r = run_cmd({*ps, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", script}, 60);
            return {r.rc == 0, trim(r.out).empty() ? trim(r.err) : trim(r.out)};
        }
        default: {
            if (!which("notify-send")) return {false, native_why + "; notify-send not found either; skipped desktop notification"};
            auto r = run_cmd({"notify-send", "-a", "Hard Drive Monitor", "-u", "critical", "-i", "drive-harddisk", title, body}, 30);
            return {r.rc == 0, trim(r.err).empty() ? "notify-send (" + native_why + ")" : trim(r.err)};
        }
    }
}

static std::vector<std::string> recipients(const json& e) {
    std::vector<std::string> to;
    const json& t = jget(e, "to");
    if (t.is_string()) to.push_back(t.get<std::string>());
    else if (t.is_array()) for (auto& x : t) to.push_back(to_str(x));
    return to;
}

std::string build_email_message(const json& cfg, const std::string& subject, const std::string& body, const std::string& host) {
    const json& e = jget(cfg, "email");
    std::string msg;
    msg += "From: " + jstr(e, "from").value_or("") + "\r\n";
    msg += "To: " + join(recipients(e), ", ") + "\r\n";
    msg += "Subject: " + subject + "\r\n";
    msg += "MIME-Version: 1.0\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: 8bit\r\n\r\n";
    msg += replace_all(body + "\n\n-- Hard Drive Monitor on " + host, "\n", "\r\n") + "\r\n";
    return msg;
}

static std::string curl_quote(const std::string& s) {
    return "\"" + replace_all(replace_all(s, "\\", "\\\\"), "\"", "\\\"") + "\"";
}

std::string build_curl_config(const json& cfg, const std::string& message_path, const std::string& password) {
    const json& e = jget(cfg, "email");
    bool ssl = truthy(jget(e, "ssl"));
    int64_t port = jint(e, "port").value_or(ssl ? 465 : 587);
    std::string c;
    c += "url = " + curl_quote(std::string(ssl ? "smtps://" : "smtp://") + jstr(e, "host").value_or("") + ":" + std::to_string(port)) + "\n";
    const json& st = jget(e, "starttls");
    if (!ssl && (st.is_null() || truthy(st))) c += "ssl-reqd\n";
    c += "mail-from = " + curl_quote(jstr(e, "from").value_or("")) + "\n";
    for (auto& r : recipients(e)) c += "mail-rcpt = " + curl_quote(r) + "\n";
    std::string user = jstr(e, "username").value_or("");
    if (!user.empty()) c += "user = " + curl_quote(user + ":" + password) + "\n";
    c += "upload-file = " + curl_quote(message_path) + "\n";
    c += "silent\nshow-error\nmax-time = 60\n";
    return c;
}

std::pair<bool, std::string> send_email(const json& cfg, const std::string& subject, const std::string& body) {
    const json& e = jget(cfg, "email");
    if (!truthy(jget(e, "enabled"))) return {false, "email disabled"};
    if (jstr(e, "host").value_or("").empty() || recipients(e).empty() || jstr(e, "from").value_or("").empty())
        return {false, "email enabled but host/from/to missing in config"};
    auto curl = which("curl");
    if (!curl) return {false, "email needs the curl command-line tool, which was not found"};
    std::string password = jstr(e, "password").value_or("");
    if (password.empty()) { auto env = jstr(e, "password_env"); if (env && !env->empty()) password = get_env(env->c_str()); }
    std::string host = get_env("COMPUTERNAME");
#ifndef _WIN32
    if (host.empty()) {   // gethostname(2), not the `hostname` program
        char buf[256] = {0};
        if (gethostname(buf, sizeof buf - 1) == 0) host = buf;
    }
#endif
    auto tmp = std::filesystem::temp_directory_path() / ("hdmon-mail-" + std::to_string(std::rand()) + ".eml");
    write_file(tmp.string(), build_email_message(cfg, subject, body, host));
    std::string conf = build_curl_config(cfg, tmp.string(), password);   // via stdin: password never on the command line
    auto r = run_cmd({*curl, "-K", "-"}, 90, &conf);
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    if (r.rc == 0) return {true, "sent"};
    return {false, "email failed: curl exit " + std::to_string(r.rc) + " " + trim(r.err)};
}

std::vector<std::string> dispatch(const json& cfg, const std::vector<Alert>& alerts) {
    std::vector<std::string> log;
    if (alerts.empty()) return log;
    const json& desk = jget(jget(cfg, "alerts"), "desktop");
    if (desk.is_null() || truthy(desk)) {
        auto [ok, info] = notify_desktop("Hard Drive Monitor", alert_text(alerts));
        log.push_back(std::string("desktop: ") + (ok ? "ok" : "skipped/failed") + " (" + info + ")");
    }
    if (truthy(jget(jget(cfg, "email"), "enabled"))) {
        auto [ok, info] = send_email(cfg, "Hard Drive Monitor", alert_text(alerts));
        (void)ok;
        log.push_back("email: " + info);
    }
    return log;
}

}  // namespace hdmon
