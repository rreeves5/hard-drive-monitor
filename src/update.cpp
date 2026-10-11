#include "update.hpp"

#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>

#include "alerts.hpp"
#include "cli.hpp"
#include "config.hpp"

namespace hdmon {
namespace fs = std::filesystem;

const char* UPDATE_REPO = "rreeves5/hard-drive-monitor";
const char* UPDATE_API_LATEST = "https://api.github.com/repos/rreeves5/hard-drive-monitor/releases/latest";
static const char* SUMS_NAME = "SHA256SUMS.txt";

// ------------------------------------------------------------------ versions / assets / checksums
std::vector<int> version_numbers(const std::string& v) {
    std::vector<int> out;
    size_t i = 0;
    while (i < v.size() && !std::isdigit(static_cast<unsigned char>(v[i]))) ++i;
    while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) {
        long n = 0;
        while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) {
            n = n * 10 + (v[i] - '0');
            if (n > 1000000) n = 1000000;
            ++i;
        }
        out.push_back(static_cast<int>(n));
        if (i + 1 < v.size() && v[i] == '.' && std::isdigit(static_cast<unsigned char>(v[i + 1]))) ++i;
        else break;
    }
    return out;
}

int compare_versions(const std::string& a, const std::string& b) {
    auto x = version_numbers(a), y = version_numbers(b);
    size_t n = std::max(x.size(), y.size());
    for (size_t i = 0; i < n; ++i) {
        int p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}

std::string display_version(const std::string& tag) {
    auto n = version_numbers(tag);
    if (n.empty()) return tag;
    std::vector<std::string> s;
    for (int x : n) s.push_back(std::to_string(x));
    return "Beta V " + join(s, ".");
}

std::string current_arch() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__arm64__)
    return "arm64";
#else
    return "other";
#endif
}

std::vector<std::string> asset_names(OS os, const std::string& arch) {
    switch (os) {
        case OS::Windows:   // x64 build; Windows on ARM runs it under emulation
            if (arch == "x86_64" || arch == "arm64") return {"hdmon-windows-x86_64.exe", "hdmonw-windows-x86_64.exe"};
            return {};
        case OS::MacOS:     // universal binary (arm64 + x86_64)
            if (arch == "x86_64" || arch == "arm64") return {"hdmon-macos-universal"};
            return {};
        default:
            if (arch == "x86_64") return {"hdmon-linux-x86_64"};
            return {};
    }
}

std::map<std::string, std::string> parse_checksums(const std::string& text) {
    std::map<std::string, std::string> out;
    for (auto& raw : split_lines(text)) {
        std::string line = trim(raw);
        if (line.size() < 66 || line[0] == '#') continue;
        std::string hash = lower(line.substr(0, 64));
        bool hex = true;
        for (char c : hash) if (!std::isxdigit(static_cast<unsigned char>(c))) { hex = false; break; }
        if (!hex || (line[64] != ' ' && line[64] != '\t')) continue;
        std::string name = trim(line.substr(64));
        if (!name.empty() && name[0] == '*') name = name.substr(1);   // binary-mode marker
        if (starts_with(name, "./")) name = name.substr(2);
        if (!name.empty()) out[name] = hash;
    }
    return out;
}

// Compact FIPS 180-4 SHA-256.
namespace {
struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned char buf[64];
    size_t blen = 0;
    uint64_t total = 0;
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const unsigned char* p) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) | (uint32_t(p[4 * i + 2]) << 8) | p[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const unsigned char* p, size_t n) {
        total += n;
        while (n) {
            size_t take = std::min(n, 64 - blen);
            std::memcpy(buf + blen, p, take);
            blen += take; p += take; n -= take;
            if (blen == 64) { block(buf); blen = 0; }
        }
    }
    std::string hex() {
        uint64_t bits = total * 8;
        unsigned char pad = 0x80, zero = 0;
        update(&pad, 1);
        while (blen != 56) update(&zero, 1);
        unsigned char len[8];
        for (int i = 0; i < 8; ++i) len[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
        update(len, 8);
        static const char* d = "0123456789abcdef";
        std::string s;
        for (uint32_t v : h) for (int i = 28; i >= 0; i -= 4) s += d[(v >> i) & 0xF];
        return s;
    }
};
}  // namespace

