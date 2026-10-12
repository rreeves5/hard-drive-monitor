#include "dbus.hpp"

#include <cstddef>
#include <cstring>

#include "util.hpp"

#if !defined(_WIN32)
#include <poll.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#endif

namespace hdmon::dbus {

void Writer::u32(uint32_t v) {
    align(4);
    for (int i = 0; i < 4; ++i) buf.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void Writer::str(const std::string& s) {
    u32(static_cast<uint32_t>(s.size()));
    buf.insert(buf.end(), s.begin(), s.end());
    buf.push_back(0);
}
void Writer::sig(const std::string& s) {
    buf.push_back(static_cast<uint8_t>(s.size()));
    buf.insert(buf.end(), s.begin(), s.end());
    buf.push_back(0);
}
size_t Writer::begin_array(size_t elem_align) {
    u32(0);
    size_t len_at = buf.size() - 4;
    align(elem_align);
    return (len_at << 8) | (buf.size() - len_at - 4);   // position of the length + padding before element 0
}
void Writer::end_array(size_t h) {
    size_t len_at = h >> 8, pad = h & 0xFF;
    uint32_t n = static_cast<uint32_t>(buf.size() - len_at - 4 - pad);
    for (int i = 0; i < 4; ++i) buf[len_at + i] = static_cast<uint8_t>(n >> (8 * i));
}
void Writer::strings(const std::vector<std::string>& v) {
    size_t h = begin_array(4);
    for (auto& s : v) str(s);
    end_array(h);
}

// ---- reader
namespace {
struct Reader {
    const std::vector<uint8_t>& b;
    size_t p;
    bool ok = true;
    void align(size_t n) { while (p % n) ++p; }
    bool need(size_t n) { if (p + n > b.size()) ok = false; return ok; }
    uint8_t byte() { if (!need(1)) return 0; return b[p++]; }
    uint32_t u32() {
        align(4);
        if (!need(4)) return 0;
        uint32_t v = b[p] | b[p + 1] << 8 | b[p + 2] << 16 | static_cast<uint32_t>(b[p + 3]) << 24;
        p += 4;
        return v;
    }
    std::string str() {
        uint32_t n = u32();
        if (!need(n + 1)) return {};
        std::string s(reinterpret_cast<const char*>(&b[p]), n);
        p += n + 1;
        return s;
    }
    std::string sig() {
        uint8_t n = byte();
        if (!need(n + 1u)) return {};
        std::string s(reinterpret_cast<const char*>(&b[p]), n);
        p += n + 1u;
        return s;
    }
};
}  // namespace

std::string Message::first_string() const {
    if (signature.empty() || signature[0] != 's') return {};
    Reader r{body, 0};
    std::string s = r.str();
    return r.ok ? s : std::string();
}

std::vector<uint8_t> encode(const Message& m) {
    Writer w;
    w.byte('l');
    w.byte(m.type);
    w.byte(m.type == 1 ? 0 : 1);   // calls expect a reply; others: NO_REPLY_EXPECTED
    w.byte(1);
    w.u32(static_cast<uint32_t>(m.body.size()));
    w.u32(m.serial);
    size_t h = w.begin_array(8);
    auto field = [&](uint8_t code, char t, const std::string& v) {
        if (v.empty()) return;
        w.begin_struct();
        w.byte(code);
        w.sig(std::string(1, t));
        if (t == 'g') w.sig(v); else w.str(v);
    };
    field(1, 'o', m.path);          // same field order libdbus uses, so captured messages compare byte-for-byte
    field(6, 's', m.destination);
    field(2, 's', m.interface);
    field(3, 's', m.member);
    field(4, 's', m.error_name);
    if (m.reply_serial) { w.begin_struct(); w.byte(5); w.sig("u"); w.u32(m.reply_serial); }
    field(7, 's', m.sender);
    field(8, 'g', m.signature);
    w.end_array(h);
    w.align(8);
    w.buf.insert(w.buf.end(), m.body.begin(), m.body.end());
    return w.buf;
}

bool decode(const std::vector<uint8_t>& raw, Message& m, size_t* used) {
    if (raw.size() < 16 || raw[0] != 'l') return false;   // we only speak little-endian; servers reply in kind
    Reader r{raw, 0};
    r.byte();
    m.type = r.byte();
    r.byte();
    r.byte();
    uint32_t body_len = r.u32();
    m.serial = r.u32();
    uint32_t fields_len = r.u32();
    size_t header_end = 16 + fields_len;
    size_t body_at = (header_end + 7) & ~size_t(7);
    if (raw.size() < body_at + body_len) return false;
    while (r.ok && r.p < header_end) {
        r.align(8);
        if (r.p >= header_end) break;
        uint8_t code = r.byte();
        std::string t = r.sig();
        std::string sv;
        uint32_t uv = 0;
        if (t == "s" || t == "o") sv = r.str();
        else if (t == "g") sv = r.sig();
        else if (t == "u") uv = r.u32();
        else return false;
        switch (code) {
            case 1: m.path = sv; break;
            case 2: m.interface = sv; break;
            case 3: m.member = sv; break;
            case 4: m.error_name = sv; break;
            case 5: m.reply_serial = uv; break;
            case 6: m.destination = sv; break;
            case 7: m.sender = sv; break;
            case 8: m.signature = sv; break;
            default: break;
        }
    }
    if (!r.ok) return false;
    m.body.assign(raw.begin() + static_cast<long>(body_at), raw.begin() + static_cast<long>(body_at + body_len));
    if (used) *used = body_at + body_len;
    return true;
}

std::string auth_external_line(unsigned uid) {
    std::string d = std::to_string(uid), hex;
    static const char* H = "0123456789abcdef";
    for (unsigned char c : d) { hex += H[c >> 4]; hex += H[c & 15]; }
    return "AUTH EXTERNAL " + hex + "\r\n";
}

std::vector<std::string> session_addresses() {
    std::vector<std::string> out;
    std::string env = get_env("DBUS_SESSION_BUS_ADDRESS");
    for (auto& a : split(env, ';')) if (!a.empty()) out.push_back(a);
    std::string rt = get_env("XDG_RUNTIME_DIR");
#if !defined(_WIN32)
    if (rt.empty()) rt = "/run/user/" + std::to_string(getuid());
#endif
    if (!rt.empty()) out.push_back("unix:path=" + rt + "/bus");   // scheduled runs often lack the env var
    return out;
}

std::string socket_path_from_address(const std::string& addr, bool* abstract_ns) {
    if (abstract_ns) *abstract_ns = false;
    if (!starts_with(addr, "unix:")) return {};
    for (auto& kv : split(addr.substr(5), ',')) {
        auto eq = kv.find('=');
        if (eq == std::string::npos) continue;
        std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
        // %XX escapes in values
        std::string d;
        for (size_t i = 0; i < v.size(); ++i) {
            if (v[i] == '%' && i + 2 < v.size()) { d += static_cast<char>(std::stoi(v.substr(i + 1, 2), nullptr, 16)); i += 2; }
            else d += v[i];
        }
        if (k == "path") return d;
        if (k == "abstract") { if (abstract_ns) *abstract_ns = true; return d; }
    }
    return {};
}

// ---- connection
#if !defined(_WIN32)
Connection::~Connection() { if (fd_ >= 0) ::close(fd_); }

static bool send_all(int fd, const void* p, size_t n) {
    const char* c = static_cast<const char*>(p);
    while (n) {
#ifdef MSG_NOSIGNAL
        ssize_t k = ::send(fd, c, n, MSG_NOSIGNAL);
#else
        ssize_t k = ::send(fd, c, n, 0);
#endif
        if (k < 0) { if (errno == EINTR) continue; return false; }
        c += k;
        n -= static_cast<size_t>(k);
    }
    return true;
}
static bool read_line(int fd, std::string& line, int timeout_ms) {
    line.clear();
    while (line.size() < 4096) {
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, timeout_ms) <= 0) return false;
        char c;
        ssize_t k = ::recv(fd, &c, 1, 0);
        if (k <= 0) return false;
        line += c;
        if (line.size() >= 2 && line.compare(line.size() - 2, 2, "\r\n") == 0) return true;
    }
    return false;
}

