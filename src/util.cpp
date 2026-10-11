#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace hdmon {

// ------------------------------------------------------------------ strings
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
std::string rstrip(const std::string& s) {
    size_t b = s.size();
    while (b > 0 && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(0, b);
}
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
bool starts_with(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }
bool ends_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}
bool contains(const std::string& s, const std::string& p) { return s.find(p) != std::string::npos; }
bool icontains(const std::string& s, const std::string& p) { return lower(s).find(lower(p)) != std::string::npos; }
std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}
std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') { if (!cur.empty() && cur.back() == '\r') cur.pop_back(); out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) { if (i) out += sep; out += v[i]; }
    return out;
}
std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) { s.replace(pos, from.size(), to); pos += to.size(); }
    return s;
}
size_t u8len(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
    return n;
}
std::string u8substr(const std::string& s, size_t count) {
    size_t n = 0, i = 0;
    for (; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0xC0) != 0x80) { if (n == count) break; ++n; }
    }
    return s.substr(0, i);
}
std::string ljust(const std::string& s, size_t w) { size_t n = u8len(s); return n >= w ? s : s + std::string(w - n, ' '); }
std::string rjust(const std::string& s, size_t w) { size_t n = u8len(s); return n >= w ? s : std::string(w - n, ' ') + s; }

// ------------------------------------------------------------------ numbers
std::string with_commas(int64_t n) {
    bool neg = n < 0;
    uint64_t u = neg ? static_cast<uint64_t>(-(n + 1)) + 1 : static_cast<uint64_t>(n);
    std::string d = std::to_string(u), out;
    int cnt = 0;
    for (auto it = d.rbegin(); it != d.rend(); ++it) {
        if (cnt && cnt % 3 == 0) out += ',';
        out += *it;
        ++cnt;
    }
    if (neg) out += '-';
    std::reverse(out.begin(), out.end());
    return out;
}
std::string fmt_fixed(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}
int64_t py_round(double v) { return static_cast<int64_t>(std::nearbyint(v)); }  // FE_TONEAREST = ties-to-even

std::string to_str(const json& v) {
    if (v.is_null()) return "None";
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "True" : "False";
    if (v.is_number_integer()) return std::to_string(v.get<int64_t>());
    if (v.is_number_unsigned()) return std::to_string(v.get<uint64_t>());
    if (v.is_number_float()) {
        std::ostringstream os;
        os << v.get<double>();
        return os.str();
    }
    return v.dump();
}

// ------------------------------------------------------------------ json helpers
static const json kNull = nullptr;

std::optional<int64_t> opt_int(const json& v) {
    if (v.is_number_integer()) return v.get<int64_t>();
    if (v.is_number_unsigned()) return static_cast<int64_t>(v.get<uint64_t>());
    if (v.is_number_float()) return static_cast<int64_t>(std::trunc(v.get<double>()));
    if (v.is_boolean()) return v.get<bool>() ? 1 : 0;
    if (v.is_string()) {
        std::string s = trim(v.get<std::string>());
        if (s.empty()) return std::nullopt;
        size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
        if (i >= s.size()) return std::nullopt;
        for (size_t k = i; k < s.size(); ++k) if (!std::isdigit(static_cast<unsigned char>(s[k])) && s[k] != '_') return std::nullopt;
        try { return std::stoll(replace_all(s, "_", "")); } catch (...) { return std::nullopt; }
    }
    return std::nullopt;
}
const json& jget(const json& obj, const char* key) {
    if (!obj.is_object()) return kNull;
    auto it = obj.find(key);
    return it == obj.end() ? kNull : *it;
}
std::optional<int64_t> jint(const json& obj, const char* key) { return opt_int(jget(obj, key)); }
std::optional<std::string> jstr(const json& obj, const char* key) {
    const json& v = jget(obj, key);
    if (v.is_string()) return v.get<std::string>();
    return std::nullopt;
}
bool truthy(const json& v) {
    if (v.is_null()) return false;
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number_integer()) return v.get<int64_t>() != 0;
    if (v.is_number_unsigned()) return v.get<uint64_t>() != 0;
    if (v.is_number_float()) return v.get<double>() != 0.0;
    if (v.is_string()) return !v.get<std::string>().empty();
    return !v.empty();
}

