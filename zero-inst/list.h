typedef struct _ListHead ListHead;

struct _ListHead {
	DBusConnection *next;
	dbus_int32_t slot;
};

#define LIST_INIT {NULL, -1}

void list_init(ListHead *head);
void list_prepend(ListHead *head, DBusConnection *connection);
void list_remove(ListHead *head, DBusConnection *connection);
void list_foreach(ListHead *head,
		  void (*callback)(DBusConnection *connection, Task *task),
		  int empty, Task *task);
int list_contains(ListHead *head, DBusConnection *connection);
void list_destroy(ListHead *head);
