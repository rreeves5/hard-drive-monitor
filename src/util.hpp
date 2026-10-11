// Hard Drive Monitor - small cross-platform helpers (strings, formatting, processes, files, time).
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "json.hpp"

namespace hdmon {

using json = nlohmann::ordered_json;

// ---- strings
std::string trim(const std::string& s);
std::string rstrip(const std::string& s);
std::string lower(std::string s);
std::string upper(std::string s);
bool starts_with(const std::string& s, const std::string& p);
bool ends_with(const std::string& s, const std::string& p);
bool contains(const std::string& s, const std::string& p);
bool icontains(const std::string& s, const std::string& p);
std::vector<std::string> split(const std::string& s, char sep);
std::vector<std::string> split_lines(const std::string& s);
std::string join(const std::vector<std::string>& v, const std::string& sep);
std::string replace_all(std::string s, const std::string& from, const std::string& to);
size_t u8len(const std::string& s);                       // number of code points
std::string u8substr(const std::string& s, size_t count);  // first `count` code points
std::string ljust(const std::string& s, size_t w);
std::string rjust(const std::string& s, size_t w);

// ---- number formatting (Python-compatible)
std::string with_commas(int64_t n);                        // f"{n:,}"
std::string fmt_fixed(double v, int decimals);             // f"{v:.Nf}"
int64_t py_round(double v);                                // round() with ties-to-even
std::string to_str(const json& v);                         // Python str() for scalars

// ---- json helpers
std::optional<int64_t> opt_int(const json& v);
std::optional<int64_t> jint(const json& obj, const char* key);
std::optional<std::string> jstr(const json& obj, const char* key);
const json& jget(const json& obj, const char* key);        // null json if missing / not an object
bool truthy(const json& v);

// ---- processes
struct ProcResult {
    int rc = -1;            // exit code; -1 if it could not be started; -2 on timeout
    std::string out, err;
    bool started = false;
};
ProcResult run_cmd(const std::vector<std::string>& argv, int timeout_s = 120,
                   const std::string* stdin_data = nullptr);
std::optional<std::string> which(const std::string& name);
std::string self_exe_path();
void set_env(const std::string& k, const std::string& v);
std::string get_env(const char* k);

// ---- files
bool file_exists(const std::string& p);
bool is_executable(const std::string& p);
bool is_dir(const std::string& p);
std::string read_file(const std::string& p);
bool write_file(const std::string& p, const std::string& data);
bool make_dirs(const std::string& p);
std::string path_join(const std::string& a, const std::string& b);
std::string basename(const std::string& p);
std::string dirname(const std::string& p);
std::vector<std::string> list_json_files(const std::string& dir);

// ---- time
std::string utc_now_iso();          // 2026-10-11T22:05:43+00:00
std::string local_now_iso();        // 2026-10-11T18:05:43-04:00
std::string local_now_hms();        // 2026-10-11 18:05:43
std::string local_ts(const std::string& iso_utc);  // "2026-10-11 18:05 EDT"

// ---- platform
enum class OS { Linux, MacOS, Windows };
OS current_os();
std::string os_name(OS os);
bool is_root();
bool is_windows_admin();
void console_init(bool want_vt);   // Windows: UTF-8 code page + VT escapes; no-op elsewhere

}  // namespace hdmon
