#include "autostart.hpp"
#include "osnative.hpp"

#include <filesystem>

#include "config.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace hdmon {
namespace fs = std::filesystem;

const char* AUTOSTART_LABEL = "com.rreeves5.hard-drive-monitor.gui";
const char* AUTOSTART_TASK = "Hard Drive Monitor GUI";

std::string desktop_exec_quote(const std::string& arg) {
    static const std::string reserved = " \t\n\"'\\><~|&;$*?#()`";
    bool needs = arg.empty() || arg.find_first_of(reserved) != std::string::npos;
    std::string out;
    if (needs) {
        out += '"';
        for (char c : arg) {
            if (c == '"' || c == '`' || c == '$' || c == '\\') out += '\\';
            out += c;
        }
        out += '"';
    } else {
        out = arg;
    }
    // The Exec value is also a desktop-entry string: backslashes doubled, '%' escaped as "%%".
    out = replace_all(out, "\\", "\\\\");
    return replace_all(out, "%", "%%");
}

std::string desktop_entry(const std::vector<std::string>& argv, const std::string& icon, bool autostart) {
    std::vector<std::string> q;
    for (auto& a : argv) q.push_back(desktop_exec_quote(a));
    std::string s = "[Desktop Entry]\n"
                    "Type=Application\n"
                    "Name=Hard Drive Monitor\n"
                    "GenericName=Drive health monitor\n"
                    "Comment=SMART drive health in the system tray\n"
                    "Exec=" + join(q, " ") + "\n"
                    "Icon=" + icon + "\n"
                    "Terminal=false\n"
                    "Categories=System;Monitor;\n";
    if (autostart) s += "X-GNOME-Autostart-enabled=true\nX-GNOME-Autostart-Delay=10\n";
    return s;
}

static std::string xml_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c;
        }
    }
    return o;
}

std::string launch_agent_plist(const std::vector<std::string>& argv) {
    std::string s = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                    "<plist version=\"1.0\">\n<dict>\n"
                    "  <key>Label</key>\n  <string>" + std::string(AUTOSTART_LABEL) + "</string>\n"
                    "  <key>ProgramArguments</key>\n  <array>\n";
    for (auto& a : argv) s += "    <string>" + xml_escape(a) + "</string>\n";
    s += "  </array>\n"
         "  <key>RunAtLoad</key>\n  <true/>\n"
         "  <key>ProcessType</key>\n  <string>Interactive</string>\n"
         "</dict>\n</plist>\n";
    return s;
}

std::vector<std::string> windows_autostart_task_args(const std::string& exe) {
    return {"schtasks", "/Create", "/TN", AUTOSTART_TASK, "/TR", "\"" + exe + "\"", "/SC", "ONLOGON",
            "/RL", "HIGHEST", "/F"};
}

std::string windows_autostart_task_xml(const std::string& exe, const std::string& user) {
    std::string user_xml = user.empty() ? "" : "<UserId>" + xml_escape(user) + "</UserId>";
    return std::string("<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n") +
        "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
        "  <RegistrationInfo><Description>Hard Drive Monitor tray app at logon. Turn off in the app's Settings.</Description></RegistrationInfo>\n"
        "  <Triggers><LogonTrigger><Enabled>true</Enabled>" + user_xml + "</LogonTrigger></Triggers>\n"
        "  <Principals><Principal id=\"Author\">" + user_xml +
        "<LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>\n"
        "  <Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
        "<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
        "<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Enabled>true</Enabled></Settings>\n"
        "  <Actions Context=\"Author\"><Exec><Command>" + xml_escape(exe) + "</Command></Exec></Actions>\n"
        "</Task>\n";
}

std::string windows_run_value(const std::string& exe) { return "\"" + exe + "\""; }

std::string autostart_path(OS os) {
    switch (os) {
        case OS::MacOS:
            return path_join(path_join(path_join(home_dir(), "Library"), "LaunchAgents"), std::string(AUTOSTART_LABEL) + ".plist");
        case OS::Linux: {
            std::string b = get_env("XDG_CONFIG_HOME");
            if (b.empty()) b = path_join(home_dir(), ".config");
            return path_join(path_join(b, "autostart"), "hard-drive-monitor.desktop");
        }
        default:
            return "";
    }
}

#ifdef _WIN32
static const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
static bool run_value_exists() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    std::wstring name = wide(AUTOSTART_TASK);
    bool ok = RegQueryValueExW(k, name.c_str(), nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}
static bool set_run_value(bool enable, const std::string& exe) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    std::wstring name = wide(AUTOSTART_TASK);
    LONG rc;
    if (enable) {
        std::wstring v = wide(windows_run_value(exe));
        rc = RegSetValueExW(k, name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                            static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
    } else {
        rc = RegDeleteValueW(k, name.c_str());
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    RegCloseKey(k);
    return rc == ERROR_SUCCESS;
}
static bool task_exists() {
    int e = osn::task_exists(AUTOSTART_TASK);   // native COM; schtasks only if COM could not answer
    if (e >= 0) return e == 1;
    return run_cmd({"schtasks", "/Query", "/TN", AUTOSTART_TASK}, 30).rc == 0;
}
#endif

bool autostart_enabled() {
#ifdef _WIN32
    return task_exists() || run_value_exists();
#else
    return file_exists(autostart_path(current_os()));
#endif
}

bool set_autostart(bool enable, const std::string& exe, std::string* err) {
#ifdef _WIN32
    // An app that requires elevation is skipped when started from the Run key at logon, so an elevated
    // GUI registers a logon task with highest privileges; a non-elevated one falls back to the Run key.
    if (!enable) {
        if (task_exists()) {
            std::string cerr;
            if (osn::task_delete(AUTOSTART_TASK, cerr)) { set_run_value(false, exe); return true; }
            auto r = run_cmd({"schtasks", "/Delete", "/TN", AUTOSTART_TASK, "/F"}, 30);
            if (r.rc != 0) { if (err) *err = trim(r.err.empty() ? r.out : r.err); return false; }
        }
        if (!set_run_value(false, exe)) { if (err) *err = "cannot change the Run registry key"; return false; }
        return true;
    }
    if (is_windows_admin()) {
        std::string uname = get_env("USERNAME"), dom = get_env("USERDOMAIN"), cerr;
        std::string user = (!uname.empty() && !dom.empty()) ? dom + "\\" + uname : uname;
        if (osn::task_register_xml(AUTOSTART_TASK, windows_autostart_task_xml(exe, user), cerr)) { set_run_value(false, exe); return true; }
        auto r = run_cmd(windows_autostart_task_args(exe), 30);
        if (r.rc == 0) { set_run_value(false, exe); return true; }
    }
    if (!set_run_value(true, exe)) { if (err) *err = "cannot write the Run registry key"; return false; }
    return true;
#else
    std::string p = autostart_path(current_os());
    if (!enable) {
        std::error_code ec;
        fs::remove(p, ec);
        if (ec && file_exists(p)) { if (err) *err = "cannot remove " + p + ": " + ec.message(); return false; }
        return true;
    }
    make_dirs(dirname(p));
    std::string body = current_os() == OS::MacOS ? launch_agent_plist({exe})
                                                 : desktop_entry({exe}, "hard-drive-monitor", true);
    if (!write_file(p, body)) { if (err) *err = "cannot write " + p; return false; }
    return true;
#endif
}

}  // namespace hdmon
