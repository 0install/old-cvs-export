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
#include "list.h"

#define ZERO_INSTALL_ERROR "net.sourceforge.zero_install.Error"

static DBusWatch *dbus_watches = NULL;
static DBusServer *server = NULL;

static DBusWatch *current_watch = NULL;	/* tmp */

static const char *current_error = NULL;

static DBusObjectPathVTable vtable;

static void dbus_refresh(DBusConnection *connection, DBusMessage *message,
			 DBusError *error, int force);
static void dbus_cancel_download(DBusConnection *connection,
			DBusMessage *message, DBusError *error);

#define OLD_SOCKET "/uri/0install/.lazyfs-cache/control"
#define OLD_SOCKET2 "/uri/0install/.lazyfs-cache/.control"

static ListHead dispatches = LIST_INIT;
static ListHead monitors = LIST_INIT;

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
	if (new_status == DBUS_DISPATCH_COMPLETE)
		return;

	list_prepend(&dispatches, connection);
}

static void send_task_update(DBusConnection *connection, Task *task)
{
	DBusMessage *message;

	assert(task->str);
	assert(task->child_task->str);

	task->notify_on_end = 1;

	message = dbus_message_new_signal("/Main", DBUS_Z_NS, "UpdateTask");

	if (message &&
	    dbus_message_append_args(message,
			DBUS_TYPE_STRING, task->str,
			DBUS_TYPE_STRING, task->child_task->str,
			DBUS_TYPE_INT64, (dbus_int64_t) task->child_task->size,
			DBUS_TYPE_INVALID) &&
	    dbus_connection_send(connection, message, NULL)) {
	} else {
		error("Out of memory");
	}

	if (message)
		dbus_message_unref(message);
}

static void send_task_error(DBusConnection *connection, Task *task)
{
	DBusMessage *message;

	assert(task->str);

	message = dbus_message_new_signal("/Main", DBUS_Z_NS, "Error");

	if (message &&
	    dbus_message_append_args(message,
			DBUS_TYPE_STRING, task->str,
			DBUS_TYPE_STRING, current_error,
			DBUS_TYPE_INVALID) &&
	    dbus_connection_send(connection, message, NULL)) {
	} else {
		error("Out of memory");
	}

	if (message)
		dbus_message_unref(message);
}
	
