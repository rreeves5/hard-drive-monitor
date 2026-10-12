// Minimal D-Bus client over the session bus socket (no libdbus / sd-bus, so the static Linux binary stays static).
// Enough for: org.freedesktop.Notifications.Notify (desktop alerts) and org.freedesktop.systemd1.Manager
// (Reload / EnableUnitFiles / StartUnit / StopUnit / DisableUnitFiles) for the background-check timer.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hdmon::dbus {

// ---- wire format (pure, unit-tested)
class Writer {
public:
    std::vector<uint8_t> buf;
    void align(size_t n) { while (buf.size() % n) buf.push_back(0); }
    void byte(uint8_t v) { buf.push_back(v); }
    void u32(uint32_t v);
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void boolean(bool v) { u32(v ? 1 : 0); }
    void str(const std::string& s);        // also object paths
    void sig(const std::string& s);
    size_t begin_array(size_t elem_align); // returns a handle for end_array
    void end_array(size_t handle);
    void begin_struct() { align(8); }
    void variant_byte(uint8_t v) { sig("y"); byte(v); }
    void variant_str(const std::string& v) { sig("s"); str(v); }
    void strings(const std::vector<std::string>& v);   // as
};

struct Message {
    uint8_t type = 1;            // 1 call, 2 return, 3 error, 4 signal
    uint32_t serial = 0, reply_serial = 0;
    std::string path, interface, member, error_name, destination, sender, signature;
    std::vector<uint8_t> body;
    std::string first_string() const;   // error text / first 's' argument, "" if none
};
std::vector<uint8_t> encode(const Message& m);                         // little-endian, header padded to 8
bool decode(const std::vector<uint8_t>& raw, Message& m, size_t* used = nullptr);   // false = incomplete/invalid
std::string auth_external_line(unsigned uid);                         // "AUTH EXTERNAL 31303030\r\n"
std::vector<std::string> session_addresses();                         // $DBUS_SESSION_BUS_ADDRESS, then $XDG_RUNTIME_DIR/bus
std::string socket_path_from_address(const std::string& addr, bool* abstract_ns);   // unix:path= / unix:abstract=

// ---- live connection (Linux and other Unix; stubs elsewhere)
class Connection {
public:
    ~Connection();
    // bus = true: message bus (sends Hello); false: peer socket such as $XDG_RUNTIME_DIR/systemd/private.
    bool open(const std::string& address, bool bus, std::string& err);
    bool open_session(std::string& err);
    // Sends a method call and waits for its reply. Returns false on transport error or an error reply (err set).
    bool call(Message call, Message& reply, std::string& err, int timeout_ms = 5000);
    bool is_open() const { return fd_ >= 0; }

private:
    int fd_ = -1;
    uint32_t serial_ = 0;
    std::vector<uint8_t> inbuf_;
};

// ---- the two services hdmon talks to
// Desktop notification. Returns false (with err) when there is no session bus or no notification server.
bool notify(const std::string& app, const std::string& summary, const std::string& body, bool critical, std::string& err);
// systemd user manager: session bus (org.freedesktop.systemd1), else $XDG_RUNTIME_DIR/systemd/private.
bool systemd_user_available();
bool systemd_user_enable(const std::vector<std::string>& units, const std::string& start_unit, std::string& err);
bool systemd_user_disable(const std::vector<std::string>& units, const std::string& stop_unit, std::string& err);
bool systemd_user_reload(std::string& err);

}  // namespace hdmon::dbus
