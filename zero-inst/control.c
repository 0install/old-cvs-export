#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/un.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include "global.h"
#include "control.h"
#include "support.h"
#include "index.h"
#include "zero-install.h"
#include "task.h"
#include "fetch.h"

static DBusWatch *dbus_watches = NULL;
static DBusConnection *dispatches = NULL;
static dbus_int32_t next_dispatch = -1;
static DBusMessageHandler *handler = NULL;
static DBusServer *server = NULL;

static DBusWatch *current_watch = NULL;	/* tmp */

static void dbus_refresh(DBusConnection *connection, DBusMessage *message,
			 DBusError *error, int force);

#define OLD_SOCKET "unix:path=/uri/0install/.lazyfs-cache/control"
#define SERVER_SOCKET "unix:path=/uri/0install/.lazyfs-cache/.control"
#define DBUS_Z_NS "net.sourceforge.zero-install"

static void remove_watch(DBusWatch *watch, void *data)
{
	DBusWatch *w;

	if (current_watch == watch) {
		current_watch = dbus_watch_get_data(current_watch);
	}

	if (watch == dbus_watches) {
		dbus_watches = dbus_watch_get_data(dbus_watches);
		dbus_watch_set_data(watch, NULL, NULL);
		return;
	}
	for (w = dbus_watches; w;) {
		DBusWatch *next = dbus_watch_get_data(w);

		if (next == watch) {
			next = dbus_watch_get_data(next);
			dbus_watch_set_data(w, next, NULL);
			dbus_watch_set_data(watch, NULL, NULL);
			return;
		}

		w = next;
	}
	assert(0);
}

static dbus_bool_t add_watch(DBusWatch *watch, void *data)
{
	dbus_watch_set_data(watch, dbus_watches, NULL);
	dbus_watches = watch;
	return 1;
}

static void dispatch_status_function(DBusConnection *connection,
                          DBusDispatchStatus new_status, void *data)
{
	DBusConnection *next;

	if (new_status == DBUS_DISPATCH_COMPLETE)
		return;

	if (!dispatches) {
		dbus_connection_set_data(connection, next_dispatch, NULL, NULL);
		dispatches = connection;
		dbus_connection_ref(connection);
		return;
	}

	for (next = dispatches; next;
		next = dbus_connection_get_data(next, next_dispatch)) {
		if (next == connection)
			return;
	}

	dbus_connection_set_data(connection, next_dispatch, dispatches, NULL);
	dispatches = connection;
	dbus_connection_ref(connection);
}

static void send_task_start(DBusConnection *connection, Task *task)
{
	DBusMessage *message;

	assert(task->str);
	assert(task->child_task->str);

	message = dbus_message_new(DBUS_Z_NS ".NewTask", NULL);

	if (message &&
	    dbus_message_append_args(message,
			DBUS_TYPE_STRING, task->str,
			DBUS_TYPE_STRING, task->child_task->str,
			DBUS_TYPE_INT64, task->child_task->size) &&
	    dbus_connection_send(connection, message, NULL)) {
		/* OK */
	} else {
		error("Out of memory");
	}

	if (message)
		dbus_message_unref(message);

}

static void dbus_monitor(DBusConnection *connection, DBusError *error)
{
	Task *task;
	unsigned long uid;

	if (!dbus_connection_get_unix_user(connection, &uid)) {
		error("Can't get UID");
		return;
	}

	for (task = all_tasks; task; task = task->next) {
		if ((task->type == TASK_CLIENT || task->type == TASK_KERNEL) &&
		    task->child_task && task->uid == uid) {
			send_task_start(connection, task);
		}
	}
}