std::string sha256_hex(const std::string& data) {
    Sha256 s;
    s.update(reinterpret_cast<const unsigned char*>(data.data()), data.size());
    return s.hex();
}

std::optional<std::string> sha256_file(const std::string& path) {
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) return std::nullopt;
    Sha256 s;
    char b[65536];
    while (f) {
        f.read(b, sizeof b);
        if (f.gcount() > 0) s.update(reinterpret_cast<const unsigned char*>(b), static_cast<size_t>(f.gcount()));
    }
    return s.hex();
}

// ------------------------------------------------------------------ hooks
UpdateHooks default_update_hooks() {
    UpdateHooks h;
    h.fetch = [](const std::string& url, std::string& err) -> std::optional<std::string> {
        auto r = run_cmd({"curl", "-fsSL", "--max-time", "60", "-H", "Accept: application/vnd.github+json",
                          "-H", std::string("User-Agent: hdmon/") + VERSION, url}, 90);
        if (!r.started) { err = "curl not found (needed for updates)"; return std::nullopt; }
        if (r.rc != 0) { err = trim(r.err).empty() ? "curl exit " + std::to_string(r.rc) : trim(r.err); return std::nullopt; }
        return r.out;
    };
    h.download = [](const std::string& url, const std::string& path, std::string& err) {
        auto r = run_cmd({"curl", "-fsSL", "--max-time", "900", "-H", std::string("User-Agent: hdmon/") + VERSION,
                          "-o", path, url}, 960);
        if (!r.started) { err = "curl not found (needed for updates)"; return false; }
        if (r.rc != 0) { err = trim(r.err).empty() ? "curl exit " + std::to_string(r.rc) : trim(r.err); return false; }
        return true;
    };
    h.notify = [](const std::string& t, const std::string& b) { return notify_desktop(t, b); };
    h.confirm = [](const std::string& q) {
        std::cout << q << " [y/N] " << std::flush;
        std::string ans;
        if (!std::getline(std::cin, ans)) return false;
        ans = lower(trim(ans));
        return ans == "y" || ans == "yes";
    };
    h.version_of = [](const std::string& exe) {
        auto r = run_cmd({exe, "--version"}, 30);
        return r.rc == 0 ? trim(r.out) : std::string();
    };
    return h;
}

UpdateHooks& update_hooks() {
    static UpdateHooks h = default_update_hooks();
    return h;
}

// ------------------------------------------------------------------ release lookup
ReleaseInfo parse_release(const std::string& body) {
    ReleaseInfo r;
    json j = json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { r.error = "unexpected reply from GitHub (not JSON)"; return r; }
    r.tag = jstr(j, "tag_name").value_or("");
    if (r.tag.empty()) {
        r.error = jstr(j, "message").value_or("no tag_name in the GitHub reply");
        return r;
    }
    r.name = jstr(j, "name").value_or("");
    r.url = jstr(j, "html_url").value_or(std::string("https://github.com/") + UPDATE_REPO + "/releases/tag/" + r.tag);
    const json& assets = jget(j, "assets");
    if (assets.is_array())
        for (auto& a : assets) {
            auto n = jstr(a, "name"), u = jstr(a, "browser_download_url");
            if (n && u) r.assets[*n] = *u;
        }
    r.ok = true;
    return r;
}

ReleaseInfo fetch_latest_release() {
    std::string err;
    auto body = update_hooks().fetch(UPDATE_API_LATEST, err);
    if (!body) {
        ReleaseInfo r;
        r.error = contains(err, "404") ? "no published release found on GitHub (" + std::string(UPDATE_REPO) + ")"
                                       : "cannot reach GitHub: " + err;
        return r;
    }
    return parse_release(*body);
}

