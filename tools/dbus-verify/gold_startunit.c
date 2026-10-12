#include <dbus/dbus.h>
#include <stdio.h>
int main(void){
  DBusMessage* m = dbus_message_new_method_call("org.freedesktop.systemd1","/org/freedesktop/systemd1","org.freedesktop.systemd1.Manager","StartUnit");
  const char* a="hard-drive-monitor.timer"; const char* b="replace";
  dbus_message_append_args(m, DBUS_TYPE_STRING,&a, DBUS_TYPE_STRING,&b, DBUS_TYPE_INVALID);
  dbus_message_set_serial(m, 3);
  char* buf; int len; dbus_message_marshal(m,&buf,&len);
  for(int i=0;i<len;i++) printf("%02x", (unsigned char)buf[i]); printf("\n");
}