static DBusHandlerResult message_handler(DBusMessageHandler *handler,
	DBusConnection *connection, DBusMessage *message, void *user_data)
{
	DBusMessage *reply = NULL;
	DBusError error;
	const char *name = dbus_message_get_name(message);

	dbus_error_init(&error);

	if (strcmp(name, "org.freedesktop.Local.Disconnect") == 0)
		dbus_connection_unref(connection);
	else if (strcmp(name, "org.freedesktop.DBus.Hello") == 0) {
		reply = dbus_message_new_reply(message);
		if (!reply)
			goto err;
		if (!dbus_message_append_args(reply,
				DBUS_TYPE_STRING, "Hi",
				DBUS_TYPE_INVALID))
			goto err;
	} else if (strcmp(name, DBUS_Z_NS ".Refresh") == 0) {
		dbus_refresh(connection, message, &error, 1);
		if (dbus_error_is_set(&error))
			goto err;
	} else if (strcmp(name, DBUS_Z_NS ".Rebuild") == 0) {
		dbus_refresh(connection, message, &error, 0);
		if (dbus_error_is_set(&error))
			goto err;
	} else if (strcmp(name, DBUS_Z_NS ".Monitor") == 0) {
		dbus_monitor(connection, &error);
		if (dbus_error_is_set(&error))
			goto err;
	} else {
		reply = dbus_message_new_error_reply(message, name,
					"Unknown message name");
	}

	if (reply && !dbus_connection_send(connection, reply, NULL))
		goto err;

	goto out;
err:
	if (reply) {
		dbus_message_unref(reply);
		reply = NULL;
	}

	if (dbus_error_is_set(&error)) {
		reply = dbus_message_new_error_reply(message, name,
							error.message);
		dbus_error_free(&error);
		if (!reply)
			goto oom;
		if (!dbus_connection_send(connection, reply, NULL))
			goto oom;
		goto out;
	}
oom:
	error("Out of memory; disconnecting client");
	dbus_connection_disconnect(connection);
out:
	if (reply)
		dbus_message_unref(reply);
	return DBUS_HANDLER_RESULT_REMOVE_MESSAGE;
}

static void new_dbus_client(DBusServer *server,
			    DBusConnection *new_connection,
			    void *data)
{
	/* NB: DBUS 0.12 bug: mem-leak if we don't ref the connection
	 * (for errors)
	 */

	printf("[ new client ]\n");
	if (!dbus_connection_set_watch_functions(new_connection,
				add_watch, remove_watch, NULL,
				NULL, NULL))
		return;

	dbus_connection_set_dispatch_status_function(new_connection,
					dispatch_status_function, NULL, NULL);


	if (!dbus_connection_add_filter(new_connection, handler)) {
		error("new_dbus_client: Out of memory");
		return;
	}
	
	dbus_connection_ref(new_connection);
}

void create_control_socket(void)
{
	DBusError error;
	const char *ext_only[] = {"EXTERNAL", NULL};

	if (!dbus_connection_allocate_data_slot(&next_dispatch)) {
		error("dbus_connection_allocate_data_slot(): OOM");
		exit(EXIT_FAILURE);
	}

	handler = dbus_message_handler_new(message_handler, NULL, NULL);
	if (!handler) {
		error("Out of memory for dbus_message_handler_new");
		exit(EXIT_FAILURE);
	}

	dbus_error_init(&error);
	server = dbus_server_listen(SERVER_SOCKET, &error);
	if (!server) {
		error("Can't create DBUS server socket (%s): %s",
				SERVER_SOCKET, error.message);
		dbus_error_free(&error);
		exit(EXIT_FAILURE);
	}
	dbus_server_set_auth_mechanisms(server, ext_only);
	dbus_server_set_new_connection_function(server, new_dbus_client,
						NULL, NULL);
	if (!dbus_server_set_watch_functions(server,
				add_watch, remove_watch, NULL,
				NULL, NULL)) {
		error("Out of memory");
		exit(EXIT_FAILURE);
	}

	unlink(OLD_SOCKET);
}

static void dispatch_pending(void)
{
	DBusConnection *next = dispatches;

	dispatches = NULL;

	while (next) {
		DBusDispatchStatus status;
		DBusConnection *this = next;

		status = dbus_connection_dispatch(this);
		if (status == DBUS_DISPATCH_DATA_REMAINS)
			continue;

		next = dbus_connection_get_data(this, next_dispatch);

		if (status != DBUS_DISPATCH_COMPLETE) {
			dbus_connection_disconnect(this);
			printf("[ error ]\n");
			/* XXX: unref too? */
		}

		dbus_connection_unref(this);
	}
}

