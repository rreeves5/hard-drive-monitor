// Opt-in self-update from the GitHub releases of rreeves5/hard-drive-monitor.
// Nothing here runs unless the user types `hdmon update`, or sets [update] auto_check = true.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "util.hpp"

namespace hdmon {

extern const char* UPDATE_REPO;          // "rreeves5/hard-drive-monitor"
extern const char* UPDATE_API_LATEST;    // https://api.github.com/repos/<repo>/releases/latest

// Which program updates itself: the `hdmon` CLI or the `hdmon-gui` desktop app.
enum class UpdateFlavor { Cli, Gui };

// ---- pure helpers (unit tested)
std::vector<int> version_numbers(const std::string& v);              // "Beta V 0.2.1" / "v0.2.1" -> {0,2,1}
int compare_versions(const std::string& a, const std::string& b);    // <0, 0, >0 on the numeric part
std::string display_version(const std::string& tag);                 // "v0.2.2" -> "Beta V 0.2.2"
// Release asset(s) for an OS/arch. First = the console `hdmon`; Windows adds the window-less hdmonw.
// Empty when no prebuilt binary exists (e.g. Linux arm64: build from source).
// GUI: Windows hdmon-gui-windows-x86_64.exe, Linux hdmon-gui-linux-x86_64; macOS ships a zipped .app
// (Hard-Drive-Monitor-macos.zip) that is not replaced in place, so the list is empty there.
std::vector<std::string> asset_names(OS os, const std::string& arch, UpdateFlavor flavor = UpdateFlavor::Cli);
std::string current_arch();                                          // x86_64 | arm64 | other
// `sha256sum` / `shasum -a 256` output: "<64 hex>  name" or "<64 hex> *name" -> name -> lowercase hex.
std::map<std::string, std::string> parse_checksums(const std::string& text);
std::string sha256_hex(const std::string& data);
std::optional<std::string> sha256_file(const std::string& path);

// ---- I/O seam: tests replace these so no test touches the network or the real binary.
struct UpdateHooks {
    // GET url -> body; nullopt + err on failure. Default: curl -fsSL.
    std::function<std::optional<std::string>(const std::string& url, std::string& err)> fetch;
    // GET url -> file. Default: curl -fsSL -o path.
    std::function<bool(const std::string& url, const std::string& path, std::string& err)> download;
    std::function<std::pair<bool, std::string>(const std::string& title, const std::string& body)> notify;
    std::function<bool(const std::string& question)> confirm;    // default: [y/N] on stdin
    std::function<std::string(const std::string& exe)> version_of; // default: `exe --version`
    std::string self_path;                                          // default: the running executable
};
UpdateHooks& update_hooks();          // process-wide (tests swap and restore it)
UpdateHooks default_update_hooks();

struct ReleaseInfo {
    bool ok = false;
    std::string error;
    std::string tag, name, url;
    std::map<std::string, std::string> assets;   // name -> browser_download_url
};
ReleaseInfo fetch_latest_release();
ReleaseInfo parse_release(const std::string& body);

struct CheckResult {
    bool ok = false;
    std::string error;
    std::string current, latest_tag, latest, url;
    bool update_available = false;
    json to_json() const;
};
// rel_out (optional) receives the release details for a following install_update().
CheckResult check_for_update(const std::string& current_version, ReleaseInfo* rel_out = nullptr);

// Download, verify SHA-256 against SHA256SUMS.txt, replace the binary (Windows: running hdmon.exe ->
// hdmon.old.exe, new one moved in; POSIX: temp file in the same dir + chmod + atomic rename).
struct InstallResult { bool ok = false; std::string message; std::vector<std::string> log; };
InstallResult install_update(const ReleaseInfo& rel, UpdateFlavor flavor = UpdateFlavor::Cli);

// Windows: delete hdmon.old.exe / hdmonw.old.exe left by a previous update (no-op elsewhere).
void cleanup_old_binaries();

// ---- scheduled-check integration ([update] auto_check / auto_install, both default false)
// update-state.json (CLI) / update-state-gui.json (GUI) next to the history DB
std::string update_state_path(const std::string& db_path, UpdateFlavor flavor = UpdateFlavor::Cli);
// Called by `hdmon check`/`watch`. Does nothing (and never touches the network) unless
// cfg.update.auto_check is true; then at most one check per 24 h. Returns a JSON summary, or null if nothing ran.
json scheduled_update_check(const json& cfg, const std::string& db_path, bool allow_notify, int64_t now_epoch,
                            UpdateFlavor flavor = UpdateFlavor::Cli);

// Edit one key of the config file in place (creates it from the sample if missing).
bool set_config_value(const std::string& path, const std::string& section, const std::string& key,
                      const std::string& raw_value, std::string* err);

}  // namespace hdmon
