#include "history.hpp"

#include <stdexcept>

#include "raid.hpp"
#include "sqlite3.h"

namespace hdmon {

static const char* SCHEMA = R"SQL(
CREATE TABLE IF NOT EXISTS runs(
  id INTEGER PRIMARY KEY, ts TEXT NOT NULL, source TEXT);
CREATE TABLE IF NOT EXISTS readings(
  id INTEGER PRIMARY KEY, run_id INTEGER NOT NULL REFERENCES runs(id),
  drive_key TEXT NOT NULL, device TEXT, model TEXT, serial TEXT,
  status TEXT, health INTEGER, performance INTEGER, temperature INTEGER,
  power_on_hours INTEGER, metrics TEXT);
CREATE INDEX IF NOT EXISTS ix_readings_drive ON readings(drive_key, run_id);
CREATE TABLE IF NOT EXISTS array_readings(
  id INTEGER PRIMARY KEY, run_id INTEGER NOT NULL REFERENCES runs(id),
  array_id TEXT NOT NULL, name TEXT, backend TEXT, level TEXT, state TEXT, status TEXT,
  rebuild_pct REAL, members TEXT);
CREATE INDEX IF NOT EXISTS ix_array_readings ON array_readings(array_id, run_id);
)SQL";

// Counters that must never go up on a healthy drive. An increase triggers an alert.
static const char* ALERT_COUNTERS[] = {"reallocated_sectors", "pending_sectors", "offline_uncorrectable",
                                       "reported_uncorrectable", "command_timeout", "udma_crc_errors",
                                       "media_errors", "uncorrected_errors"};
// Tracked and shown as trends; they rise normally, so they do not alert by themselves.
static const char* TREND_METRICS[] = {"wear_percent_used", "unsafe_shutdowns", "error_log_entries"};

json compare_readings(const json& prev, const std::string& status, std::optional<int> health, const json& metrics) {
    json out = json::array();
    if (!prev.is_object() || prev.empty()) return out;
    const json& pm = jget(prev, "metrics");
    std::string pstatus = jstr(prev, "status").value_or("OK");
    if (status_rank(status) > status_rank(pstatus))
        out.push_back({{"kind", "status"}, {"metric", "status"}, {"old", jget(prev, "status")}, {"new", status}, {"alert", true}});
    for (auto k : ALERT_COUNTERS) {
        auto oldv = opt_int(jget(pm, k)), newv = opt_int(jget(metrics, k));
        if (newv && *newv > oldv.value_or(0))
            out.push_back({{"kind", "counter"}, {"metric", k}, {"old", oldv.value_or(0)}, {"new", *newv}, {"alert", true}});
    }
    auto ph = jint(prev, "health");
    if (health && ph && *health < *ph)
        out.push_back({{"kind", "health"}, {"metric", "health"}, {"old", *ph}, {"new", *health}, {"alert", false}});
    for (auto k : TREND_METRICS) {
        auto oldv = opt_int(jget(pm, k)), newv = opt_int(jget(metrics, k));
        if (newv && oldv && *newv > *oldv)
            out.push_back({{"kind", "trend"}, {"metric", k}, {"old", *oldv}, {"new", *newv}, {"alert", false}});
    }
    return out;
}

json compare_array_states(const json& prev, const std::string& state) {
    json out = json::array();
    bool first = !prev.is_object() || prev.empty();
    std::string p = first ? "" : jstr(prev, "state").value_or("UNKNOWN");
    auto add = [&](const char* event, bool alert) {
        out.push_back({{"kind", "array_state"}, {"metric", "array state"}, {"old", first ? json(nullptr) : json(p)},
                       {"new", state}, {"event", event}, {"alert", alert}});
    };
    if (state == p) return out;
    if (state == "FAILED") add("failed", true);
    else if (state == "DEGRADED") add("degraded", p != "FAILED");
    else if (state == "REBUILDING" && first) add("rebuilding", true);   // first sight mid-rebuild: still running without redundancy
    else if (state == "REBUILDING" && (p == "OK" || p == "UNKNOWN")) add("rebuild_started", true);
    else if (state == "REBUILDING" && !first) add("rebuilding", false);
    else if (state == "OK" && (p == "REBUILDING" || p == "DEGRADED" || p == "FAILED")) add("rebuild_complete", true);
    return out;
}