int control_add_select(int n, fd_set *rfds, fd_set *wfds)
{
	DBusWatch *watch = dbus_watches;

	while (dispatches)
		dispatch_pending();

	while (watch) {
		if (dbus_watch_get_enabled(watch)) {
			int fd = dbus_watch_get_fd(watch);
			unsigned int flags = dbus_watch_get_flags(watch);

			if (fd >= n)
				n = fd + 1;
			if (flags & DBUS_WATCH_READABLE)
				FD_SET(fd, rfds);
			if (flags & DBUS_WATCH_WRITABLE)
				FD_SET(fd, wfds);
		}
			
		watch = dbus_watch_get_data(watch);
	}
	
	return n;
}

/* Just sends an OK reply to task's message */
static void send_ok(Task *task)
{
	DBusMessage *reply;

	reply = dbus_message_new_reply(task->message);
	if (!reply || !dbus_message_append_args(reply,
				DBUS_TYPE_BOOLEAN, 1,
				DBUS_TYPE_INVALID))
		error("Out of memory");

	if (!dbus_connection_send(task->connection, reply, NULL))
		error("Out of memory");

	if (reply)
		dbus_message_unref(reply);
}

static void send_result(Task *task, int success)
{
	if (success)
		send_ok(task);
	else {
		DBusMessage *reply;
		reply = dbus_message_new_error_reply(task->message,
				dbus_message_get_name(task->message),
				"Failed");
		if (!reply || !dbus_connection_send(task->connection,
						reply, NULL))
			error("Out of memory");
		if (reply)
			dbus_message_unref(reply);
	}	

	task_destroy(task, success);
}

/* Message requests the cache for 'host' be refetched.
 * (force is 0 if we're rebuilding due to a changed override.xml)
 * Returns error, or NULL on success.
 */
static void dbus_refresh(DBusConnection *connection, DBusMessage *message,
			 DBusError *error, int force)
{
	const char *site;
	Task *task = NULL;
	Index *index;

	if (!dbus_message_get_args(message, error,
				DBUS_TYPE_STRING, &site, DBUS_TYPE_INVALID))
		return;

	if (strchr(site, '/') || site[0] == '.' || !*site) {
		dbus_set_error_const(error, "Error", "Bad hostname");
		return;
	}

	task = task_new(TASK_CLIENT);
	if (!task)
		goto oom;
	task->step = send_result;
	task_set_message(task, connection, message);

	task->str = build_string("/%s", site);
	if (!task->str)
		goto oom;

	index = get_index(task->str, &task->child_task, force);
	if (index) {
		/* Already cached, and we didn't force a refresh.
		 * Rebuild site, as override.xml may have changed.
		 */
		if (build_ddds_for_site(index, site)) {
			send_ok(task);
		} else {
			dbus_set_error_const(error, "Error",
				"Failed to rebuild '...' index files");
		}
		index_free(index);
		goto done;
	}

	if (task->child_task)
		printf("fetching\n");//control_notify_user(client->task->uid);
	else {
		dbus_set_error_const(error, "Error",
				"Failed to start fetching index");
		goto done;
	}

	return;
oom:
	dbus_set_error_const(error, "Error", "Out of memory");
done:
	if (task)
		task_destroy(task, 0);
}

void control_check_select(fd_set *rfds, fd_set *wfds)
{
	current_watch = dbus_watches;
	while (current_watch) {
		DBusWatch *this = current_watch;

		/* current_watch may get moved forward by remove_watch */
		current_watch = dbus_watch_get_data(this);

		if (dbus_watch_get_enabled(this)) {
			int fd = dbus_watch_get_fd(this);
			unsigned int flags = 0;

			if (FD_ISSET(fd, rfds))
				flags |= DBUS_WATCH_READABLE;
			if (FD_ISSET(fd, wfds))
				flags |= DBUS_WATCH_WRITABLE;

			if (flags && !dbus_watch_handle(this, flags)) {
				/* XXX: OOM */
				printf("[ OOM ]\n");
			}
		}
	}
	current_watch = NULL;
}

void control_notify_user(uid_t uid)
{
	fprintf(stderr, "control_notify_user\n");
}

void control_drop_clients(void)
{
	dbus_connection_free_data_slot(&next_dispatch);
	dbus_message_handler_unref(handler);
	dbus_server_disconnect(server);
	dbus_server_unref(server);
}
