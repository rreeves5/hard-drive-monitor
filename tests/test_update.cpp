// Opt-in self-update: pure helpers + the update flow with fake network/binary hooks (no real network).
#include <filesystem>

#include "helpers.hpp"
#include "config.hpp"
#include "update.hpp"

using namespace hdmon;
using th::run;

namespace {
// Swaps the process-wide update hooks for fakes and restores them afterwards.
struct FakeNet {
    UpdateHooks saved;
    int fetches = 0, downloads = 0, notifies = 0;
    std::map<std::string, std::string> files;   // url -> body
    std::string release_json;
    FakeNet() : saved(update_hooks()) {
        UpdateHooks& h = update_hooks();
        h.fetch = [this](const std::string& url, std::string& err) -> std::optional<std::string> {
            ++fetches;
            if (url == UPDATE_API_LATEST) {
                if (release_json.empty()) { err = "The requested URL returned error: 404"; return std::nullopt; }
                return release_json;
            }
            auto it = files.find(url);
            if (it == files.end()) { err = "404"; return std::nullopt; }
            return it->second;
        };
        h.download = [this](const std::string& url, const std::string& path, std::string& err) {
            ++downloads;
            auto it = files.find(url);
            if (it == files.end()) { err = "404"; return false; }
            return write_file(path, it->second);
        };
        h.notify = [this](const std::string&, const std::string&) { ++notifies; return std::make_pair(true, std::string("fake")); };
        h.confirm = [](const std::string&) { return false; };
        h.version_of = [](const std::string&) { return std::string("fake-version"); };
    }
    ~FakeNet() { update_hooks() = saved; }
    // A release `tag` whose assets have the given bodies; sums are correct unless corrupt_sums.
    void publish(const std::string& tag, const std::map<std::string, std::string>& assets, bool corrupt_sums = false) {
        json rel = {{"tag_name", tag}, {"name", "Hard Drive Monitor " + display_version(tag)},
                    {"html_url", "https://github.com/rreeves5/hard-drive-monitor/releases/tag/" + tag}};
        json arr = json::array();
        std::string sums;
        for (auto& [name, body] : assets) {
            std::string url = "https://example.invalid/" + tag + "/" + name;
            files[url] = body;
            arr.push_back({{"name", name}, {"browser_download_url", url}});
            sums += (corrupt_sums ? sha256_hex(body + "x") : sha256_hex(body)) + "  " + name + "\n";
        }
        std::string sums_url = "https://example.invalid/" + tag + "/SHA256SUMS.txt";
        files[sums_url] = sums;
        arr.push_back({{"name", "SHA256SUMS.txt"}, {"browser_download_url", sums_url}});
        rel["assets"] = arr;
        release_json = rel.dump();
    }
};

std::map<std::string, std::string> all_assets() {
    return {{"hdmon-windows-x86_64.exe", "NEW-WIN"}, {"hdmonw-windows-x86_64.exe", "NEW-WINW"},
            {"hdmon-macos-universal", "NEW-MAC"}, {"hdmon-linux-x86_64", "NEW-LINUX"}};
}
std::string my_asset_body() {
    switch (current_os()) {
        case OS::Windows: return "NEW-WIN";
        case OS::MacOS: return "NEW-MAC";
        default: return "NEW-LINUX";
    }
}
}  // namespace

