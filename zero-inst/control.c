#include <sys/types.h>
#include <sys/socket.h>
#include <signal.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "control.h"
#include "support.h"
#include "zero-install.h"

typedef struct _Client Client;

struct _Client {
	int socket;
	int have_uid;
	uid_t uid;

	int need_update;

	int send_offset;
	char *to_send;

	Client *next;
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

	if (listen(control, 5) == -1) {
		perror("Failed to listen on control socket");
		exit(EXIT_FAILURE);
	}

	set_blocking(control, 0);

	return control;
}

static void client_push_update(Client *client)
{
	Request *request;
	size_t len = 0;
	char *buf;
	int i;

	client->need_update = 1;

	if (client->to_send)
		return;

	len += sizeof("UPDATE\nEND\n");	/* (final \0 at end) */
	for (request = open_requests; request; request = request->next) {
		for (i = 0; i < request->n_users; i++) {
			if (request->users[i].uid != client->uid)
				continue;
			len += strlen(request->path) + 1 +
				strlen(request->users[i].leaf) + 1;
		}
	}
	client->to_send = my_malloc(len);
	if (!client->to_send)
		return;

	buf = client->to_send;
	buf += sprintf(buf, "UPDATE\n");
	for (request = open_requests; request; request = request->next) {
		for (i = 0; i < request->n_users; i++) {
			if (request->users[i].uid != client->uid)
				continue;
			buf += sprintf(buf, "%s/%s\n", request->path,
					request->users[i].leaf);
		}
	}
	buf += sprintf(buf, "END\n");
	*buf = '\0';
	buf++;
	if (buf - client->to_send != len) {
		fprintf(stderr, "client_push_update: Internal error (%d,%d)\n",
				buf - client->to_send, len);
		exit(EXIT_FAILURE);
	}

	client->send_offset = 0;
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

	printf("New client %d\n", fd);

	client->socket = fd;
	client->have_uid = 0;
	client->send_offset = 0;
	client->to_send = NULL;
	client->next = clients;
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
	printf("Lost client %d\n", client->socket);
	close(client->socket);
	client->next = NULL;
	free(client);
}

static int read_from_client(Client *client)
{
	char buffer[256];

	if (!client->have_uid) {
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

		client->have_uid = 1;
		client->uid = ((struct ucred *) CMSG_DATA(cmsg))->uid;

		printf("Client authenticated as user %ld\n",
				(long) client->uid);

		client_push_update(client);

		return 0;
	}

	return -1;
#if 0
	got = read(client->socket, buffer, sizeof(buffer) - 1);
	if (got <= 0) {
		if (got < 0)
			perror("Reading from client");
		return -1;
	}

	buffer[got] = '\0';

	printf("Got '%s' from %d\n", buffer, client->socket);

	return 0;
#endif
}

static int write_to_client(Client *client)
{
	int sent;
	char *data = client->to_send + client->send_offset;

	if (!client->to_send) {
		fprintf(stderr, "write_to_client: Internal error\n");
		return -1;
	}

	sent = write(client->socket, data, strlen(data));
	if (sent <= 0)
		return -1;

	client->send_offset += sent;
	if (client->to_send[client->send_offset] == '\0') {
		free(client->to_send);
		client->to_send = NULL;

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
		if (client->have_uid && client->uid == uid)
			client_push_update(client);
	}
}