bool Connection::open(const std::string& address, bool bus, std::string& err) {
    bool abs = false;
    std::string path = socket_path_from_address(address, &abs);
    if (path.empty()) { err = "unsupported D-Bus address " + address; return false; }
#ifdef SOCK_CLOEXEC
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
    if (fd < 0) { err = std::strerror(errno); return false; }
#ifdef SO_NOSIGPIPE
    { int one = 1; ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    size_t off = abs ? 1 : 0;
    if (path.size() + off >= sizeof sa.sun_path) { ::close(fd); err = "socket path too long"; return false; }
    std::memcpy(sa.sun_path + off, path.data(), path.size());
    socklen_t len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + off + path.size() + (abs ? 0 : 1));
    if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), len) != 0) {
        err = "connect " + path + ": " + std::strerror(errno);
        ::close(fd);
        return false;
    }
    std::string hello = std::string(1, '\0') + auth_external_line(getuid());
    std::string line;
    if (!send_all(fd, hello.data(), hello.size()) || !read_line(fd, line, 3000) || !starts_with(line, "OK ")) {
        err = "D-Bus authentication failed" + (line.empty() ? std::string() : ": " + trim(line));
        ::close(fd);
        return false;
    }
    if (!send_all(fd, "BEGIN\r\n", 7)) { ::close(fd); err = "D-Bus: send failed"; return false; }
    fd_ = fd;
    if (bus) {
        Message h, rep;
        h.destination = "org.freedesktop.DBus";
        h.path = "/org/freedesktop/DBus";
        h.interface = "org.freedesktop.DBus";
        h.member = "Hello";
        if (!call(h, rep, err)) { ::close(fd_); fd_ = -1; return false; }
    }
    return true;
}