json CheckResult::to_json() const {
    json j = json::object();
    j["ok"] = ok;
    j["current"] = current;
    j["latest"] = latest.empty() ? json() : json(latest);
    j["latest_tag"] = latest_tag.empty() ? json() : json(latest_tag);
    j["update_available"] = update_available;
    j["url"] = url.empty() ? json() : json(url);
    if (!error.empty()) j["error"] = error;
    return j;
}

static CheckResult check_impl(const std::string& current, ReleaseInfo* rel_out) {
    CheckResult c;
    c.current = current;
    ReleaseInfo rel = fetch_latest_release();
    if (!rel.ok) { c.error = rel.error; return c; }
    c.ok = true;
    c.latest_tag = rel.tag;
    c.latest = display_version(rel.tag);
    c.url = rel.url;
    c.update_available = compare_versions(rel.tag, current) > 0;
    if (rel_out) *rel_out = rel;
    return c;
}

CheckResult check_for_update(const std::string& current_version, ReleaseInfo* rel_out) { return check_impl(current_version, rel_out); }

// ------------------------------------------------------------------ install
namespace {
std::string rand_suffix() {
    std::random_device rd;
    return std::to_string(rd() % 1000000);
}
bool rename_ok(const fs::path& a, const fs::path& b, std::string& err) {
    std::error_code ec;
    fs::rename(a, b, ec);
    if (ec) { err = ec.message(); return false; }
    return true;
}
void remove_quiet(const fs::path& p) { std::error_code ec; fs::remove(p, ec); }
std::string with_suffix_before_ext(const fs::path& p, const std::string& suffix) {   // hdmon.exe -> hdmon.old.exe
    return (p.parent_path() / fs::path(p.stem().string() + suffix + p.extension().string())).string();
}
}  // namespace

