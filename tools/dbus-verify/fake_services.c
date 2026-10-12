/* Test-only fake D-Bus services for verifying hdmon's raw-socket D-Bus client on a box with no desktop / systemd.
 * Owns org.freedesktop.Notifications and org.freedesktop.systemd1 on the session bus, logs every call with its
 * arguments to stdout, and replies like the real services. Build: cc fake_services.c $(pkg-config --cflags --libs dbus-1) */
#include <dbus/dbus.h>
#include <stdio.h>
#include <string.h>

static void dump_iter(DBusMessageIter* it, int depth) {
    do {
        int t = dbus_message_iter_get_arg_type(it);
        if (t == DBUS_TYPE_INVALID) break;
        for (int i = 0; i < depth; i++) printf("  ");
        if (t == DBUS_TYPE_STRING || t == DBUS_TYPE_OBJECT_PATH) { const char* s; dbus_message_iter_get_basic(it, &s); printf("s \"%s\"\n", s); }
        else if (t == DBUS_TYPE_UINT32 || t == DBUS_TYPE_INT32) { dbus_uint32_t v; dbus_message_iter_get_basic(it, &v); printf("%c %d\n", t, (int)v); }
        else if (t == DBUS_TYPE_BYTE) { unsigned char v; dbus_message_iter_get_basic(it, &v); printf("y %u\n", v); }
        else if (t == DBUS_TYPE_BOOLEAN) { dbus_bool_t v; dbus_message_iter_get_basic(it, &v); printf("b %s\n", v ? "true" : "false"); }
        else { DBusMessageIter sub; printf("%c [\n", t); dbus_message_iter_recurse(it, &sub); dump_iter(&sub, depth + 1);
               for (int i = 0; i < depth; i++) printf("  "); printf("]\n"); }
    } while (dbus_message_iter_next(it));
}

int main(void) {
    DBusError e; dbus_error_init(&e);
    DBusConnection* c = dbus_bus_get(DBUS_BUS_SESSION, &e);
    if (!c) { fprintf(stderr, "bus: %s\n", e.message); return 1; }
    dbus_bus_request_name(c, "org.freedesktop.Notifications", 0, &e);
    dbus_bus_request_name(c, "org.freedesktop.systemd1", 0, &e);
    printf("READY\n"); fflush(stdout);
    dbus_uint32_t nid = 41;
    while (dbus_connection_read_write(c, -1)) {
        DBusMessage* m;
        while ((m = dbus_connection_pop_message(c))) {
            if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL) { dbus_message_unref(m); continue; }
            const char* mem = dbus_message_get_member(m);
            printf("CALL %s.%s sig=%s dest=%s\n", dbus_message_get_interface(m), mem, dbus_message_get_signature(m),
                   dbus_message_get_destination(m));
            DBusMessageIter it;
            if (dbus_message_iter_init(m, &it)) dump_iter(&it, 1);
            fflush(stdout);
            DBusMessage* r = dbus_message_new_method_return(m);
            DBusMessageIter ri; dbus_message_iter_init_append(r, &ri);
            if (!strcmp(mem, "Notify")) { nid++; dbus_message_iter_append_basic(&ri, DBUS_TYPE_UINT32, &nid); }
            else if (!strcmp(mem, "StartUnit") || !strcmp(mem, "StopUnit")) {
                const char* job = "/org/freedesktop/systemd1/job/7"; dbus_message_iter_append_basic(&ri, DBUS_TYPE_OBJECT_PATH, &job); }
            else if (!strcmp(mem, "EnableUnitFiles")) {
                dbus_bool_t f = 0; dbus_message_iter_append_basic(&ri, DBUS_TYPE_BOOLEAN, &f);
                DBusMessageIter a; dbus_message_iter_open_container(&ri, DBUS_TYPE_ARRAY, "(sss)", &a); dbus_message_iter_close_container(&ri, &a); }
            else if (!strcmp(mem, "DisableUnitFiles")) {
                DBusMessageIter a; dbus_message_iter_open_container(&ri, DBUS_TYPE_ARRAY, "(sss)", &a); dbus_message_iter_close_container(&ri, &a); }
            dbus_connection_send(c, r, NULL); dbus_message_unref(r); dbus_message_unref(m);
        }
    }
    return 0;
}
