// Raw-socket D-Bus client: wire format checked against libdbus-marshalled bytes (tools/dbus-verify/gold.c).
#include "helpers.hpp"
#include "dbus.hpp"

using namespace hdmon;
using namespace hdmon::dbus;

namespace {
std::vector<uint8_t> unhex(const std::string& h) {
    std::vector<uint8_t> v;
    for (size_t i = 0; i + 1 < h.size(); i += 2) v.push_back(static_cast<uint8_t>(std::stoi(h.substr(i, 2), nullptr, 16)));
    return v;
}
// dbus_message_new_method_call(systemd1 StartUnit "hard-drive-monitor.timer" "replace"), serial 3, dbus_message_marshal
const char* LIBDBUS_STARTUNIT =
    "6c0100012c00000003000000a000000001016f00190000002f6f72672f667265656465736b746f702f73797374656d6431000000000000"
    "0006017300180000006f72672e667265656465736b746f702e73797374656d6431000000000000000002017300200000006f72672e6672"
    "65656465736b746f702e73797374656d64312e4d616e61676572000000000000000003017300090000005374617274556e697400000000"
    "000000080167000273730018000000686172642d64726976652d6d6f6e69746f722e74696d657200000000070000007265706c61636500";
}  // namespace

TEST_CASE("dbus: method call encodes exactly like libdbus") {
    Message m;
    m.serial = 3;
    m.destination = "org.freedesktop.systemd1";
    m.path = "/org/freedesktop/systemd1";
    m.interface = "org.freedesktop.systemd1.Manager";
    m.member = "StartUnit";
    m.signature = "ss";
    Writer w;
    w.str("hard-drive-monitor.timer");
    w.str("replace");
    m.body = w.buf;
    CHECK(encode(m) == unhex(LIBDBUS_STARTUNIT));
}

TEST_CASE("dbus: decode libdbus bytes, incomplete input, and error replies") {
    auto raw = unhex(LIBDBUS_STARTUNIT);
    Message m;
    size_t used = 0;
    REQUIRE(decode(raw, m, &used));
    CHECK(used == raw.size());
    CHECK(m.type == 1);
    CHECK(m.serial == 3);
    CHECK(m.member == "StartUnit");
    CHECK(m.destination == "org.freedesktop.systemd1");
    CHECK(m.signature == "ss");
    CHECK(m.first_string() == "hard-drive-monitor.timer");
    std::vector<uint8_t> part(raw.begin(), raw.end() - 5);
    Message p;
    CHECK_FALSE(decode(part, p, &used));

    Message e;
    e.type = 3;
    e.serial = 9;
    e.reply_serial = 2;
    e.error_name = "org.freedesktop.DBus.Error.ServiceUnknown";
    e.signature = "s";
    Writer w;
    w.str("The name is not activatable");
    e.body = w.buf;
    auto bytes = encode(e);
    bytes.insert(bytes.end(), raw.begin(), raw.end());   // two messages back to back
    Message d;
    REQUIRE(decode(bytes, d, &used));
    CHECK(d.type == 3);
    CHECK(d.reply_serial == 2);
    CHECK(d.error_name == "org.freedesktop.DBus.Error.ServiceUnknown");
    CHECK(d.first_string() == "The name is not activatable");
    std::vector<uint8_t> rest(bytes.begin() + static_cast<long>(used), bytes.end());
    Message d2;
    CHECK(decode(rest, d2));
    CHECK(d2.member == "StartUnit");
}

TEST_CASE("dbus: Notify (susssasa{sv}i) encodes exactly like libdbus") {
    // tools/dbus-verify/gold_notify.c: Notify("Hard Drive Monitor", 0, "drive-harddisk", "T", "B", [], {urgency: <byte 2>}, -1)
    const char* gold =
    "6c0100015c000000020000009b00000001016f001e0000002f6f72672f667265656465736b746f702f4e6f74696669636174"
    "696f6e730000060173001d0000006f72672e667265656465736b746f702e4e6f74696669636174696f6e7300000002017300"
    "1d0000006f72672e667265656465736b746f702e4e6f74696669636174696f6e7300000003017300060000004e6f74696679"
    "0000080167000d73757373736173617b73767d690000000000001200000048617264204472697665204d6f6e69746f720000"
    "000000000e00000064726976652d686172646469736b00000100000054000000010000004200000000000000100000000700"
    "0000757267656e63790001790002ffffffff";
    Message m;
    m.serial = 2;
    m.destination = "org.freedesktop.Notifications";
    m.path = "/org/freedesktop/Notifications";
    m.interface = "org.freedesktop.Notifications";
    m.member = "Notify";
    m.signature = "susssasa{sv}i";
    Writer w;
    w.str("Hard Drive Monitor");
    w.u32(0);
    w.str("drive-harddisk");
    w.str("T");
    w.str("B");
    w.end_array(w.begin_array(4));
    size_t h = w.begin_array(8);
    w.begin_struct();
    w.str("urgency");
    w.variant_byte(2);
    w.end_array(h);
    w.i32(-1);
    m.body = w.buf;
    CHECK(encode(m) == unhex(gold));
}

TEST_CASE("dbus: auth line and bus addresses") {
    CHECK(auth_external_line(1000) == "AUTH EXTERNAL 31303030\r\n");
    CHECK(auth_external_line(0) == "AUTH EXTERNAL 30\r\n");
    bool abs = true;
    CHECK(socket_path_from_address("unix:path=/run/user/1000/bus", &abs) == "/run/user/1000/bus");
    CHECK_FALSE(abs);
    CHECK(socket_path_from_address("unix:abstract=/tmp/dbus-XYZ,guid=abc", &abs) == "/tmp/dbus-XYZ");
    CHECK(abs);
    CHECK(socket_path_from_address("unix:path=/tmp/a%20b,guid=1", &abs) == "/tmp/a b");
    CHECK(socket_path_from_address("tcp:host=localhost,port=1", &abs).empty());
}