// ------------------------------------------------------------------ environment / files
void set_env(const std::string& k, const std::string& v) {
#ifdef _WIN32
    _putenv_s(k.c_str(), v.c_str());
#else
    setenv(k.c_str(), v.c_str(), 1);
#endif
}
std::string get_env(const char* k) {
    const char* v = std::getenv(k);
    return v ? std::string(v) : std::string();
}
bool file_exists(const std::string& p) { std::error_code ec; return fs::is_regular_file(p, ec); }
bool is_dir(const std::string& p) { std::error_code ec; return fs::is_directory(p, ec); }
bool is_executable(const std::string& p) {
#ifdef _WIN32
    std::error_code ec;
    return fs::is_regular_file(fs::u8path(p), ec);
#else
    return file_exists(p) && !is_dir(p) && access(p.c_str(), X_OK) == 0;
#endif
}
std::string read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
bool write_file(const std::string& p, const std::string& data) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << data;
    return static_cast<bool>(f);
}
bool make_dirs(const std::string& p) { std::error_code ec; fs::create_directories(p, ec); return is_dir(p); }
std::string path_join(const std::string& a, const std::string& b) { return (fs::path(a) / b).string(); }
std::string basename(const std::string& p) { return fs::path(p).filename().string(); }
std::string dirname(const std::string& p) { return fs::path(p).parent_path().string(); }
std::vector<std::string> list_json_files(const std::string& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() == ".json") out.push_back(e.path().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<std::string> which(const std::string& name) {
    std::string path = get_env("PATH");
#ifdef _WIN32
    const char sep = ';';
    // CreateProcess needs a real program: only accept extension-less names with an executable extension.
    std::vector<std::string> exts = {".exe", ".cmd", ".bat"};
    if (fs::path(name).has_extension()) exts = {""};
#else
    const char sep = ':';
    std::vector<std::string> exts = {""};
#endif
    for (auto& d : split(path, sep)) {
        if (d.empty()) continue;
        for (auto& e : exts) {
            std::string c = path_join(d, name + e);
            if (is_executable(c)) return c;
        }
    }
    return std::nullopt;
}

std::string self_exe_path() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof buf / sizeof buf[0]));
    return fs::path(std::wstring(buf, n)).string();
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) == 0) {
        std::error_code ec;
        auto p = fs::canonical(buf, ec);
        return ec ? std::string(buf) : p.string();
    }
    return "hdmon";
#else
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? std::string("hdmon") : p.string();
#endif
}

