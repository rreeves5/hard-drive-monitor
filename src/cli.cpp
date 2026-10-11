#include "cli.hpp"

#include <chrono>
#include <ctime>
#include <map>
#include <set>
#include <sstream>
#include <thread>

#include "alerts.hpp"
#include "collect.hpp"
#include "config.hpp"
#include "render.hpp"
#include "service.hpp"
#include "update.hpp"

namespace hdmon {

const char* VERSION = HDMON_VERSION;

namespace {

struct Args {
    std::string cmd;
    std::map<std::string, std::string> opt;     // --key value
    std::set<std::string> flag;                 // --flag
    std::vector<std::string> from_json;
    std::vector<std::string> pos;
    bool has(const std::string& f) const { return flag.count(f) > 0; }
    std::string get(const std::string& k, const std::string& d = "") const {
        auto it = opt.find(k);
        return it == opt.end() ? d : it->second;
    }
};

struct CmdSpec {
    std::string help;
    std::set<std::string> flags, values;
    std::vector<std::string> positionals;   // names; trailing "?" = optional
    std::string usage_extra;
};

const std::set<std::string> COMMON_V = {"--color", "--config", "--db"};

std::map<std::string, CmdSpec> specs() {
    std::set<std::string> src = {"--from-json"}, rec = {"--no-record", "--record"};
    auto U = [](std::set<std::string> a, const std::set<std::string>& b) { a.insert(b.begin(), b.end()); return a; };
    std::map<std::string, CmdSpec> s;
    s["status"] = {"colored table of all drives", U({"--json"}, rec), U(COMMON_V, src), {}, ""};
    s["detail"] = {"full attribute view of one drive", {"--json"}, U(U(COMMON_V, src), {"--type"}), {"device"},
                   "  device         e.g. /dev/sda, sda, nvme0, disk0, D: or a serial number\n"
                   "  -d, --type T   smartctl -d device type (e.g. sat, nvme) if auto-detect fails\n"};
    s["history"] = {"trends from the local SQLite log", {"--json"}, U(COMMON_V, {"--limit", "--drive"}), {},
                    "  --limit N      readings per drive (default 30)\n  --drive D      only this device/serial\n"};
    s["check"] = {"one scheduled-style run: record, alert on worsening, exit 0/1/2 = OK/WARNING/CRITICAL",
                  U({"--json", "--quiet", "--no-alert"}, rec), U(COMMON_V, src), {},
                  "  --quiet        print only alerts\n  --no-alert     record but do not notify\n"};
    s["watch"] = {"loop: check every --interval", U({"--json", "--quiet", "--no-alert"}, rec),
                  U(U(COMMON_V, src), {"--interval", "--count"}), {},
                  "  --interval I   e.g. 1h, 30m, 2h30m (default 1h)\n  --count N      stop after N runs (default: forever)\n"};
    s["selftest"] = {"start a SMART self-test or show results", {"--json", "--results"}, U(U(COMMON_V, src), {"--type"}),
                     {"device", "kind?"},
                     "  kind           short (default) or long\n  --results      show the self-test log instead of starting a test\n"
                     "  -d, --type T   smartctl -d device type\n"};
    s["install-service"] = {"schedule hourly checks (systemd user timer / launchd / Task Scheduler)",
                            {"--json", "--system", "--no-enable", "--dry-run", "--highest"}, U(COMMON_V, {"--interval"}), {},
                            "  --interval I   default 1h\n  --no-enable    write the files but do not enable them\n"
                            "  --dry-run      print, write nothing\n  --highest      Windows: task runs with highest privileges\n"
                            "  --system       root/system-wide: print the files and commands only, never installed silently\n"};
    s["uninstall-service"] = {"remove the scheduled check", {"--json", "--system"}, COMMON_V, {},
                              "  --system       print the commands to remove the root/system-wide job\n"};
    s["init-config"] = {"write a sample config.toml", {"--json", "--force"}, COMMON_V, {}, "  --force        overwrite\n"};
    s["alert-test"] = {"send a test desktop notification (and e-mail if enabled)", {"--json"}, COMMON_V, {}, ""};
    s["update"] = {"check GitHub for a newer release and install it (only when you ask)",
                   {"--json", "--check", "--yes", "--enable-auto-check", "--disable-auto-check"}, COMMON_V, {},
                   "  --check        only report whether a newer release exists (exit 0 = up to date, 1 = update available)\n"
                   "  -y, --yes      install without asking for confirmation\n"
                   "  --enable-auto-check   set [update] auto_check = true: the scheduled check looks once a day and notifies\n"
                   "  --disable-auto-check  set [update] auto_check = false and auto_install = false (the default)\n"};
    return s;
}

const char* ORDER[] = {"status", "detail", "history", "check", "watch", "selftest", "install-service",
                       "uninstall-service", "init-config", "alert-test", "update"};

std::string main_usage() {
    std::ostringstream o;
    o << "usage: hdmon [-h] [--version] COMMAND ...\n\n"
         "Hard Drive Monitor - SMART drive health monitor (smartmontools based) for Linux, macOS and Windows.\n"
         "Early warning only - keep backups.\n\ncommands:\n";
    auto s = specs();
    for (auto c : ORDER) o << "  " << ljust(c, 19) << s[c].help << "\n";
    o << "\ncommon options: --json  --color {auto,always,never}  --config FILE  --db FILE\n"
         "                --from-json FILE_OR_DIR (repeatable; status/detail/check/watch/selftest)\n"
         "                --no-record / --record (status/check/watch)\n\n"
         "Run `hdmon COMMAND -h` for command help.\n";
    return o.str();
}

std::string cmd_usage(const std::string& name, const CmdSpec& sp) {
    std::ostringstream o;
    o << "usage: hdmon " << name;
    for (auto& p : sp.positionals) o << (p.back() == '?' ? " [" + p.substr(0, p.size() - 1) + "]" : " " + p);
    o << " [options]\n\n" << sp.help << "\n\noptions:\n" << sp.usage_extra;
    o << "  --json         machine-readable JSON output\n  --color C      auto | always | never\n"
         "  --config FILE  config file (default " << config_path() << ")\n"
         "  --db FILE      history database (default " << default_db() << ")\n";
    if (sp.values.count("--from-json")) o << "  --from-json P  read saved `smartctl -x --json` output instead of live drives (repeatable)\n";
    if (sp.flags.count("--no-record")) o << "  --no-record    do not write this run to the history DB\n"
                                            "  --record       with --from-json: also record to history\n";
    return o.str();
}

struct UsageError : std::runtime_error { using std::runtime_error::runtime_error; };
struct HelpRequested { std::string text; };

Args parse(const std::vector<std::string>& argv) {
    Args a;
    auto all = specs();
    size_t i = 0;
    if (argv.empty()) throw UsageError("the following arguments are required: COMMAND");
    if (argv[0] == "-h" || argv[0] == "--help") throw HelpRequested{main_usage()};
    if (argv[0] == "--version") throw HelpRequested{std::string("Hard Drive Monitor (hdmon) ") + VERSION + "\n"};
    if (argv[0] == "serve") throw UsageError("`serve` (web dashboard) is not available in the native build; use `hdmon status` or `--json`");
    auto it = all.find(argv[0]);
    if (it == all.end()) throw UsageError("invalid choice: '" + argv[0] + "'");
    a.cmd = argv[0];
    const CmdSpec& sp = it->second;
    for (i = 1; i < argv.size(); ++i) {
        std::string t = argv[i];
        if (t == "-h" || t == "--help") throw HelpRequested{cmd_usage(a.cmd, sp)};
        if (t == "-d") t = "--type";
        if (t == "-y") t = "--yes";
        std::string val;
        bool has_val = false;
        if (starts_with(t, "--") && t.find('=') != std::string::npos) {
            val = t.substr(t.find('=') + 1);
            t = t.substr(0, t.find('='));
            has_val = true;
        }
        if (sp.flags.count(t)) {
            if (has_val) throw UsageError("argument " + t + ": ignored explicit argument '" + val + "'");
            a.flag.insert(t);
        } else if (sp.values.count(t)) {
            if (!has_val) {
                if (i + 1 >= argv.size()) throw UsageError("argument " + t + ": expected one argument");
                val = argv[++i];
            }
            if (t == "--from-json") a.from_json.push_back(val);
            else a.opt[t] = val;
        } else if (starts_with(t, "-") && t.size() > 1) {
            throw UsageError("unrecognized arguments: " + argv[i]);
        } else {
            a.pos.push_back(t);
        }
    }
    size_t required = 0;
    for (auto& p : sp.positionals) if (p.back() != '?') ++required;
    if (a.pos.size() < required) throw UsageError("the following arguments are required: " + sp.positionals[a.pos.size()]);
    if (a.pos.size() > sp.positionals.size()) throw UsageError("unrecognized arguments: " + a.pos.back());
    std::string color = a.get("--color", "auto");
    if (color != "auto" && color != "always" && color != "never")
        throw UsageError("argument --color: invalid choice: '" + color + "' (choose from 'auto', 'always', 'never')");
    if (a.cmd == "selftest" && a.pos.size() > 1 && a.pos[1] != "short" && a.pos[1] != "long")
        throw UsageError("argument kind: invalid choice: '" + a.pos[1] + "' (choose from 'short', 'long')");
    return a;
}

int to_int(const std::string& s, const std::string& name) {
    try {
        size_t p = 0;
        int v = std::stoi(s, &p);
        if (p == s.size()) return v;
    } catch (...) {}
    throw UsageError("argument " + name + ": invalid int value: '" + s + "'");
}

std::string dump(const json& j) { return j.dump(2, ' ', true, json::error_handler_t::replace); }

const std::map<std::string, int> EXIT_CODE = {{"OK", 0}, {"UNKNOWN", 0}, {"WARNING", 1}, {"CRITICAL", 2}};

struct Ctx {
    Args& a;
    json cfg;
    Painter paint;
    std::ostream& out;
    std::ostream& err;
    bool jsonl = false;
};

json results_json(const std::vector<Result>& results, const Changes& changes, const json& meta) {
    json j = json::object();
    j["version"] = VERSION;
    j["generated"] = local_now_iso();
    j["meta"] = meta;
    json drives = json::array();
    for (auto& [d, a] : results) {
        auto it = changes.find(d.key());
        drives.push_back({{"drive", d.to_json()}, {"assessment", a.to_json()},
                          {"changes", it == changes.end() ? json::array() : it->second}});
    }
    j["drives"] = drives;
    return j;
}

bool should_record(const Args& a) {
    if (a.has("--no-record")) return false;
    return a.from_json.empty() || a.has("--record");
}

std::vector<Result> gather(Ctx& c, json& meta, Changes& changes) {
    auto results = collect(c.cfg, c.a.from_json, meta);
    if (should_record(c.a)) {
        History h(c.a.get("--db", default_db()));
        changes = h.record(results, jstr(meta, "source").value_or("live"));
    }
    return results;
}

int cmd_status(Ctx& c) {
    json meta;
    Changes ch;
    auto results = gather(c, meta, ch);
    c.out << (c.a.has("--json") ? dump(results_json(results, ch, meta)) : status_view(results, c.paint, meta, ch)) << "\n";
    return 0;
}

int cmd_detail(Ctx& c) {
    json meta;
    auto results = collect(c.cfg, c.a.from_json, meta);
    std::string dev = c.a.pos[0];
    auto hit = find_drive(results, dev);
    if (!hit && c.a.from_json.empty()) {
        auto path = find_smartctl(jstr(jget(c.cfg, "smartctl"), "path").value_or(""));
        if (path) {   // not in the scan (e.g. USB bridge): try it directly
            json sudo = jget(jget(c.cfg, "smartctl"), "sudo");
            Smartctl sc(*path, sudo.is_null() ? json("auto") : sudo);
            DeviceRef ref;
            ref.name = dev;
            if (c.a.opt.count("--type")) ref.dev_type = c.a.get("--type");
            Drive d = read_drive(sc, ref);
            hit = Result(d, assess(d, jget(c.cfg, "thresholds")));
        }
    }
    if (!hit) {
        std::vector<std::string> known;
        for (auto& r : results) known.push_back(r.first.device);
        c.err << "hdmon: no drive matching '" << dev << "'. Known: " << join(known, ", ") << "\n";
        return 3;
    }
    auto& [d, a] = *hit;
    if (c.a.has("--json")) c.out << dump({{"drive", d.to_json()}, {"assessment", a.to_json()}, {"meta", meta}}) << "\n";
    else c.out << detail_view(d, a, c.paint) << "\n";
    return 0;
}

int cmd_history(Ctx& c) {
    std::string path = c.a.get("--db", default_db());
    json summary = json::array();
    if (c.a.opt.count("--db") || file_exists(path)) {
        History h(path);
        summary = h.summary(to_int(c.a.get("--limit", "30"), "--limit"), c.a.get("--drive"));
    }
    if (c.a.has("--json")) c.out << dump({{"db", path}, {"drives", summary}}) << "\n";
    else c.out << "History: " << path << "\n\n" << history_view(summary, c.paint) << "\n";
    return 0;
}

int cmd_check(Ctx& c) {
    json meta;
    Changes ch;
    auto results = gather(c, meta, ch);
    auto alerts = build_alerts(results, ch);
    std::vector<std::string> sent = c.a.has("--no-alert") ? std::vector<std::string>{} : dispatch(c.cfg, alerts);
    // Opt-in only ([update] auto_check, default false): at most one release check per day; notify, never auto-run.
    json upd = scheduled_update_check(c.cfg, c.a.get("--db", default_db()), !c.a.has("--no-alert"),
                                      static_cast<int64_t>(std::time(nullptr)));
    std::string worst = "OK";
    bool first = true;
    for (auto& r : results) {   // max() by rank; first wins on ties, like Python
        if (first || status_rank(r.second.status) > status_rank(worst)) worst = r.second.status;
        first = false;
    }
    if (c.a.has("--json")) {
        json j = results_json(results, ch, meta);
        json al = json::array();
        for (auto& a : alerts) al.push_back({{"severity", a.severity}, {"title", a.title}, {"body", a.body}});
        j["alerts"] = al;
        j["alert_log"] = sent;
        j["worst"] = worst;
        if (!upd.is_null()) j["update"] = upd;
        c.out << (c.jsonl ? j.dump(-1, ' ', true, json::error_handler_t::replace) : dump(j)) << "\n";
    } else if (!c.a.has("--quiet")) {
        c.out << "[" << local_now_hms() << "] " << results.size() << " drive(s), worst status " << worst << ", "
              << alerts.size() << " new alert(s)\n";
        for (auto& al : alerts) c.out << "  ALERT " << al.title << "\n    " << replace_all(al.body, "\n", "\n    ") << "\n";
        for (auto& line : sent) c.out << "  " << line << "\n";
        if (truthy(jget(upd, "checked"))) {
            if (!truthy(jget(upd, "ok"))) c.out << "  update check failed: " << to_str(jget(upd, "error")) << "\n";
            else if (upd.contains("installed")) c.out << "  UPDATE " << to_str(jget(upd, "install_message")) << "\n";
            else if (truthy(jget(upd, "update_available")))
                c.out << "  UPDATE " << to_str(jget(upd, "latest")) << " is available: " << to_str(jget(upd, "url"))
                      << " (install with: hdmon update)\n";
        }
    } else {
        for (auto& al : alerts) c.out << "ALERT " << al.title << ": " << replace_all(al.body, "\n", " | ") << "\n";
        if (truthy(jget(upd, "update_available")))
            c.out << "UPDATE " << to_str(jget(upd, "latest")) << " is available (hdmon update)\n";
    }
    c.out.flush();
    return EXIT_CODE.at(worst);
}

int cmd_watch(Ctx& c) {
    int64_t interval;
    try { interval = parse_interval(c.a.get("--interval", "1h")); }
    catch (const std::invalid_argument& e) { throw UsageError(e.what()); }
    int count = to_int(c.a.get("--count", "0"), "--count");
    c.jsonl = true;
    int n = 0, rc = 0;
    while (true) {
        rc = cmd_check(c);
        if (count && ++n >= count) break;
        std::this_thread::sleep_for(std::chrono::seconds(interval));
    }
    return rc;
}

int print_selftest(Ctx& c, const Drive& d) {
    if (c.a.has("--json")) {
        c.out << dump({{"device", d.device}, {"current", d.self_test}, {"log", d.self_test_log}}) << "\n";
        return 0;
    }
    c.out << selftest_view(d, c.paint) << "\n";
    return 0;
}

int cmd_selftest(Ctx& c) {
    std::string dev = c.a.pos[0];
    std::string kind = c.a.pos.size() > 1 ? c.a.pos[1] : "short";
    bool results_only = c.a.has("--results");
    if (!c.a.from_json.empty()) {
        if (!results_only) { c.err << "hdmon: --from-json can only show --results, not start a test\n"; return 3; }
        json meta;
        auto results = collect(c.cfg, c.a.from_json, meta);
        auto hit = find_drive(results, dev);
        if (!hit) { c.err << "hdmon: no drive matching '" << dev << "'\n"; return 3; }
        return print_selftest(c, hit->first);
    }
    auto path = find_smartctl(jstr(jget(c.cfg, "smartctl"), "path").value_or(""));
    if (!path) {
        c.err << "smartctl (smartmontools) is needed for self-tests. Install it with:\n  " << install_hint() << "\n";
        return 3;
    }
    json sudo = jget(jget(c.cfg, "smartctl"), "sudo");
    Smartctl sc(*path, sudo.is_null() ? json("auto") : sudo);
    std::optional<DeviceRef> ref;
    for (auto& r : discover(&sc))
        if (dev == r.name || dev == r.name.substr(r.name.rfind('/') + 1)) { ref = r; break; }
    if (!ref) {
        ref = DeviceRef();
        ref->name = dev;
        if (c.a.opt.count("--type")) ref->dev_type = c.a.get("--type");
    }
    Drive d = read_drive(sc, *ref);
    if (results_only) return print_selftest(c, d);
    if (!d.smart_available) {
        c.err << "hdmon: cannot read SMART on " << ref->name << ": " << join(d.notes, "; ") << "\n";
        return 3;
    }
    const json& st = d.self_test;
    if (truthy(jget(st, "in_progress"))) {
        std::string msg = "A self-test is already running on " + ref->name + " (" + to_str(jget(st, "remaining_percent")) +
                          "% remaining). Abort it with: smartctl -X " + ref->name;
        c.out << (c.a.has("--json") ? dump({{"started", false}, {"reason", msg}}) : msg) << "\n";
        return 1;
    }
    auto minutes = jint(st, kind == "short" ? "short_minutes" : "extended_minutes");
    std::string expected;
    if (minutes && *minutes) {
        expected = "about " + std::to_string(*minutes) + " minute(s)" +
                   (*minutes >= 90 ? " (~" + fmt_fixed(static_cast<double>(*minutes) / 60.0, 1) + " h)" : "");
    } else if (d.kind == "NVMe" && kind == "short") {
        expected = "up to 2 minutes (NVMe spec limit)";
    } else {
        expected = "unknown - the drive did not report it (NVMe extended tests are typically 10-90 min)";
    }
    std::vector<std::string> cmd = {"-t", kind, "--json"};
    if (ref->dev_type) { cmd.push_back("-d"); cmd.push_back(*ref->dev_type); }
    cmd.push_back(ref->name);
    json data;
    std::string errs;
    int rc = sc.run(cmd, data, errs, 60);
    bool ok = !(rc & 0x07);
    std::string info = messages_text(data);
    if (info.empty()) info = trim(errs);
    if (c.a.has("--json")) {
        c.out << dump({{"started", ok}, {"device", ref->name}, {"kind", kind}, {"expected_duration", expected},
                       {"expected_minutes", minutes ? json(*minutes) : json()}, {"smartctl_exit", rc}, {"message", info}}) << "\n";
    } else if (ok) {
        c.out << "Started " << kind << " self-test on " << ref->name << " (" << d.model.value_or("") << "). Expected duration: "
              << expected << ".\nThe drive stays usable meanwhile (it may be slower). Check later with:\n  hdmon selftest "
              << dev << " --results\n";
    } else {
        c.err << "Self-test did not start on " << ref->name << " (smartctl exit " << rc << "): " << info << "\n";
    }
    return ok ? 0 : 1;
}

int cmd_install(Ctx& c) {
    int64_t interval;
    try { interval = parse_interval(c.a.get("--interval", "1h")); }
    catch (const std::invalid_argument& e) { throw UsageError(e.what()); }
    std::vector<std::string> lines;
    bool js = c.a.has("--json");
    Out o = [&](const std::string& s) { if (js) lines.push_back(s); else c.out << s << "\n" << std::flush; };
    int rc = install_service(interval, !c.a.has("--no-enable"), c.a.has("--system"), c.a.has("--highest"), c.a.has("--dry-run"), o);
    if (js) c.out << dump({{"ok", rc == 0}, {"output", lines}}) << "\n";
    return rc;
}

int cmd_uninstall(Ctx& c) {
    std::vector<std::string> lines;
    bool js = c.a.has("--json");
    Out o = [&](const std::string& s) { if (js) lines.push_back(s); else c.out << s << "\n" << std::flush; };
    int rc = uninstall_service(c.a.has("--system"), o);
    if (js) c.out << dump({{"ok", rc == 0}, {"output", lines}}) << "\n";
    return rc;
}

int cmd_init_config(Ctx& c) {
    std::string e;
    std::string p = write_sample_config(c.a.get("--config"), c.a.has("--force"), &e);
    bool js = c.a.has("--json");
    if (p.empty()) {
        c.out << (js ? dump({{"ok", false}, {"error", e}}) : e) << "\n";
        return 1;
    }
    c.out << (js ? dump({{"ok", true}, {"path", p}}) : "Wrote " + p) << "\n";
    return 0;
}

int cmd_alert_test(Ctx& c) {
    std::string title = "Hard Drive Monitor test alert", body = "If you can read this, Hard Drive Monitor alerts work.";
    auto [ok, info] = notify_desktop(title, body);
    json res = {{"desktop", {{"ok", ok}, {"info", info}}}};
    if (truthy(jget(jget(c.cfg, "email"), "enabled"))) {
        auto [ok2, info2] = send_email(c.cfg, title, body);
        res["email"] = {{"ok", ok2}, {"info", info2}};
    }
    if (c.a.has("--json")) c.out << dump(res) << "\n";
    else {
        std::vector<std::string> L;
        for (auto it = res.begin(); it != res.end(); ++it)
            L.push_back(it.key() + ": " + (it.value()["ok"].get<bool>() ? "ok" : "FAILED") + " (" + it.value()["info"].get<std::string>() + ")");
        c.out << join(L, "\n") << "\n";
    }
    return 0;
}

int cmd_update(Ctx& c) {
    bool js = c.a.has("--json");
    if (c.a.has("--enable-auto-check") || c.a.has("--disable-auto-check")) {
        if (c.a.has("--enable-auto-check") && c.a.has("--disable-auto-check"))
            throw UsageError("--enable-auto-check and --disable-auto-check are mutually exclusive");
        bool on = c.a.has("--enable-auto-check");
        std::string path = c.a.get("--config", config_path()), e;
        bool ok = set_config_value(path, "update", "auto_check", on ? "true" : "false", &e);
        if (ok && !on) ok = set_config_value(path, "update", "auto_install", "false", &e);
        if (js) c.out << dump({{"ok", ok}, {"config", path}, {"auto_check", on}, {"error", ok ? json() : json(e)}}) << "\n";
        else if (ok) c.out << (on ? "Automatic update checks ON (once a day, notify only; nothing is installed unless "
                                    "auto_install = true): "
                                  : "Automatic update checks and installs OFF: ") << path << "\n";
        else c.err << "hdmon: " << e << "\n";
        return ok ? 0 : 3;
    }
    ReleaseInfo rel;
    CheckResult r = check_for_update(VERSION, &rel);
    if (!r.ok) {
        if (js) c.out << dump(r.to_json()) << "\n";
        else c.err << "hdmon: update check failed: " << r.error << "\n";
        return 3;
    }
    std::string ahead = compare_versions(VERSION, r.latest_tag) > 0 ? "newer than the latest release, " : "latest release: ";
    if (c.a.has("--check")) {
        if (js) c.out << dump(r.to_json()) << "\n";
        else if (r.update_available)
            c.out << "Update available: " << r.latest << " (you have " << VERSION << ")\n  " << r.url
                  << "\nInstall it with: hdmon update\n";
        else c.out << "Up to date: " << VERSION << " (" << ahead << r.latest << ")\n";
        return r.update_available ? 1 : 0;
    }
    if (!r.update_available) {
        if (js) c.out << dump({{"check", r.to_json()}, {"installed", false}, {"message", "up to date"}}) << "\n";
        else c.out << "Up to date: " << VERSION << " (" << ahead << r.latest << ")\n";
        return 0;
    }
    if (!c.a.has("--yes")) {
        if (js) throw UsageError("installing with --json needs --yes (no interactive prompt in JSON mode)");
        c.out << "Update available: " << r.latest << " (you have " << VERSION << ")\n  " << r.url << "\n" << std::flush;
        auto& H = update_hooks();
        if (!H.confirm || !H.confirm("Download, verify and install " + r.latest + " now?")) {
            c.out << "Cancelled. Nothing was changed.\n";
            return 1;
        }
    }
    InstallResult ir = install_update(rel);
    if (js) {
        c.out << dump({{"check", r.to_json()}, {"installed", ir.ok}, {"message", ir.message}, {"log", ir.log}}) << "\n";
    } else {
        for (auto& l : ir.log) c.out << "  " << l << "\n";
        (ir.ok ? c.out : c.err) << (ir.ok ? "" : "hdmon: update failed: ") << ir.message << "\n";
    }
    return ir.ok ? 0 : 3;
}

}  // namespace

int run_cli(const std::vector<std::string>& argv, std::ostream& out, std::ostream& err) {
    Args a;
    try {
        a = parse(argv);
    } catch (const HelpRequested& h) {
        out << h.text;
        return 0;
    } catch (const UsageError& e) {
        err << "usage: hdmon [-h] [--version] COMMAND ...\nhdmon: error: " << e.what() << "\n";
        return 2;
    }
    cleanup_old_binaries();   // Windows: hdmon.old.exe left behind by a previous `hdmon update`
    json cfg;
    try {
        cfg = load_config(a.get("--config"));
    } catch (const std::exception& e) {
        err << "hdmon: cannot read config: " << e.what() << "\n";
        return 3;
    }
    Ctx c{a, cfg, Painter(a.has("--json") ? "never" : a.get("--color", "auto")), out, err};
    try {
        const std::string& k = a.cmd;
        if (k == "status") return cmd_status(c);
        if (k == "detail") return cmd_detail(c);
        if (k == "history") return cmd_history(c);
        if (k == "check") return cmd_check(c);
        if (k == "watch") return cmd_watch(c);
        if (k == "selftest") return cmd_selftest(c);
        if (k == "install-service") return cmd_install(c);
        if (k == "uninstall-service") return cmd_uninstall(c);
        if (k == "init-config") return cmd_init_config(c);
        if (k == "alert-test") return cmd_alert_test(c);
        if (k == "update") return cmd_update(c);
        return 2;
    } catch (const SmartctlMissing& e) {
        if (a.has("--json")) out << dump({{"error", e.what()}, {"install", install_hint()}}) << "\n";
        else err << "hdmon: " << e.what() << "\n";
        return 3;
    } catch (const UsageError& e) {
        err << "usage: hdmon " << a.cmd << " [options]\nhdmon: error: " << e.what() << "\n";
        return 2;
    } catch (const std::exception& e) {
        err << "hdmon: error: " << e.what() << "\n";
        return 3;
    }
}

}  // namespace hdmon