History::History(const std::string& path) {
    make_dirs(dirname(path).empty() ? "." : dirname(path));
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        std::string e = db_ ? sqlite3_errmsg(db_) : "out of memory";
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("cannot open history database " + path + ": " + e);
    }
    sqlite3_busy_timeout(db_, 5000);
    exec(SCHEMA);
}
History::~History() { if (db_) sqlite3_close(db_); }

void History::exec(const char* sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string e = err ? err : "?";
        sqlite3_free(err);
        throw std::runtime_error("sqlite: " + e);
    }
}

namespace {
struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) throw std::runtime_error(std::string("sqlite: ") + sqlite3_errmsg(db));
    }
    ~Stmt() { sqlite3_finalize(s); }
    void bind(int i, const std::string& v) { sqlite3_bind_text(s, i, v.c_str(), -1, SQLITE_TRANSIENT); }
    void bind(int i, const std::optional<std::string>& v) { if (v) bind(i, *v); else sqlite3_bind_null(s, i); }
    void bind(int i, std::optional<int64_t> v) { if (v) sqlite3_bind_int64(s, i, *v); else sqlite3_bind_null(s, i); }
    void bind(int i, int64_t v) { sqlite3_bind_int64(s, i, v); }
    bool step() { int rc = sqlite3_step(s); if (rc == SQLITE_ROW) return true; if (rc == SQLITE_DONE) return false; throw std::runtime_error("sqlite step failed"); }
    json row() const {
        json r = json::object();
        int n = sqlite3_column_count(s);
        for (int i = 0; i < n; ++i) {
            const char* name = sqlite3_column_name(s, i);
            switch (sqlite3_column_type(s, i)) {
                case SQLITE_INTEGER: r[name] = static_cast<int64_t>(sqlite3_column_int64(s, i)); break;
                case SQLITE_FLOAT: r[name] = sqlite3_column_double(s, i); break;
                case SQLITE_NULL: r[name] = nullptr; break;
                default: r[name] = std::string(reinterpret_cast<const char*>(sqlite3_column_text(s, i)));
            }
        }
        return r;
    }
};
json fix_metrics(json r) {
    if (r.contains("metrics")) {
        json m = r["metrics"].is_string() ? json::parse(r["metrics"].get<std::string>(), nullptr, false) : json::object();
        r["metrics"] = m.is_discarded() ? json::object() : m;
    }
    return r;
}
}  // namespace

json History::last_reading(const std::string& key) {
    Stmt st(db_, "SELECT r.*, runs.ts FROM readings r JOIN runs ON runs.id = r.run_id "
                 "WHERE drive_key = ? ORDER BY r.run_id DESC LIMIT 1");
    st.bind(1, key);
    if (st.step()) return fix_metrics(st.row());
    return json::object();
}

json History::last_array_reading(const std::string& id) {
    Stmt st(db_, "SELECT a.*, runs.ts FROM array_readings a JOIN runs ON runs.id = a.run_id "
                 "WHERE array_id = ? ORDER BY a.run_id DESC LIMIT 1");
    st.bind(1, id);
    if (st.step()) return st.row();
    return json::object();
}

json History::array_readings(const std::string& id, int limit) {
    Stmt st(db_, "SELECT a.*, runs.ts FROM array_readings a JOIN runs ON runs.id = a.run_id WHERE array_id = ? "
                 "ORDER BY a.run_id DESC LIMIT ?");
    st.bind(1, id); st.bind(2, static_cast<int64_t>(limit));
    std::vector<json> rows;
    while (st.step()) rows.push_back(st.row());
    json out = json::array();
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) out.push_back(*it);
    return out;
}