// ------------------------------------------------------------------ processes
#ifdef _WIN32
static std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
static std::string narrow(const std::string& raw) { return raw; }
static std::wstring quote_arg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++bs; continue; }
        if (c == L'"') { out.append(bs * 2 + 1, L'\\'); out += L'"'; bs = 0; continue; }
        out.append(bs, L'\\'); bs = 0; out += c;
    }
    out.append(bs * 2, L'\\');
    out += L'"';
    return out;
}
ProcResult run_cmd(const std::vector<std::string>& argv, int timeout_s, const std::string* stdin_data) {
    ProcResult r;
    if (argv.empty()) return r;
    std::wstring cmdline;
    for (size_t i = 0; i < argv.size(); ++i) { if (i) cmdline += L' '; cmdline += quote_arg(widen(argv[i])); }
    std::wstring exe = widen(argv[0]);
    if (exe.find(L'\\') == std::wstring::npos && exe.find(L'/') == std::wstring::npos) {
        if (auto w = which(argv[0])) exe = widen(*w);
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out_r, out_w, err_r, err_w, in_r, in_w;
    CreatePipe(&out_r, &out_w, &sa, 0); SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    CreatePipe(&err_r, &err_w, &sa, 0); SetHandleInformation(err_r, HANDLE_FLAG_INHERIT, 0);
    CreatePipe(&in_r, &in_w, &sa, 0); SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = out_w; si.hStdError = err_w; si.hStdInput = in_r;
    PROCESS_INFORMATION pi{};
    std::wstring cl = cmdline;
    BOOL ok = CreateProcessW(exe.c_str(), cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(out_w); CloseHandle(err_w); CloseHandle(in_r);
    if (!ok) { CloseHandle(out_r); CloseHandle(err_r); CloseHandle(in_w); r.err = "cannot start " + argv[0]; return r; }
    r.started = true;
    auto reader = [](HANDLE h, std::string* dst) {
        char buf[4096]; DWORD n;
        while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n > 0) dst->append(buf, n);
    };
    std::thread t1(reader, out_r, &r.out), t2(reader, err_r, &r.err);
    if (stdin_data) { DWORD n; WriteFile(in_w, stdin_data->data(), static_cast<DWORD>(stdin_data->size()), &n, nullptr); }
    CloseHandle(in_w);
    DWORD w = WaitForSingleObject(pi.hProcess, timeout_s > 0 ? static_cast<DWORD>(timeout_s) * 1000 : INFINITE);
    if (w == WAIT_TIMEOUT) { TerminateProcess(pi.hProcess, 1); r.rc = -2; WaitForSingleObject(pi.hProcess, 5000); }
    else { DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code); r.rc = static_cast<int>(code); }
    t1.join(); t2.join();
    CloseHandle(out_r); CloseHandle(err_r); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    r.out = narrow(r.out); r.err = narrow(r.err);
    return r;
}
#else
ProcResult run_cmd(const std::vector<std::string>& argv, int timeout_s, const std::string* stdin_data) {
    ProcResult r;
    if (argv.empty()) return r;
    int po[2], pe[2], pi[2];
    if (pipe(po) || pipe(pe) || pipe(pi)) { r.err = "pipe failed"; return r; }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, po[1], 1);
    posix_spawn_file_actions_adddup2(&fa, pe[1], 2);
    posix_spawn_file_actions_adddup2(&fa, pi[0], 0);
    for (int fd : {po[0], po[1], pe[0], pe[1], pi[0], pi[1]}) posix_spawn_file_actions_addclose(&fa, fd);
    std::vector<char*> args;
    for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0].c_str(), &fa, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(po[1]); close(pe[1]); close(pi[0]);
    if (rc != 0) {
        close(po[0]); close(pe[0]); close(pi[1]);
        r.err = std::string("cannot start ") + argv[0] + ": " + std::strerror(rc);
        return r;
    }
    r.started = true;
    if (stdin_data && !stdin_data->empty()) {
        signal(SIGPIPE, SIG_IGN);
        ssize_t off = 0, len = static_cast<ssize_t>(stdin_data->size());
        while (off < len) { ssize_t n = write(pi[1], stdin_data->data() + off, len - off); if (n <= 0) break; off += n; }
    }
    close(pi[1]);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s > 0 ? timeout_s : 86400 * 365);
    struct pollfd fds[2] = {{po[0], POLLIN, 0}, {pe[0], POLLIN, 0}};
    int open_fds = 2;
    bool timed_out = false;
    char buf[8192];
    while (open_fds > 0) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) { timed_out = true; break; }
        int n = poll(fds, 2, static_cast<int>(std::min<long long>(left, 1000)));
        if (n < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < 2; ++i) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t k = read(fds[i].fd, buf, sizeof buf);
            if (k > 0) (i == 0 ? r.out : r.err).append(buf, static_cast<size_t>(k));
            else { close(fds[i].fd); fds[i].fd = -1; --open_fds; }
        }
    }
    if (timed_out) kill(pid, SIGKILL);
    for (auto& f : fds) if (f.fd >= 0) close(f.fd);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (timed_out) r.rc = -2;
    else if (WIFEXITED(status)) r.rc = WEXITSTATUS(status);
    else r.rc = 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    return r;
}
#endif