TEST_SUITE("update") {
TEST_CASE("version numbers and comparison") {
    CHECK(version_numbers("Beta V 0.2.1") == std::vector<int>{0, 2, 1});
    CHECK(version_numbers("v0.2.10") == std::vector<int>{0, 2, 10});
    CHECK(version_numbers("v1.0.0-rc1") == std::vector<int>{1, 0, 0});
    CHECK(version_numbers("nothing").empty());
    CHECK(compare_versions("v0.2.2", "Beta V 0.2.1") > 0);
    CHECK(compare_versions("v0.2.1", "Beta V 0.2.1") == 0);
    CHECK(compare_versions("v0.2.1", "Beta V 0.2.2") < 0);
    CHECK(compare_versions("v0.10.0", "Beta V 0.9.9") > 0);   // numeric, not lexical
    CHECK(compare_versions("v0.3", "0.2.9") > 0);
    CHECK(compare_versions("v1.0", "1.0.0") == 0);
    CHECK(display_version("v0.2.2") == "Beta V 0.2.2");
    CHECK(compare_versions(VERSION, "v0.2.1") > 0);           // this build is newer than 0.2.1
    CHECK(starts_with(VERSION, "Beta V "));
}

TEST_CASE("asset names per OS/arch") {
    CHECK(asset_names(OS::Windows, "x86_64") == std::vector<std::string>{"hdmon-windows-x86_64.exe", "hdmonw-windows-x86_64.exe"});
    CHECK(asset_names(OS::Windows, "arm64").size() == 2);       // x64 build under emulation
    CHECK(asset_names(OS::MacOS, "arm64") == std::vector<std::string>{"hdmon-macos-universal"});
    CHECK(asset_names(OS::MacOS, "x86_64") == std::vector<std::string>{"hdmon-macos-universal"});
    CHECK(asset_names(OS::Linux, "x86_64") == std::vector<std::string>{"hdmon-linux-x86_64"});
    CHECK(asset_names(OS::Linux, "arm64").empty());              // no prebuilt: build from source
    CHECK_FALSE(asset_names(current_os(), current_arch()).empty());
}

TEST_CASE("checksum lines and SHA-256") {
    std::string a(64, 'A'), b(64, 'b');
    auto m = parse_checksums(a + "  hdmon-linux-x86_64\r\n" + b + " *hdmon-windows-x86_64.exe\n# comment\n"
                             "short  x\n" + std::string(64, 'g') + "  not-hex\n" + b + "  ./SHA-ish.txt\n");
    CHECK(m.size() == 3);
    CHECK(m["hdmon-linux-x86_64"] == std::string(64, 'a'));       // lower-cased
    CHECK(m["hdmon-windows-x86_64.exe"] == b);                   // binary-mode '*' stripped
    CHECK(m.count("SHA-ish.txt") == 1);
    CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    th::TempDir tmp;
    write_file(tmp.file("f"), "abc");
    CHECK(sha256_file(tmp.file("f")).value() == sha256_hex("abc"));
    CHECK_FALSE(sha256_file(tmp.file("missing")).has_value());
}

TEST_CASE("config defaults: automatic updates are off") {
    json d = default_config();
    CHECK(d["update"]["auto_check"] == false);
    CHECK(d["update"]["auto_install"] == false);
    json s = parse_simple_toml(SAMPLE_CONFIG);
    CHECK(s["update"]["auto_check"] == false);
    CHECK(s["update"]["auto_install"] == false);
    th::TempDir tmp;
    CHECK(load_config(tmp.file("none.toml"))["update"]["auto_check"] == false);
}

TEST_CASE("set_config_value edits in place and --enable/--disable-auto-check") {
    th::TempDir tmp;
    std::string p = tmp.file("c.toml");
    CHECK(run({"init-config", "--config", p}).rc == 0);
    CHECK(run({"update", "--enable-auto-check", "--config", p}).rc == 0);
    json c = load_config(p);
    CHECK(c["update"]["auto_check"] == true);
    CHECK(c["update"]["auto_install"] == false);
    CHECK(c["email"]["port"] == 587);                            // rest of the file untouched
    CHECK(th::has(read_file(p), "# auto_check: the scheduled"));
    CHECK(run({"update", "--disable-auto-check", "--config", p}).rc == 0);
    CHECK(load_config(p)["update"]["auto_check"] == false);
    // An older config without an [update] section gets one appended; missing keys are inserted.
    std::string q = tmp.file("old.toml");
    write_file(q, "[alerts]\ndesktop = false\n");
    CHECK(set_config_value(q, "update", "auto_check", "true", nullptr));
    CHECK(set_config_value(q, "alerts", "extra", "1", nullptr));
    c = load_config(q);
    CHECK(c["update"]["auto_check"] == true);
    CHECK(c["alerts"]["desktop"] == false);
    CHECK(c["alerts"]["extra"] == 1);
    CHECK(run({"update", "--enable-auto-check", "--disable-auto-check", "--config", q}).rc == 2);
}

TEST_CASE("check does not touch the network when auto_check is false") {
    FakeNet net;
    net.publish("v9.9.9", all_assets());
    th::TempDir tmp;
    std::string cfg = tmp.file("c.toml");
    write_file(cfg, "[update]\nauto_check = false\nauto_install = true\n");   // auto_install alone does nothing
    auto r = run({"check", "--from-json", th::fix(), "--no-record", "--config", cfg, "--db", tmp.file("h.db")});
    CHECK(r.rc == 2);
    CHECK(net.fetches == 0);
    CHECK(net.downloads == 0);
    CHECK_FALSE(th::has(r.out, "UPDATE"));
    CHECK_FALSE(file_exists(update_state_path(tmp.file("h.db"))));
    // default config (no file at all) behaves the same
    r = run({"check", "--from-json", th::fix(), "--no-record", "--config", tmp.file("none.toml"), "--db", tmp.file("h.db"), "--json"});
    CHECK(net.fetches == 0);
    CHECK_FALSE(json::parse(r.out).contains("update"));
}

TEST_CASE("auto_check: one check per day, notify only, never installs") {
    FakeNet net;
    net.publish("v9.9.9", all_assets());
    th::TempDir tmp;
    std::string cfg = tmp.file("c.toml"), db = tmp.file("h.db");
    write_file(cfg, "[update]\nauto_check = true\n");
    auto r = run({"check", "--from-json", th::fix(), "--no-record", "--config", cfg, "--db", db});
    CHECK(net.fetches == 1);
    CHECK(net.downloads == 0);                                   // notify only
    CHECK(net.notifies == 1);
    CHECK(th::has(r.out, "UPDATE Beta V 9.9.9 is available"));
    CHECK(file_exists(update_state_path(db)));
    r = run({"check", "--from-json", th::fix(), "--no-record", "--config", cfg, "--db", db, "--json"});
    CHECK(net.fetches == 1);                                     // within 24 h: no second request
    CHECK(json::parse(r.out)["update"]["checked"] == false);
    // a day later: checks again, but does not re-notify about the same version
    json st = json::parse(read_file(update_state_path(db)));
    st["last_check_epoch"] = st["last_check_epoch"].get<int64_t>() - 90000;
    write_file(update_state_path(db), st.dump());
    run({"check", "--from-json", th::fix(), "--no-record", "--config", cfg, "--db", db});
    CHECK(net.fetches == 2);
    CHECK(net.notifies == 1);
}

TEST_CASE("update --check: up to date / available / json / no release") {
    FakeNet net;
    th::TempDir tmp;
    std::string cfg = tmp.file("none.toml");
    auto r = run({"update", "--check", "--config", cfg});
    CHECK(r.rc == 3);
    CHECK(th::has(r.err, "no published release"));
    net.publish("v0.2.1", all_assets());
    r = run({"update", "--check", "--config", cfg});
    CHECK(r.rc == 0);
    CHECK(th::has(r.out, std::string("Up to date: ") + VERSION + " (newer than the latest release, Beta V 0.2.1)"));
    net.publish("v" + std::string(VERSION).substr(7), all_assets());   // same version as this build
    CHECK(th::has(run({"update", "--check", "--config", cfg}).out, "Up to date"));
    net.publish("v9.3.0", all_assets());
    r = run({"update", "--check", "--config", cfg});
    CHECK(r.rc == 1);
    CHECK(th::has(r.out, "Update available: Beta V 9.3.0"));
    CHECK(th::has(r.out, "https://github.com/rreeves5/hard-drive-monitor/releases/tag/v9.3.0"));
    r = run({"update", "--check", "--json", "--config", cfg});
    json j = json::parse(r.out);
    CHECK(j["update_available"] == true);
    CHECK(j["latest"] == "Beta V 9.3.0");
    CHECK(j["latest_tag"] == "v9.3.0");
    CHECK(j["current"] == VERSION);
    CHECK(net.downloads == 0);
}

TEST_CASE("update installs a verified binary in place; bad checksum changes nothing") {
    FakeNet net;
    th::TempDir tmp;
    std::string cfg = tmp.file("none.toml");
    bool win = current_os() == OS::Windows;
    std::string self = tmp.file(win ? "hdmon.exe" : "hdmon"), twin = tmp.file("hdmonw.exe");
    write_file(self, "OLD");
    if (win) write_file(twin, "OLDW");
    update_hooks().self_path = self;

    // declined confirmation -> nothing happens
    net.publish("v9.3.0", all_assets());
    auto r = run({"update", "--config", cfg});
    CHECK(r.rc == 1);
    CHECK(th::has(r.out, "Cancelled"));
    CHECK(read_file(self) == "OLD");

    net.publish("v9.3.0", all_assets(), /*corrupt_sums=*/true);
    r = run({"update", "--yes", "--config", cfg});
    CHECK(r.rc == 3);
    CHECK(th::has(r.err, "checksum mismatch"));
    CHECK(read_file(self) == "OLD");
    for (auto& e : std::filesystem::directory_iterator(tmp.path))   // no temp files left behind
        CHECK_MESSAGE(!th::has(e.path().filename().string(), ".new"), e.path().string());

    net.publish("v9.3.0", all_assets());
    r = run({"update", "-y", "--config", cfg, "--json"});
    CHECK(r.rc == 0);
    json j = json::parse(r.out);
    CHECK(j["installed"] == true);
    CHECK(read_file(self) == my_asset_body());
    if (win) {
        CHECK(read_file(twin) == "NEW-WINW");
        CHECK(read_file(tmp.file("hdmon.old.exe")) == "OLD");    // running copy renamed aside
        CHECK(read_file(tmp.file("hdmonw.old.exe")) == "OLDW");
        cleanup_old_binaries();                                  // what the next run does
        CHECK_FALSE(file_exists(tmp.file("hdmon.old.exe")));
        CHECK_FALSE(file_exists(tmp.file("hdmonw.old.exe")));
    } else {
        CHECK(is_executable(self));
        size_t n = 0;
        for (auto& e : std::filesystem::directory_iterator(tmp.path)) { (void)e; ++n; }
        CHECK(n == 1);                                           // atomic rename, no leftovers
    }
    // --json install needs --yes
    net.publish("v9.4.0", all_assets());
    CHECK(run({"update", "--json", "--config", cfg}).rc == 2);
}

TEST_CASE("auto_install installs quietly only when both keys are true") {
    FakeNet net;
    th::TempDir tmp;
    std::string self = tmp.file(current_os() == OS::Windows ? "hdmon.exe" : "hdmon");
    write_file(self, "OLD");
    update_hooks().self_path = self;
    net.publish("v9.3.0", all_assets());
    std::string cfg = tmp.file("c.toml");
    write_file(cfg, "[update]\nauto_check = true\nauto_install = true\n");
    auto r = run({"check", "--from-json", th::fix(), "--no-record", "--no-alert", "--config", cfg, "--db", tmp.file("h.db"), "--json"});
    json u = json::parse(r.out)["update"];
    CHECK(u["installed"] == true);
    CHECK(read_file(self) == my_asset_body());
    CHECK(net.notifies == 0);                                    // --no-alert
}
}
