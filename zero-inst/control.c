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

/* Our connection to the system bus */
static DBusConnection *bus = NULL;

static DBusWatch *dbus_watches = NULL;

static DBusWatch *current_watch = NULL;	/* tmp */

static DBusObjectPathVTable vtable;

static void dbus_refresh(DBusConnection *connection, DBusMessage *message,
			 DBusError *error, int force);
static DBusMessage *handle_dbus_version(DBusConnection *connection,
			DBusMessage *message, DBusError *error);
static void dbus_cancel_download(DBusConnection *connection,
			DBusMessage *message, DBusError *error);

#define OLD_SOCKET "/uri/0install/.lazyfs-cache/control"
#define OLD_SOCKET2 "/uri/0install/.lazyfs-cache/.control"

static ListHead dispatches = LIST_INIT;

typedef struct _Monitor Monitor;

struct _Monitor {
	unsigned long uid;	/* Events to this user... */
	char *service;		/* ...should be sent here. */
	Monitor *next;
};

static Monitor *monitors = NULL;

/* Register a watcher for this user. Error if the user is already
 * registered.
 */
static void add_monitor(const char *service, unsigned long uid, DBusError *err)
{
	Monitor *next, *new;

	for (next = monitors; next; next = next->next) {
		if (next->uid == uid) {
			dbus_set_error_const(err, "AlreadyRegistered",
				"You already have a monitor registered");
			return;
		}
	}

	new = my_malloc(sizeof(Monitor));
	if (!new)
		goto oom;

	new->uid = uid;
	new->service = my_strdup(service);
	if (!new->service) {
		free(new);
		goto oom;
	}

	new->next = monitors;
	monitors = new;
	return;

oom:
	dbus_set_error_const(err, "Error", "Out of memory");
}

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

static void send_task_update(const char *service, Task *task)
{
	DBusMessage *message;

	assert(task->str);
	assert(task->child_task->str);

	task->notify_on_end = 1;

	message = dbus_message_new_signal("/ZeroInstall",
					DBUS_Z_NS, "UpdateTask");

	if (!message)
		goto oom;

	if (dbus_message_append_args(message,
			DBUS_TYPE_STRING, task->str,
			DBUS_TYPE_STRING, task->child_task->str,
			DBUS_TYPE_INT64, (dbus_int64_t) task->child_task->size,
			DBUS_TYPE_INVALID))
		goto oom;
	
	if (dbus_connection_send(bus, message, NULL))
		goto oom;

	if (message)
		dbus_message_unref(message);

	return;
oom:
	error("Out of memory");
}

static void send_task_error(const char *service, Task *task,
			    const char *error)
{
	DBusMessage *message;

	assert(task->str);

	message = dbus_message_new_signal("/Main", DBUS_Z_NS, "Error");

	if (message &&
	    dbus_message_append_args(message,
			DBUS_TYPE_STRING, task->str,
			DBUS_TYPE_STRING, message,
			DBUS_TYPE_INVALID) &&
	    dbus_connection_send(bus, message, NULL)) {
	} else {
		error("Out of memory");
	}

	if (message)
		dbus_message_unref(message);
}
	
static void send_task_end(const char *service, Task *task)
{
	DBusMessage *message;

	assert(task->str);

	message = dbus_message_new_signal("/Main", DBUS_Z_NS, "EndTask");

	if (message &&
	    dbus_message_append_args(message,
			DBUS_TYPE_STRING, task->str,
			DBUS_TYPE_INVALID) &&
	    dbus_connection_send(bus, message, NULL)) {
	} else {
		error("Out of memory");
	}

	if (message)
		dbus_message_unref(message);
}