bool Connection::open_session(std::string& err) {
    std::string last;
    for (auto& a : session_addresses()) {
        if (open(a, true, last)) return true;
    }
    err = last.empty() ? "no D-Bus session bus" : last;
    return false;
}

bool Connection::call(Message m, Message& reply, std::string& err, int timeout_ms) {
    if (fd_ < 0) { err = "not connected"; return false; }
    m.type = 1;
    m.serial = ++serial_;
    auto raw = encode(m);
    if (!send_all(fd_, raw.data(), raw.size())) { err = "D-Bus: send failed"; return false; }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        size_t used = 0;
        Message in;
        while (decode(inbuf_, in, &used)) {
            inbuf_.erase(inbuf_.begin(), inbuf_.begin() + static_cast<long>(used));
            if ((in.type == 2 || in.type == 3) && in.reply_serial == m.serial) {
                reply = in;
                if (in.type == 3) {
                    err = in.error_name + (in.first_string().empty() ? "" : ": " + in.first_string());
                    return false;
                }
                return true;
            }
            in = Message();   // signals (NameAcquired) and unrelated replies are skipped
        }
        int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
        if (left <= 0) { err = "D-Bus: no reply to " + m.member; return false; }
        pollfd p{fd_, POLLIN, 0};
        if (::poll(&p, 1, left) <= 0) continue;
        uint8_t buf[4096];
        ssize_t k = ::recv(fd_, buf, sizeof buf, 0);
        if (k <= 0) { err = "D-Bus: connection closed"; return false; }
        inbuf_.insert(inbuf_.end(), buf, buf + k);
    }
}
#else
Connection::~Connection() {}
bool Connection::open(const std::string&, bool, std::string& err) { err = "D-Bus is not used on Windows"; return false; }
bool Connection::open_session(std::string& err) { err = "D-Bus is not used on Windows"; return false; }
bool Connection::call(Message, Message&, std::string& err, int) { err = "D-Bus is not used on Windows"; return false; }
#endif

// ---- services
bool notify(const std::string& app, const std::string& summary, const std::string& body, bool critical, std::string& err) {
    Connection c;
    if (!c.open_session(err)) return false;
    Message m, rep;
    m.destination = "org.freedesktop.Notifications";
    m.path = "/org/freedesktop/Notifications";
    m.interface = "org.freedesktop.Notifications";
    m.member = "Notify";
    m.signature = "susssasa{sv}i";
    Writer w;
    w.str(app);
    w.u32(0);
    w.str("drive-harddisk");
    w.str(summary);
    w.str(body);
    w.end_array(w.begin_array(4));            // actions: none
    size_t h = w.begin_array(8);              // hints: urgency (0 low, 1 normal, 2 critical)
    w.begin_struct();
    w.str("urgency");
    w.variant_byte(critical ? 2 : 1);
    w.end_array(h);
    w.i32(-1);
    m.body = w.buf;
    return c.call(m, rep, err);
}