Changes History::record(const std::vector<Result>& results, const std::string& source, const std::string& ts_in,
                        const std::vector<RaidArray>* arrays, Changes* array_changes) {
    std::string ts = ts_in.empty() ? utc_now_iso() : ts_in;
    Changes changes;
    exec("BEGIN");
    try {
        Stmt run(db_, "INSERT INTO runs(ts, source) VALUES (?, ?)");
        run.bind(1, ts); run.bind(2, source); run.step();
        int64_t run_id = sqlite3_last_insert_rowid(db_);
        for (auto& [d, a] : results) {
            json prev = last_reading(d.key());
            changes[d.key()] = compare_readings(prev, a.status, a.health, d.metrics);
            Stmt ins(db_, "INSERT INTO readings(run_id, drive_key, device, model, serial, status, health, performance,"
                          " temperature, power_on_hours, metrics) VALUES (?,?,?,?,?,?,?,?,?,?,?)");
            ins.bind(1, run_id); ins.bind(2, d.key()); ins.bind(3, d.device); ins.bind(4, d.model);
            ins.bind(5, d.serial); ins.bind(6, a.status);
            ins.bind(7, a.health ? std::optional<int64_t>(*a.health) : std::nullopt);
            ins.bind(8, a.performance ? std::optional<int64_t>(*a.performance) : std::nullopt);
            ins.bind(9, d.temperature_c); ins.bind(10, d.power_on_hours); ins.bind(11, d.metrics.dump());
            ins.step();
        }
        if (arrays) for (auto& a : *arrays) {
            json prev = last_array_reading(a.id);
            json ch = compare_array_states(prev, a.state);
            if (array_changes && !ch.empty()) (*array_changes)[a.id] = ch;
            json members = json::array();
            for (auto& m : a.members) members.push_back({{"device", m.device}, {"role", m.role}, {"drive_key", m.drive_key}});
            Stmt ins(db_, "INSERT INTO array_readings(run_id, array_id, name, backend, level, state, status, rebuild_pct, members)"
                          " VALUES (?,?,?,?,?,?,?,?,?)");
            ins.bind(1, run_id); ins.bind(2, a.id); ins.bind(3, a.name); ins.bind(4, a.backend); ins.bind(5, a.level);
            ins.bind(6, a.state); ins.bind(7, array_status(a, results));
            if (a.rebuild_pct) sqlite3_bind_double(ins.s, 8, *a.rebuild_pct); else sqlite3_bind_null(ins.s, 8);
            ins.bind(9, members.dump());
            ins.step();
        }
        exec("COMMIT");
    } catch (...) {
        exec("ROLLBACK");
        throw;
    }
    return changes;
}

json History::drives() {
    Stmt st(db_, "SELECT drive_key, MAX(device) device, MAX(model) model, MAX(serial) serial, COUNT(*) n, "
                 "MIN(runs.ts) first_ts, MAX(runs.ts) last_ts FROM readings JOIN runs ON runs.id = readings.run_id "
                 "GROUP BY drive_key ORDER BY device");
    json out = json::array();
    while (st.step()) out.push_back(st.row());
    return out;
}

json History::readings(const std::string& key, int limit) {
    Stmt st(db_, "SELECT r.*, runs.ts FROM readings r JOIN runs ON runs.id = r.run_id WHERE drive_key = ? "
                 "ORDER BY r.run_id DESC LIMIT ?");
    st.bind(1, key); st.bind(2, static_cast<int64_t>(limit));
    std::vector<json> rows;
    while (st.step()) rows.push_back(fix_metrics(st.row()));
    json out = json::array();
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) out.push_back(*it);
    return out;
}

static std::optional<int> opt_health(const json& r) {
    auto h = jint(r, "health");
    return h ? std::optional<int>(static_cast<int>(*h)) : std::nullopt;
}

json History::summary(int limit, const std::string& filter) {
    json out = json::array();
    for (auto drv : drives()) {
        std::string dev = jstr(drv, "device").value_or("");
        std::string ser = jstr(drv, "serial").value_or("");
        if (!filter.empty() && !contains(dev, filter) && filter != ser && !ends_with(dev, "/" + filter)) continue;
        json rs = readings(drv["drive_key"].get<std::string>(), limit);
        json flagged = json::array();
        for (size_t i = 1; i < rs.size(); ++i) {
            for (auto c : compare_readings(rs[i - 1], jstr(rs[i], "status").value_or(""), opt_health(rs[i]), rs[i]["metrics"])) {
                c["ts"] = rs[i]["ts"];
                flagged.push_back(c);
            }
        }
        json latest = json::array();
        if (rs.size() > 1) {
            const json& last = rs[rs.size() - 1];
            latest = compare_readings(rs[rs.size() - 2], jstr(last, "status").value_or(""), opt_health(last), last["metrics"]);
        }
        drv["readings"] = rs;
        drv["changes_in_window"] = flagged;
        drv["changes_since_last"] = latest;
        out.push_back(drv);
    }
    return out;
}

}  // namespace hdmon