static DBusMessage *handle_dbus_monitor(DBusConnection *connection,
			 DBusMessage *message, DBusError *error)
{
	DBusMessage *reply = NULL;
	Task *task;
	unsigned long uid;
	const char *sender;

	sender = dbus_message_get_sender(message);
	if (!sender)
	{
		error("Can't get message sender!");
		return NULL;
	}

	uid = dbus_bus_get_unix_user(connection, sender, error);

	if (dbus_error_is_set(error)) {
		error("Can't get UID: %s", error->message);
		return NULL;
	}

	error("Monitor request from %s for %ld", sender, uid);

	add_monitor(sender, uid, error);
	if (dbus_error_is_set(error))
		return NULL;

	for (task = all_tasks; task; task = task->next) {
		if ((task->type == TASK_CLIENT || task->type == TASK_KERNEL) &&
		    task->child_task && task->uid == uid) {
			send_task_update(sender, task);
		}
	}

	reply = dbus_message_new_method_return(message);
	if (!reply)
		dbus_set_error_const(error, "Error", "Out of memory");
	return reply;
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
		reply = handle_dbus_monitor(connection, message, &error);
		if (dbus_error_is_set(&error))
			goto err;
	} else if (dbus_message_is_method_call(message, DBUS_Z_NS, "Cancel")) {
		dbus_cancel_download(connection, message, &error);
		if (dbus_error_is_set(&error))
			goto err;
	} else if (dbus_message_is_method_call(message, DBUS_Z_NS, "Version")) {
		reply = handle_dbus_version(connection, message, &error);
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
		reply = dbus_message_new_error(message,
					ZERO_INSTALL_ERROR, error.message);
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

static DBusHandlerResult filter_func(DBusConnection *connection,
				     DBusMessage    *message,
				     void           *user_data)
{
	if (dbus_message_is_signal(message,
				DBUS_INTERFACE_ORG_FREEDESKTOP_LOCAL,
				"Disconnected"))
	{
		error("Zero Install: lost connection to system bus");

		dbus_connection_disconnect(connection);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void create_control_socket(void)
{
	DBusError error;
	list_init(&dispatches);

	dbus_error_init(&error);
	bus = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
	if (!bus) {
		error("Can't connect to D-BUS system bus: %s", error.message);
		dbus_error_free(&error);
		exit(EXIT_FAILURE);
	}

	dbus_connection_set_dispatch_status_function(bus,
					dispatch_status_function, NULL, NULL);

	if (!dbus_connection_set_watch_functions(bus,
				add_watch, remove_watch, NULL, NULL, NULL)) {
		error("Out of memory");
		exit(EXIT_FAILURE);
	}

#if 0
	if (!dbus_connection_set_timeout_functions (connection,
				add_timeout,
				remove_timeout,
				NULL,
				NULL, NULL))
		goto nomem;
#endif

	dispatch_status_function(bus,
			dbus_connection_get_dispatch_status(bus), NULL);

	if (!dbus_connection_add_filter(bus,
				filter_func, NULL, NULL)) {
		error("dbus_connection_add_filter: Out of memory");
		exit(EXIT_FAILURE);
	}


	error("Zero Install: Connected to D-BUS system bus as %s",
			dbus_bus_get_base_service(bus));

	dbus_bus_acquire_service(bus, DBUS_ZEROINSTALL_SERVICE, 0, &error);

	if (dbus_error_is_set(&error))
	{
		error("Can't aquire Zero Install D-BUS service %s: %s",
				DBUS_ZEROINSTALL_SERVICE, error.message);
		dbus_error_free(&error);
		exit(EXIT_FAILURE);
	}

	if (!dbus_connection_register_object_path(bus, "/ZeroInstall",
						  &vtable, NULL)) {
		error("dbus_connection_register_object_path: Out of memory");
		exit(EXIT_FAILURE);
	}

	unlink(DBUS_SERVER_SOCKET);
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

/* This task is about to be cancelled. If nothing else depends on its child
 * task, kill the child task too.
 */
static void may_kill_child(Task *parent)
{
	Task *task;
	Task *child = parent->child_task;

	for (task = all_tasks; task; task = task->next) {
		if (task != parent && task->child_task == child) {
			error("Not cancelling download; another "
			      "user wants '%s' too", parent->str);
			return;
		}
	}
	error("Cancelling download for '%s'", parent->str);
	if (child->child_pid == -1)
		error("Child process already exited!");
	else
		kill(child->child_pid, SIGTERM);
}

/* 1 on success (kernel or client task exists) */
static int cancel_download(const char *request, uid_t uid)
{
	Task *task;

	for (task = all_tasks; task; task = task->next) {
		if ((task->type == TASK_CLIENT || task->type == TASK_KERNEL) &&
		    task->child_task && task->uid == uid &&
		    strcmp(task->str, request) == 0) {

			may_kill_child(task);

			if (task->type == TASK_CLIENT)
				send_result(task,
					    "Cancelled at user's request");
			else
				kernel_cancel_task(task);

			fetch_set_auto_reject(request, uid);

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

	if (!dbus_connection_get_unix_user(connection, &uid)) {
		assert(0);
		abort();
	}

	if (!cancel_download(request, uid)) {
		dbus_set_error_const(error, "Error", "Not being fetched");
	}
}

static DBusMessage *handle_dbus_version(DBusConnection *connection,
			DBusMessage *message, DBusError *error)
{
	DBusMessage *reply = NULL;

	if (!dbus_message_get_args(message, error, DBUS_TYPE_INVALID))
		return NULL;

	reply = dbus_message_new_method_return(message);
	if (!reply || !dbus_message_append_args(reply,
				DBUS_TYPE_STRING, VERSION,
				DBUS_TYPE_INVALID)) {
		dbus_set_error_const(error, "Error", "Out of memory");
		if (reply)
			dbus_message_unref(reply);
	}

	return reply;
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
		task_destroy(task, error ? error->message : NULL);
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
	Monitor *next;

	for (next = monitors; next; next = next->next) {
		if (task->uid == next->uid)
			send_task_update(next->service, task);
	}
}

void control_notify_end(Task *task)
{
	Monitor *next;

	for (next = monitors; next; next = next->next) {
		if (task->uid == next->uid)
			send_task_end(next->service, task);
	}
}

void control_notify_error(Task *task, const char *message)
{
	Monitor *next;

	for (next = monitors; next; next = next->next) {
		if (task->uid == next->uid)
			send_task_error(next->service, task, message);
	}
}

void control_drop_clients(void)
{
	dbus_connection_disconnect(bus);
	dbus_connection_unref(bus);
	bus = NULL;
	//list_foreach(&monitors, drop_monitors, 1, NULL);
	list_destroy(&dispatches);
	//list_destroy(&monitors);
}

static DBusObjectPathVTable vtable = {
	NULL,
	message_handler,
};