static void send_task_end(DBusConnection *connection, Task *task)
{
	DBusMessage *message;

	assert(task->str);

	message = dbus_message_new_signal("/Main", DBUS_Z_NS, "EndTask");

	if (message &&
	    dbus_message_append_args(message,
			DBUS_TYPE_STRING, task->str,
			DBUS_TYPE_INVALID) &&
	    dbus_connection_send(connection, message, NULL)) {
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

	if (list_contains(&monitors, connection)) {
		dbus_set_error_const(error, "Error", "Already monitoring!");
		return;
	}
	
	list_prepend(&monitors, connection);

	for (task = all_tasks; task; task = task->next) {
		if ((task->type == TASK_CLIENT || task->type == TASK_KERNEL) &&
		    task->child_task && task->uid == uid) {
			send_task_update(connection, task);
		}
	}
}

static DBusHandlerResult message_handler(DBusConnection *connection,
					 DBusMessage *message, void *user_data)
{
	DBusMessage *reply = NULL;
	DBusError error;

	dbus_error_init(&error);

	if (dbus_message_is_method_call(message, DBUS_Z_NS, "Refresh")) {
		dbus_refresh(connection, message, &error, 1);
		if (dbus_error_is_set(&error))
			goto err;
	} else if (dbus_message_is_method_call(message, DBUS_Z_NS, "Rebuild")) {
		dbus_refresh(connection, message, &error, 0);
		if (dbus_error_is_set(&error))
			goto err;
	} else if (dbus_message_is_method_call(message, DBUS_Z_NS, "Monitor")) {
		dbus_monitor(connection, &error);
		if (dbus_error_is_set(&error))
			goto err;
	} else if (dbus_message_is_method_call(message, DBUS_Z_NS, "Cancel")) {
		dbus_cancel_download(connection, message, &error);
		if (dbus_error_is_set(&error))
			goto err;
	} else
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (reply && !dbus_connection_send(connection, reply, NULL))
		goto err;

	goto out;
err:
	if (reply) {
		dbus_message_unref(reply);
		reply = NULL;
	}

	if (dbus_error_is_set(&error)) {
		reply = dbus_message_new_error(message, ZERO_INSTALL_ERROR, error.message);
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
	return DBUS_HANDLER_RESULT_HANDLED;
}

static dbus_bool_t allow_anyone_to_connect(DBusConnection *connection,
                                           unsigned long   uid,
                                           void           *data)
{
	return 1;
}

static DBusHandlerResult filter_func(DBusConnection *connection,
				     DBusMessage    *message,
				     void           *user_data)
{
	if (dbus_message_is_signal(message,
				DBUS_INTERFACE_ORG_FREEDESKTOP_LOCAL,
				"Disconnected"))
	{
		if (list_contains(&monitors, connection))
			list_remove(&monitors, connection);
		dbus_connection_disconnect(connection);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void new_dbus_client(DBusServer *server,
			    DBusConnection *new_connection,
			    void *data)
{
	const char *path[] = {"Main", NULL};

	if (!dbus_connection_set_watch_functions(new_connection,
				add_watch, remove_watch, NULL,
				NULL, NULL))
		goto err;

	dbus_connection_set_unix_user_function(new_connection,
			allow_anyone_to_connect, NULL, NULL);

	dbus_connection_set_dispatch_status_function(new_connection,
					dispatch_status_function, NULL, NULL);


	if (!dbus_connection_register_object_path(new_connection, path,
			&vtable, NULL)) {
		error("new_dbus_client: Out of memory");
		goto err;
	}

	if (!dbus_connection_add_filter(new_connection,
				filter_func, NULL, NULL)) {
		error("new_dbus_client: Out of memory");
		goto err;
	}

	dbus_connection_ref(new_connection);
	return;
err:
	/* Comment in bus.c suggests we need to do this; merely not
	 * taking a ref won't actually close it.
	 */
	dbus_connection_disconnect(new_connection);
	return;

}

void create_control_socket(void)
{
	DBusError error;
	const char *ext_only[] = {"EXTERNAL", NULL};

	list_init(&dispatches);
	list_init(&monitors);

	dbus_error_init(&error);
	server = dbus_server_listen(DBUS_SERVER_SOCKET, &error);
	if (!server) {
		error("Can't create DBUS server socket (%s): %s",
				DBUS_SERVER_SOCKET, error.message);
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

	unlink(OLD_SOCKET2);
	unlink(OLD_SOCKET);
}

static void dispatch_one(DBusConnection *connection, Task *unused)
{
	DBusDispatchStatus status;

	while (1) {
		status = dbus_connection_dispatch(connection);

		if (status == DBUS_DISPATCH_DATA_REMAINS)
			continue;

		if (status == DBUS_DISPATCH_COMPLETE)
			break;

		dbus_connection_disconnect(connection);

		/* error */
	}
}

int control_add_select(int n, fd_set *rfds, fd_set *wfds)
{
	DBusWatch *watch = dbus_watches;

	list_foreach(&dispatches, dispatch_one, 1, NULL);

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

	reply = dbus_message_new_method_return(task->message);
	if (!reply || !dbus_message_append_args(reply,
				DBUS_TYPE_BOOLEAN, 1,
				DBUS_TYPE_INVALID))
		error("Out of memory");

	if (!dbus_connection_send(task->connection, reply, NULL))
		error("Out of memory");

	if (reply)
		dbus_message_unref(reply);
}

static void send_result(Task *task, const char *err)
{
	if (!err)
		send_ok(task);
	else {
		DBusMessage *reply;
		reply = dbus_message_new_error(task->message,
				ZERO_INSTALL_ERROR,
				err);
		if (!reply || !dbus_connection_send(task->connection,
						reply, NULL))
			err = "Out of memory";
		if (reply)
			dbus_message_unref(reply);
	}	

	task_destroy(task, err);
}

/* 1 on success (kernel or client task exists) */
static int cancel_download(const char *request, uid_t uid)
{
	Task *task;

	for (task = all_tasks; task; task = task->next) {
		if ((task->type == TASK_CLIENT || task->type == TASK_KERNEL) &&
		    task->child_task && task->uid == uid &&
		    strcmp(task->str, request) == 0) {
			if (task->type == TASK_CLIENT)
				send_result(task, 0);
			else
				kernel_cancel_task(task);
			return 1;
		}
	}
	return 0;
}

static void dbus_cancel_download(DBusConnection *connection,
			DBusMessage *message, DBusError *error)
{
	unsigned long uid;
	const char *request = NULL;

	if (!dbus_message_get_args(message, error,
				DBUS_TYPE_STRING, &request, DBUS_TYPE_INVALID))
		return;

	if (!dbus_connection_get_unix_user(connection, &uid))
		assert(0);

	if (!cancel_download(request, uid)) {
		dbus_set_error_const(error, "Error", "Not being fetched");
	}
}

/* Message requests the cache for 'host' be refetched.
 * (force is 0 if we're rebuilding due to a changed override.xml)
 * Returns error, or NULL on success.
 */
static void dbus_refresh(DBusConnection *connection, DBusMessage *message,
			 DBusError *error, int force)
{
	char *site;
	Task *task = NULL;
	unsigned long uid;
	Index *index;

	if (!dbus_message_get_args(message, error,
				DBUS_TYPE_STRING, &site, DBUS_TYPE_INVALID))
		return;

	if (strchr(site, '/') || site[0] == '.' || !*site) {
		dbus_set_error_const(error, "Error", "Bad hostname");
		goto done;
	}

	task = task_new(TASK_CLIENT);
	if (!task)
		goto oom;
	task->step = send_result;
	if (!dbus_connection_get_unix_user(connection, &uid))
		assert(0);
	task->uid = uid;
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

	if (task->child_task) {
		control_notify_update(task);
	} else {
		dbus_set_error_const(error, "Error",
				"Failed to start fetching index");
		goto done;
	}

	free(site);
	return;
oom:
	dbus_set_error_const(error, "Error", "Out of memory");
done:
	if (task)
		task_destroy(task, 0);
	if (site)
		free(site);
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

void control_notify_update(Task *task)
{
	list_foreach(&monitors, send_task_update, 0, task);
}

void control_notify_end(Task *task)
{
	list_foreach(&monitors, send_task_end, 0, task);
}

void control_notify_error(Task *task, const char *message)
{
	assert(!current_error);
	current_error = message;
	list_foreach(&monitors, send_task_error, 0, task);
	current_error = NULL;
}

static void drop_monitors(DBusConnection *connection, Task *unused)
{
	dbus_connection_disconnect(connection);
}

void control_drop_clients(void)
{
	dbus_server_disconnect(server);
	dbus_server_unref(server);
	list_foreach(&monitors, drop_monitors, 1, NULL);
	list_destroy(&dispatches);
	list_destroy(&monitors);
}

static DBusObjectPathVTable vtable = {
	NULL,
	message_handler,
};
