#include <assert.h>

#include "global.h"
#include "support.h"
#include "list.h"

void list_init(ListHead *head)
{
	assert(head->next == NULL);
	assert(head->slot == -1);

	if (!dbus_connection_allocate_data_slot(&head->slot)) {
		error("dbus_connection_allocate_data_slot(): OOM");
		exit(EXIT_FAILURE);
	}
}

/* Add connection to the list. connection must not already be in this
 * list. List takes a reference to the connection.
 */
void list_prepend(ListHead *head, DBusConnection *connection)
{
	dbus_int32_t slot = head->slot;

	assert(slot != -1);
	assert(dbus_connection_get_data(connection, slot) == NULL);
	
	if (head->next)
		dbus_connection_set_data(connection, slot, head->next, NULL);

	head->next = connection;
	dbus_connection_ref(connection);
}

void list_remove(ListHead *head, DBusConnection *connection)
{
	DBusConnection *prev, *next;
	dbus_int32_t slot = head->slot;

	assert(slot != -1);
	assert(head->next != NULL);
	assert(connection != NULL);

	next = dbus_connection_get_data(connection, slot);

	if (head->next == connection) {
		/* First link */
		head->next = next;
		goto out;
	}

	prev = head->next;

	while (prev) {
		DBusConnection *this;

		this = dbus_connection_get_data(prev, slot);
		if (this == connection) {
			dbus_connection_set_data(prev, slot, next, NULL);
			goto out;
		}
	}

	assert(0);
out:
	dbus_connection_set_data(connection, slot, NULL, NULL);
	return;
}

/* Call 'callback(connection)' for each connection in the list.
 * If empty is 1, the list will be empty at the end.
 */
void list_foreach(ListHead *head, void (*callback)(DBusConnection *connection),
		  int empty)
{
	dbus_int32_t slot = head->slot;
	DBusConnection *next = head->next;

	assert(slot != -1);
	assert(empty == 0 || empty == 1);

	if (empty)
		head->next = NULL;

	while (next) {
		DBusConnection *this = next;

		callback(this);

		next = dbus_connection_get_data(this, slot);

		dbus_connection_unref(this);
	}
}

/* List must be empty */
void list_destroy(ListHead *head)
{
	assert(head->next == NULL);
	assert(head->slot != -1);

	dbus_connection_free_data_slot(&head->slot);

	assert(head->slot == -1);
}

int list_contains(ListHead *head, DBusConnection *connection)
{
	DBusConnection *next;
	dbus_int32_t slot = head->slot;

	assert(slot != -1);
	assert(connection);

#if 0
	if (dbus_connection_get_data(connection, slot) != NULL)
		return 1;
#endif

	for (next = head->next; next;
			next = dbus_connection_get_data(next, slot)) {
		if (next == connection)
			return 1;
	}
	return 0;
}