InstallResult install_update(const ReleaseInfo& rel) {
    InstallResult res;
    UpdateHooks& H = update_hooks();
    auto names = asset_names(current_os(), current_arch());
    if (names.empty()) {
        res.message = "no prebuilt binary for " + os_name(current_os()) + "/" + current_arch() +
                      "; build from source (see README) instead";
        return res;
    }
    for (auto& n : names)
        if (!rel.assets.count(n)) { res.message = "release " + rel.tag + " has no asset " + n; return res; }
    if (!rel.assets.count(SUMS_NAME)) { res.message = "release " + rel.tag + " has no " + SUMS_NAME + "; refusing to install unverified files"; return res; }

    std::string err;
    fs::path self = fs::path(H.self_path.empty() ? self_exe_path() : H.self_path);
    fs::path dir = self.parent_path();
    // Plain download (no GitHub API Accept header) of the checksum list into the target folder.
    fs::path sums_tmp = dir / fs::path(".hdmon-SHA256SUMS-" + rand_suffix() + ".txt");
    if (!H.download(rel.assets.at(SUMS_NAME), sums_tmp.string(), err)) {
        remove_quiet(sums_tmp);
        res.message = "cannot download " + std::string(SUMS_NAME) + ": " + err +
                      " (is " + dir.string() + " writable? try an elevated/sudo shell)";
        return res;
    }
    auto sums = parse_checksums(read_file(sums_tmp.string()));
    remove_quiet(sums_tmp);
    // (asset, target) pairs. Windows also refreshes the window-less twin hdmonw.exe next to hdmon.exe.
    std::vector<std::pair<std::string, fs::path>> plan;
    if (current_os() == OS::Windows) {
        std::string base = lower(self.filename().string());
        bool gui = starts_with(base, "hdmonw");
        std::string orig = self.filename().string();
        fs::path twin = dir / fs::path(gui ? "hdmon" + orig.substr(6) : "hdmonw" + orig.substr(std::min<size_t>(5, orig.size())));
        bool twin_ok = starts_with(base, "hdmon") && fs::exists(twin);
        plan.push_back({gui ? names[1] : names[0], self});
        if (twin_ok && names.size() > 1) plan.push_back({gui ? names[0] : names[1], twin});
    } else {
        plan.push_back({names[0], self});
    }

    // Download + verify everything before touching the installed files.
    std::vector<fs::path> temps;
    auto cleanup = [&] { for (auto& t : temps) remove_quiet(t); };
    for (auto& [asset, target] : plan) {
        auto want = sums.find(asset);
        if (want == sums.end()) { cleanup(); res.message = std::string(SUMS_NAME) + " has no entry for " + asset; return res; }
        fs::path tmp = current_os() == OS::Windows
                           ? fs::path(with_suffix_before_ext(target, ".new"))
                           : dir / fs::path("." + target.filename().string() + ".update-" + rand_suffix());
        remove_quiet(tmp);
        temps.push_back(tmp);
        res.log.push_back("downloading " + asset);
        if (!H.download(rel.assets.at(asset), tmp.string(), err)) {
            cleanup();
            res.message = "download of " + asset + " failed: " + err +
                          (fs::exists(dir) ? " (is " + dir.string() + " writable? try an elevated/sudo shell)" : "");
            return res;
        }
        auto got = sha256_file(tmp.string());
        if (!got || *got != want->second) {
            cleanup();
            res.message = "checksum mismatch for " + asset + " (expected " + want->second + ", got " +
                          got.value_or("unreadable") + "); nothing was changed";
            return res;
        }
        res.log.push_back("verified " + asset + " sha256 " + *got);
    }

    // Replace.
    for (size_t i = 0; i < plan.size(); ++i) {
        const fs::path& target = plan[i].second;
        const fs::path& tmp = temps[i];
        if (current_os() == OS::Windows) {
            // A running .exe cannot be overwritten or deleted, but it can be renamed.
            fs::path old = fs::path(with_suffix_before_ext(target, ".old"));
            remove_quiet(old);
            if (fs::exists(old)) old = fs::path(with_suffix_before_ext(target, ".old-" + rand_suffix()));
            bool had = fs::exists(target);
            if (had && !rename_ok(target, old, err)) {
                cleanup();
                res.message = "cannot move " + target.string() + " aside: " + err;
                return res;
            }
            if (!rename_ok(tmp, target, err)) {
                if (had) { std::string e2; rename_ok(old, target, e2); }
                cleanup();
                res.message = "cannot move the new binary into place: " + err;
                return res;
            }
            res.log.push_back("replaced " + target.string() + (had ? " (previous copy: " + old.filename().string() + ", removed on next run)" : ""));
        } else {
            std::error_code ec;
            fs::permissions(tmp, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                                     fs::perms::others_read | fs::perms::others_exec,
                            fs::perm_options::replace, ec);
            if (ec || !rename_ok(tmp, target, err)) {   // rename(2) within one directory is atomic
                cleanup();
                res.message = "cannot replace " + target.string() + ": " + (ec ? ec.message() : err) +
                              " (try: sudo hdmon update)";
                return res;
            }
            res.log.push_back("replaced " + target.string());
        }
    }
    std::string v = H.version_of ? H.version_of(plan[0].second.string()) : "";
    res.ok = true;
    res.message = "Updated to " + display_version(rel.tag) + (v.empty() ? "" : " (" + v + ")");
    return res;
}

void cleanup_old_binaries() {
    if (current_os() != OS::Windows) return;
    const UpdateHooks& H = update_hooks();
    fs::path self = fs::path(H.self_path.empty() ? self_exe_path() : H.self_path);
    std::error_code ec;
    fs::path dir = self.parent_path();
    if (dir.empty() || !fs::is_directory(dir, ec)) return;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::string n = lower(it->path().filename().string());
        if (starts_with(n, "hdmon") && ends_with(n, ".exe") && contains(n, ".old")) remove_quiet(it->path());
    }
}

// ------------------------------------------------------------------ scheduled check
std::string update_state_path(const std::string& db_path) {
    std::string d = dirname(db_path.empty() ? default_db() : db_path);
    return path_join(d.empty() ? "." : d, "update-state.json");
}