namespace {
const char* SD_DEST = "org.freedesktop.systemd1";
const char* SD_PATH = "/org/freedesktop/systemd1";
const char* SD_IFACE = "org.freedesktop.systemd1.Manager";

// Session bus first; the private socket works even when the user bus is not running.
std::unique_ptr<Connection> systemd_conn(bool& via_bus, std::string& err) {
    std::string e1, e2;
    auto c = std::make_unique<Connection>();
    if (c->open_session(e1)) {
        Message ping, rep;
        ping.destination = SD_DEST;
        ping.path = SD_PATH;
        ping.interface = "org.freedesktop.DBus.Peer";
        ping.member = "Ping";
        if (c->call(ping, rep, e1, 3000)) { via_bus = true; return c; }
    }
    std::string rt = get_env("XDG_RUNTIME_DIR");
#if !defined(_WIN32)
    if (rt.empty()) rt = "/run/user/" + std::to_string(getuid());
#endif
    c = std::make_unique<Connection>();
    if (c->open("unix:path=" + rt + "/systemd/private", false, e2)) { via_bus = false; return c; }
    err = "systemd user manager not reachable over D-Bus (" + e1 + "; " + e2 + ")";
    return nullptr;
}
Message sd_call(bool via_bus, const char* member, const std::string& sig, std::vector<uint8_t> body) {
    Message m;
    if (via_bus) m.destination = SD_DEST;
    m.path = SD_PATH;
    m.interface = SD_IFACE;
    m.member = member;
    m.signature = sig;
    m.body = std::move(body);
    return m;
}
}  // namespace

bool systemd_user_available() {
    bool bus = false;
    std::string err;
    return systemd_conn(bus, err) != nullptr;
}

bool systemd_user_reload(std::string& err) {
    bool bus = false;
    auto cp = systemd_conn(bus, err);
    if (!cp) return false;
    Connection& c = *cp;
    Message rep;
    return c.call(sd_call(bus, "Reload", "", {}), rep, err, 30000);
}

bool systemd_user_enable(const std::vector<std::string>& units, const std::string& start_unit, std::string& err) {
    bool bus = false;
    auto cp = systemd_conn(bus, err);
    if (!cp) return false;
    Connection& c = *cp;
    Message rep;
    if (!c.call(sd_call(bus, "Reload", "", {}), rep, err, 30000)) return false;
    Writer w;
    w.strings(units);
    w.boolean(false);   // runtime
    w.boolean(true);    // force (replace stale symlinks)
    if (!c.call(sd_call(bus, "EnableUnitFiles", "asbb", w.buf), rep, err, 30000)) return false;
    if (!c.call(sd_call(bus, "Reload", "", {}), rep, err, 30000)) return false;
    if (start_unit.empty()) return true;
    Writer s;
    s.str(start_unit);
    s.str("replace");
    return c.call(sd_call(bus, "StartUnit", "ss", s.buf), rep, err, 30000);
}

bool systemd_user_disable(const std::vector<std::string>& units, const std::string& stop_unit, std::string& err) {
    bool bus = false;
    auto cp = systemd_conn(bus, err);
    if (!cp) return false;
    Connection& c = *cp;
    Message rep;
    if (!stop_unit.empty()) {
        Writer s;
        s.str(stop_unit);
        s.str("replace");
        std::string e;
        c.call(sd_call(bus, "StopUnit", "ss", s.buf), rep, e, 30000);   // not loaded = fine
    }
    Writer w;
    w.strings(units);
    w.boolean(false);
    if (!c.call(sd_call(bus, "DisableUnitFiles", "asb", w.buf), rep, err, 30000)) return false;
    return c.call(sd_call(bus, "Reload", "", {}), rep, err, 30000);
}

}  // namespace hdmon::dbus
