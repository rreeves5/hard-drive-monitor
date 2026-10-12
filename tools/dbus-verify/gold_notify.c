#include <dbus/dbus.h>
#include <stdio.h>
int main(void){
  DBusMessage* m = dbus_message_new_method_call("org.freedesktop.Notifications","/org/freedesktop/Notifications","org.freedesktop.Notifications","Notify");
  DBusMessageIter it, a, d, e, v; dbus_message_iter_init_append(m,&it);
  const char *app="Hard Drive Monitor", *icon="drive-harddisk", *s="T", *b="B", *k="urgency"; dbus_uint32_t r=0; dbus_int32_t t=-1; unsigned char u=2;
  dbus_message_iter_append_basic(&it,DBUS_TYPE_STRING,&app); dbus_message_iter_append_basic(&it,DBUS_TYPE_UINT32,&r);
  dbus_message_iter_append_basic(&it,DBUS_TYPE_STRING,&icon); dbus_message_iter_append_basic(&it,DBUS_TYPE_STRING,&s);
  dbus_message_iter_append_basic(&it,DBUS_TYPE_STRING,&b);
  dbus_message_iter_open_container(&it,DBUS_TYPE_ARRAY,"s",&a); dbus_message_iter_close_container(&it,&a);
  dbus_message_iter_open_container(&it,DBUS_TYPE_ARRAY,"{sv}",&d);
  dbus_message_iter_open_container(&d,DBUS_TYPE_DICT_ENTRY,NULL,&e); dbus_message_iter_append_basic(&e,DBUS_TYPE_STRING,&k);
  dbus_message_iter_open_container(&e,DBUS_TYPE_VARIANT,"y",&v); dbus_message_iter_append_basic(&v,DBUS_TYPE_BYTE,&u);
  dbus_message_iter_close_container(&e,&v); dbus_message_iter_close_container(&d,&e); dbus_message_iter_close_container(&it,&d);
  dbus_message_iter_append_basic(&it,DBUS_TYPE_INT32,&t);
  dbus_message_set_serial(m, 2);
  char* buf; int len; dbus_message_marshal(m,&buf,&len);
  for(int i=0;i<len;i++) printf("%02x", (unsigned char)buf[i]); printf("\n");
}
