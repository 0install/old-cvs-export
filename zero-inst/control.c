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

typedef struct _Client Client;

struct _Client {
	int socket;
	int monitor;
	int terminate;

	int need_update;
	int update_flagged;

	int send_offset;
	char *to_send;
	int send_len;

	char command[10 + MAX_PATH_LEN];	/* Command being read */

	Task	*task;	/* NULL => not auth'd */

	Client	*next;
};

static Client *clients = NULL;
static DBusWatch *dbus_watches = NULL;
static DBusConnection *dispatches = NULL;
static dbus_int32_t next_dispatch = -1;
static DBusMessageHandler *handler = NULL;
static DBusServer *server = NULL;

static DBusWatch *current_watch = NULL;	/* tmp */

static void dbus_refresh(DBusConnection *connection, DBusMessage *message,
			 DBusError *error, int force);

#define SERVER_SOCKET "unix:path=/uri/0install/.lazyfs-cache/.control"

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

static DBusHandlerResult message_handler(DBusMessageHandler *handler,
	DBusConnection *connection, DBusMessage *message, void *user_data)
{
	DBusMessage *reply = NULL;
	DBusError error;
	const char *name = dbus_message_get_name(message);

	printf("[ message '%s' ]\n", name);

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
	} else if (strcmp(name, "net.sourceforge.zero-install.Refresh") == 0) {
		dbus_refresh(connection, message, &error, 1);
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

int create_control_socket(void)
{
	int control;
	struct sockaddr_un addr;
	struct sigaction act;
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

	addr.sun_family = AF_UNIX;
	if (snprintf(addr.sun_path, sizeof(addr.sun_path),
		     "%s/control", cache_dir) >= sizeof(addr.sun_path)) {
		error("Control socket path too long!");
		exit(EXIT_FAILURE);
	}

	/* Ignore SIGPIPE - check for EPIPE errors instead */
	act.sa_handler = SIG_IGN;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	sigaction(SIGPIPE, &act, NULL);

	control = socket(PF_UNIX, SOCK_STREAM, PF_UNIX);
	if (control == -1) {
		error("Failed to create control socket: %m");
		exit(EXIT_FAILURE);
	}

	unlink(addr.sun_path);
	if (bind(control, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
		error("Failed to bind control socket: %m "
			"(trying to create socket %s)", addr.sun_path);
		exit(EXIT_FAILURE);
	}
	if (chmod(addr.sun_path, 0777)) {
		error("chmod: %m, (trying to give access to socket %s)",
				addr.sun_path);
	}

	if (listen(control, 5) == -1) {
		error("Failed to listen on control socket: %m");
		exit(EXIT_FAILURE);
	}

	set_blocking(control, 0);

	return control;
}

static void client_push_update(Client *client)
{
	Task *task;
	size_t len = 0;
	char *buf;
	char buffer[20];

	assert(client->task);

	client->need_update = 1;

	if (client->terminate || !client->monitor)
		return;

	if (client->to_send) {
		if (verbose)
			printf("\t(update already in progress; deferring)\n");
		return;
	}

	len += sizeof("UPDATE_END");	/* (final \0 at end) */

	for (task = all_tasks; task; task = task->next) {
		if ((task->type == TASK_CLIENT || task->type == TASK_KERNEL) &&
				task->child_task &&
				task->uid == client->task->uid) {
			assert(task->str);
			assert(task->child_task->str);
			/* The path of the request */
			len += strlen(task->str) + 1;
			len += strlen(task->child_task->str) + 1;
			len += snprintf(buffer, sizeof(buffer), "%ld",
					task->child_task->size) + 1;
		}
	}

	client->to_send = my_malloc(len);
	if (!client->to_send)
		return;

	buf = client->to_send;
	buf += sprintf(buf, "UPDATE%c", 0);

	for (task = all_tasks; task; task = task->next) {
		if ((task->type == TASK_CLIENT || task->type == TASK_KERNEL) &&
				task->child_task &&
				task->uid == client->task->uid) {
			buf += sprintf(buf, "%s%c%s%c%ld%c",
					task->str, 0,
					task->child_task->str, 0,
				      	task->child_task->size, 0);
		}
	}

	buf += sprintf(buf, "END") + 1;	/* (includes \0) */
	if (buf - client->to_send != len) {
		error("client_push_update: Internal error (%d,%d)",
				buf - client->to_send, len);
		exit(EXIT_FAILURE);
	}

	client->send_offset = 0;
	client->send_len = len;
	client->need_update = 0;
}

void read_from_control(int control)
{
	Client *client;
	int fd;
	int one = 1;

	fd = accept(control, NULL, 0);
	if (fd == -1) {
		error("Error accepting control connection: %m");
		return;
	}

	client = my_malloc(sizeof(Client));
	if (!client) {
		close(fd);
		return;
	}

	syslog(LOG_INFO, "New client %d", fd);

	client->socket = fd;
	client->monitor = 0;
	client->send_offset = 0;
	client->task = NULL;
	client->to_send = NULL;
	client->command[0] = '\0';
	client->next = clients;
	client->terminate = 0;
	client->update_flagged = 0;
	client->need_update = 0;
	clients = client;

	if (setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &one, sizeof(one))) {
		error("Can't set socket to get credentials: %m");
		exit(EXIT_FAILURE);
	}
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
	Client *next;
	DBusWatch *watch = dbus_watches;

	while (dispatches)
		dispatch_pending();

	for (next = clients; next; next = next->next) {
		FD_SET(next->socket, rfds);
		if (next->socket >= n)
			n = next->socket + 1;
		if (next->to_send)
			FD_SET(next->socket, wfds);
	}

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

static void client_free(Client *client)
{
	if (verbose)
		syslog(LOG_INFO, "Lost client %d", client->socket);
	close(client->socket);
	client->next = NULL;
	if (client->to_send)
		free(client->to_send);

	if (client->task)
		task_destroy(client->task, 0);

	free(client);
}

/* Add 'message' to the client's output buffer. On OOM, terminate connection.
 * A '\0' will be added to the end of the message.
 */
static void client_send_reply(Client *client, const char *message)
{
	int old_len = client->to_send ? client->send_len : 0;
	int message_len = strlen(message) + 1;
	char *new;

	new = my_realloc(client->to_send, old_len + message_len);
	if (!new) {
		client->terminate = 1;
		return;
	}
	
	client->to_send = new;
	client->send_len = old_len + message_len;
	
	memcpy(new + old_len, message, message_len - 1);
	new[client->send_len - 1] = '\0';
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

/* Client requests the cache for 'host' be refetched.
 * (force is 0 if we're rebuilding due to a changed override.xml)
 */
static void client_refresh(Client *client, const char *host, int force)
{
	Index *index;

	if (strchr(host, '/') || host[0] == '.' || !*host) {
		client_send_reply(client, "Bad hostname");
		return;
	}

	if (client->task->child_task) {
		client_send_reply(client, "Busy with another request!");
		return;
	}

	task_set_string(client->task, NULL);
	client->task->str = build_string("/%s", host);
	if (!client->task->str) {
		client_send_reply(client, "Out of memory");
		return;
	}

	index = get_index(client->task->str, &client->task->child_task, force);
	if (index) {
		/* Already cached, and we didn't force a refresh.
		 * Rebuild site, as override.xml may have changed.
		 */
		if (build_ddds_for_site(index, host)) {
			client_send_reply(client, "OK");
		} else {
			client_send_reply(client, "Error");
		}
		index_free(index);
		return;
	}

	if (client->task->child_task)
		control_notify_user(client->task->uid);
	else
		client_send_reply(client, "Error");
}

static void client_do_command(Client *client, const char *command)
{
	if (client->terminate)
		return;

	if (strcmp(command, "MONITOR") == 0) {
		client->monitor = 1;
		client_push_update(client);
	} else if (strncmp(command, "REFRESH ", 8) == 0) {
		client_refresh(client, command + 8, 1);
	} else if (strncmp(command, "REBUILD ", 8) == 0) {
		client_refresh(client, command + 8, 0);
	} else {
		error("Unknown command '%s' from client %d",
				command, client->socket);
		client_send_reply(client, "Bad command");
		client->terminate = 1;
	}
}

/* Execute each complete command in client->command, removing them
 * as we go. If client is set to terminate, does nothing.
 */
static void client_do_commands(Client *client)
{
	char *nl;

	while ((nl = strchr(client->command, '\n'))) {
		*nl = '\0';
		client_do_command(client, client->command);
		memmove(client->command, nl + 1, strlen(nl + 1) + 1);
	}
}

static void client_step(Task *task, int success)
{
	control_notify_user(task->uid);
	client_send_reply((Client *) task->data, success ? "OK" : "FAIL");
}

static int read_from_client(Client *client)
{
	int got;
	int current_len;

	if (!client->task) {
		char buffer[256];
		struct msghdr msg;
		struct iovec vec[1];
		struct cmsghdr *cmsg;
		char c;
		
		vec[0].iov_base = &c;
		vec[0].iov_len = 1;

		msg.msg_name = NULL;
		msg.msg_namelen = 0;
		msg.msg_iov = vec;
		msg.msg_iovlen = 1;
		msg.msg_control = buffer;
		msg.msg_controllen = sizeof(buffer);
		msg.msg_flags = 0;
		if (recvmsg(client->socket, &msg, 0) != 1) {
			error("Failed to get credentials from client");
			return -1;
		}

		cmsg = CMSG_FIRSTHDR(&msg);
		if (!cmsg) {
			error("No control message attached "
				"to client greeting");
			return -1;
		}

		if (cmsg->cmsg_level != SOL_SOCKET ||
		    cmsg->cmsg_type != SCM_CREDENTIALS) {
			error("Control message not SCM_CREDENTIALS");
			return -1;
		}

		client->task = task_new(TASK_CLIENT);
		if (!client->task)
			return 1;	/* OOM */
		client->task->uid = ((struct ucred *) CMSG_DATA(cmsg))->uid;
		client->task->data = client;
		client->task->step = client_step;

		syslog(LOG_INFO, "New client for user %ld",
				(long) client->task->uid);

		return 0;
	}

	current_len = strlen(client->command);

	if (current_len >= sizeof(client->command) - 1) {
		error("Command too long from %d", client->socket);
		return -1;
	}

	got = read(client->socket, client->command + current_len,
			sizeof(client->command) - 1 - current_len);
	if (got <= 0) {
		if (got < 0)
			error("Reading from client: %m");
		else if (verbose)
			syslog(LOG_INFO, "Client closed connection");
		return -1;
	}

	client->command[current_len + got] = '\0';

	client_do_commands(client);

	if (client->terminate && !client->to_send)
		return -1;

	return 0;
}

static int write_to_client(Client *client)
{
	int sent;
	char *data = client->to_send + client->send_offset;

	if (!client->to_send) {
		error("write_to_client: Internal error");
		return -1;
	}

	sent = write(client->socket, data,
			client->send_len - client->send_offset);
	if (sent <= 0)
		return -1;

	client->send_offset += sent;
	assert(client->send_offset <= client->send_len);

	if (client->send_offset == client->send_len) {
		free(client->to_send);
		client->to_send = NULL;

		if (client->terminate)
			return -1;

		if (client->need_update)
			client_push_update(client);
	}

	return 0;
}

void control_check_select(fd_set *rfds, fd_set *wfds)
{
	Client *client = clients;
	Client *next, *prev = NULL;

	while (client) {
		next = client->next;
		if ((FD_ISSET(client->socket, rfds) && read_from_client(client))
				||
		    (FD_ISSET(client->socket, wfds) && write_to_client(client)))
		{
			if (prev)
				prev->next = next;
			else
				clients = next;
			client_free(client);
			client = NULL;
		}
		if (client)
			prev = client;
		client = next;
	}

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
	Client *client;

	for (client = clients; client; client = client->next) {
		if (client->task && client->task->uid == uid)
			client->update_flagged = 1;
	}
}

/* Do all updates for clients flagged with control_notify_user() */
void control_push_updates(void)
{
	Client *client;
	for (client = clients; client; client = client->next) {
		if (client->update_flagged) {
			client->update_flagged = 0;
			client_push_update(client);
		}
	}
}

void control_drop_clients(void)
{
	Client *client = clients;
	while (client) {
		Client *old = client;
		client = client->next;

		client_free(old);
	}

	dbus_connection_free_data_slot(&next_dispatch);
	dbus_message_handler_unref(handler);
	dbus_server_disconnect(server);
	dbus_server_unref(server);
}