// ------------------------------------------------------------------ time
static std::string fmt_tm(const std::tm& t, const char* f) {
    char buf[128];
    std::strftime(buf, sizeof buf, f, &t);
    return buf;
}
static std::tm gm(std::time_t t) {
    std::tm out{};
#ifdef _WIN32
    gmtime_s(&out, &t);
#else
    gmtime_r(&t, &out);
#endif
    return out;
}
static std::tm loc(std::time_t t) {
    std::tm out{};
#ifdef _WIN32
    localtime_s(&out, &t);
#else
    localtime_r(&t, &out);
#endif
    return out;
}
std::string utc_now_iso() { return fmt_tm(gm(std::time(nullptr)), "%Y-%m-%dT%H:%M:%S+00:00"); }
std::string local_now_iso() {
    // Offset computed by hand: strftime("%z") gives a zone *name* on Windows.
    std::time_t now = std::time(nullptr);
    std::tm lt = loc(now);
    std::tm copy = lt;
#ifdef _WIN32
    long long off = static_cast<long long>(_mkgmtime(&copy)) - static_cast<long long>(now);
#else
    long long off = static_cast<long long>(timegm(&copy)) - static_cast<long long>(now);
#endif
    char sign = off < 0 ? '-' : '+';
    if (off < 0) off = -off;
    char buf[48];
    std::snprintf(buf, sizeof buf, "%c%02lld:%02lld", sign, off / 3600, (off % 3600) / 60);
    return fmt_tm(lt, "%Y-%m-%dT%H:%M:%S") + buf;
}
std::string local_now_hms() { return fmt_tm(loc(std::time(nullptr)), "%Y-%m-%d %H:%M:%S"); }
std::string local_ts(const std::string& iso) {
    if (iso.empty()) return "-";
    int Y, M, D, h, m, s, oh = 0, om = 0;
    char sign = '+';
    int n = std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d%c%d:%d", &Y, &M, &D, &h, &m, &s, &sign, &oh, &om);
    if (n < 6) return iso;
    std::tm t{};
    t.tm_year = Y - 1900; t.tm_mon = M - 1; t.tm_mday = D; t.tm_hour = h; t.tm_min = m; t.tm_sec = s;
#ifdef _WIN32
    std::time_t utc = _mkgmtime(&t);
#else
    std::time_t utc = timegm(&t);
#endif
    if (n == 9) { long off = (oh * 3600L + om * 60L) * (sign == '-' ? -1 : 1); utc -= off; }
    return fmt_tm(loc(utc), "%Y-%m-%d %H:%M %Z");
}

// ------------------------------------------------------------------ platform
OS current_os() {
#ifdef _WIN32
    return OS::Windows;
#elif defined(__APPLE__)
    return OS::MacOS;
#else
    return OS::Linux;
#endif
}
std::string os_name(OS os) { return os == OS::Windows ? "windows" : (os == OS::MacOS ? "macos" : "linux"); }
bool is_root() {
#ifdef _WIN32
    return is_windows_admin();
#else
    return geteuid() == 0;
#endif
}
bool is_windows_admin() {
#ifdef _WIN32
    BOOL member = FALSE;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID admins = nullptr;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admins)) {
        CheckTokenMembership(nullptr, admins, &member);
        FreeSid(admins);
    }
    return member != FALSE;
#else
    return false;
#endif
}
void console_init(bool want_vt) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    if (want_vt) {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (GetConsoleMode(h, &mode)) SetConsoleMode(h, mode | 0x0004 /*ENABLE_VIRTUAL_TERMINAL_PROCESSING*/);
    }
#else
    (void)want_vt;
#endif
}

}  // namespace hdmon