json scheduled_update_check(const json& cfg, const std::string& db_path, bool allow_notify, int64_t now_epoch) {
    const json& up = jget(cfg, "update");
    if (!truthy(jget(up, "auto_check"))) return json();      // default: never touches the network
    std::string sp = update_state_path(db_path);
    json state = json::object();
    if (file_exists(sp)) {
        state = json::parse(read_file(sp), nullptr, false);
        if (state.is_discarded() || !state.is_object()) state = json::object();
    }
    int64_t last = jint(state, "last_check_epoch").value_or(0);
    if (last > 0 && now_epoch - last < 86400 && now_epoch >= last)
        return {{"checked", false}, {"reason", "already checked in the last 24 h"}, {"last_check", jget(state, "last_check")}};

    state["last_check_epoch"] = now_epoch;
    state["last_check"] = utc_now_iso();
    make_dirs(dirname(sp));
    write_file(sp, state.dump(2));          // record the attempt first so failures do not retry hourly

    ReleaseInfo rel;
    CheckResult c = check_impl(VERSION, &rel);
    json out = c.to_json();
    out["checked"] = true;
    if (c.ok) state["latest_tag"] = c.latest_tag;
    if (c.ok && c.update_available) {
        UpdateHooks& H = update_hooks();
        if (truthy(jget(up, "auto_install"))) {
            InstallResult ir = install_update(rel);
            out["installed"] = ir.ok;
            out["install_message"] = ir.message;
            if (allow_notify && H.notify)
                H.notify(ir.ok ? "Hard Drive Monitor updated" : "Hard Drive Monitor update failed",
                         ir.ok ? ir.message : c.latest + " is available but could not be installed: " + ir.message);
        } else if (jstr(state, "notified_tag").value_or("") != c.latest_tag) {
            if (allow_notify && H.notify) {
                auto [ok, info] = H.notify("Hard Drive Monitor update available",
                                           c.latest + " is out (you have " + std::string(VERSION) + "). Run: hdmon update");
                out["notified"] = ok;
                if (ok) state["notified_tag"] = c.latest_tag;
            }
        }
    }
    write_file(sp, state.dump(2));
    return out;
}

// ------------------------------------------------------------------ config editing
bool set_config_value(const std::string& path, const std::string& section, const std::string& key,
                      const std::string& raw_value, std::string* err) {
    std::string p = path.empty() ? config_path() : path;
    std::string text = file_exists(p) ? read_file(p) : std::string(SAMPLE_CONFIG);
    std::vector<std::string> lines = split_lines(text);
    std::string want = "[" + section + "]";
    std::string newline = key + " = " + raw_value;
    long sec = -1, end = static_cast<long>(lines.size());
    for (long i = 0; i < static_cast<long>(lines.size()); ++i) {
        std::string t = trim(lines[i]);
        if (sec < 0) { if (t == want) sec = i; continue; }
        if (!t.empty() && t[0] == '[') { end = i; break; }
    }
    bool done = false;
    if (sec >= 0) {
        long last_content = sec;
        for (long i = sec + 1; i < end; ++i) {
            std::string t = trim(lines[i]);
            if (!t.empty()) last_content = i;
            if (t.empty() || t[0] == '#') continue;
            auto eq = t.find('=');
            if (eq != std::string::npos && trim(t.substr(0, eq)) == key) { lines[i] = newline; done = true; break; }
        }
        if (!done) { lines.insert(lines.begin() + last_content + 1, newline); done = true; }
    } else {
        while (!lines.empty() && trim(lines.back()).empty()) lines.pop_back();
        lines.push_back("");
        lines.push_back(want);
        lines.push_back(newline);
    }
    std::string outtext = join(lines, "\n") + "\n";
    make_dirs(dirname(p));
    if (!write_file(p, outtext)) { if (err) *err = "cannot write " + p; return false; }
    std::error_code ec;
    fs::permissions(fs::path(p), fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    return true;
}

}  // namespace hdmon
