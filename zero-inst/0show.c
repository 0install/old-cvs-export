#include <sys/socket.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/un.h>
#include <unistd.h>

static void send_command(int socket, const char *message)
{
	while (*message) {
		int sent;
		sent = send(socket, message, strlen(message), 0);

		if (sent <= 0) {
			perror("send");
			exit(EXIT_FAILURE);
		}
		message += sent;
	}
}

int main(int argc, char **argv)
{
	int control;
	struct sockaddr_un addr;
	struct msghdr msg = {0};
	struct iovec vec[1];
	char c = '\n';
	char buffer[CMSG_SPACE(sizeof(struct ucred))];
	struct cmsghdr *cmsg;
	struct ucred *cred;

	addr.sun_family = AF_UNIX;
	if (snprintf(addr.sun_path, sizeof(addr.sun_path),
	    "/uri/0install/.lazyfs-cache/control") >= sizeof(addr.sun_path)) {
		fprintf(stderr, "Control socket path too long!\n");
		return 1;
	}

	control = socket(AF_UNIX, SOCK_STREAM, AF_UNIX);
	if (control == -1) {
		perror("socket");
		return 1;
	}
	if (connect(control, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
		perror("connect");
		fprintf(stderr,
			"Can't connect to Zero Install helper application.\n"
			"Is it running?\n");
		return 1;
	}

	vec[0].iov_base = &c;
	vec[0].iov_len = 1;

	msg.msg_name = NULL;
	msg.msg_namelen = 0;
	msg.msg_iov = vec;
	msg.msg_iovlen = 1;
	msg.msg_control = buffer;
	msg.msg_controllen = sizeof(buffer);
	msg.msg_flags = 0;

	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_CREDENTIALS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(struct ucred));

	cred = (struct ucred *) CMSG_DATA(cmsg);
	cred->pid = getpid();
	cred->uid = getuid();
	cred->gid = getgid();

	msg.msg_controllen = cmsg->cmsg_len;
	
	
	if (sendmsg(control, &msg, 0) != 1) {
		perror("sendmsg");
		return 1;
	}

	send_command(control, "MONITOR\n");

	while (1) {
		char buffer[256];
		int got;
		int i;

		got = recv(control, buffer, sizeof(buffer) - 1, 0);
		if (got < 0)
			perror("recv");
		if (got <= 0)
			break;
		for (i = 0; i < got; i++)
			if (buffer[i] == '\0')
				buffer[i] = '\n';
		write(1, buffer, got);
	}

	return 0;
}
