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

int create_control_socket(void)
{
	int control;
	struct sockaddr_un addr;
	struct sigaction act;
	
	addr.sun_family = AF_UNIX;
	if (snprintf(addr.sun_path, sizeof(addr.sun_path),
		     "%s/control", cache_dir) >= sizeof(addr.sun_path)) {
		fprintf(stderr, "Control socket path too long!\n");
		exit(EXIT_FAILURE);
	}

	/* Ignore SIGPIPE - check for EPIPE errors instead */
	act.sa_handler = SIG_IGN;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	sigaction(SIGPIPE, &act, NULL);

	control = socket(PF_UNIX, SOCK_STREAM, PF_UNIX);
	if (control == -1) {
		perror("Failed to create control socket");
		exit(EXIT_FAILURE);
	}

	unlink(addr.sun_path);
	if (bind(control, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
		perror("Failed to bind control socket");
		fprintf(stderr, "(trying to create socket %s)\n",
				addr.sun_path);
		exit(EXIT_FAILURE);
	}
	if (chmod(addr.sun_path, 0777)) {
		perror("chmod");
		fprintf(stderr, "(trying to give access to socket %s)\n",
				addr.sun_path);
	}

	if (listen(control, 5) == -1) {
		perror("Failed to listen on control socket");
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
		fprintf(stderr, "client_push_update: Internal error (%d,%d)\n",
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
		perror("Error accepting control connection");
		return;
	}

	client = my_malloc(sizeof(Client));
	if (!client) {
		close(fd);
		return;
	}

	if (verbose)
		printf("New client %d\n", fd);

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
		perror("Can't set socket to get credentials");
		exit(EXIT_FAILURE);
	}
}

int control_add_select(int n, fd_set *rfds, fd_set *wfds)
{
	Client *next;

	for (next = clients; next; next = next->next) {
		FD_SET(next->socket, rfds);
		if (next->socket >= n)
			n = next->socket + 1;
		if (next->to_send)
			FD_SET(next->socket, wfds);
	}
	
	return n;
}

static void client_free(Client *client)
{
	if (verbose)
		printf("Lost client %d\n", client->socket);
	close(client->socket);
	client->next = NULL;
	if (client->to_send) {
		if (verbose)
			printf("(discarding buffered messages)\n");
		free(client->to_send);
	}

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

/* Client requests the cache for 'host' be refetched */
static void client_refresh(Client *client, const char *host)
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
	client->task->str = my_malloc(strlen(host) + 2);
	if (!client->task->str) {
		client_send_reply(client, "Out of memory");
		return;
	}
	sprintf(client->task->str, "/%s", host);

	index = get_index(client->task->str, &client->task->child_task, 1);
	assert(!index);

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
		client_refresh(client, command + 8);
	} else {
		fprintf(stderr, "Unknown command '%s' from client %d\n",
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
			fprintf(stderr,
				"Failed to get credentials from client\n");
			return -1;
		}

		cmsg = CMSG_FIRSTHDR(&msg);
		if (!cmsg) {
			fprintf(stderr, "No control message attached "
				"to client greeting\n");
			return -1;
		}

		if (cmsg->cmsg_level != SOL_SOCKET ||
		    cmsg->cmsg_type != SCM_CREDENTIALS) {
			fprintf(stderr,
				"Control message not SCM_CREDENTIALS\n");
			return -1;
		}

		client->task = task_new(TASK_CLIENT);
		if (!client->task)
			return 1;	/* OOM */
		client->task->uid = ((struct ucred *) CMSG_DATA(cmsg))->uid;
		client->task->data = client;
		client->task->step = client_step;

		if (verbose)
			printf("Client authenticated as user %ld\n",
					(long) client->task->uid);

		return 0;
	}

	current_len = strlen(client->command);

	if (current_len >= sizeof(client->command) - 1) {
		fprintf(stderr, "Command too long from %d\n", client->socket);
		return -1;
	}

	got = read(client->socket, client->command + current_len,
			sizeof(client->command) - 1 - current_len);
	if (got <= 0) {
		if (got < 0)
			perror("Reading from client");
		else if (verbose)
			printf("Client closed connection\n");
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
		fprintf(stderr, "write_to_client: Internal error\n");
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
}
