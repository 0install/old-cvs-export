#define _XOPEN_SOURCE /* glibc2 needs this */
#define _BSD_SOURCE /* To control timezone info */
#include <time.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/un.h>
#include <unistd.h>

#define ZERO_MNT "/uri/0install"
#define MAX_PATH_LEN 4096

static time_t parse_date(const char *str)
{
	struct tm tm_date;
	const char *end;
	char *old_tz;
	time_t retval = 0;

	/* Surely there's a saner way to force GMT? */
	old_tz = getenv("TZ");
	if (old_tz) {
		char *tmp = old_tz;
		old_tz = malloc(strlen(tmp) + 1);
		strcpy(old_tz, tmp);
	}
	setenv("TZ", "UTC", 1);
	
	gmtime_r(&retval, &tm_date);	/* Zero fields */

	end = strptime(str, "%Y-%m-%d,%R", &tm_date);
	if (!end)
		end = strptime(str, "%Y-%m-%d", &tm_date);

	if (!end || *end) {
		fprintf(stderr,
			"Invalid date '%s' (should be YYYY-MM-DD[,HH:MM])\n",
			str);
		exit(EXIT_FAILURE);
	}

	retval = mktime(&tm_date);

	if (old_tz) {
		setenv("TZ", old_tz, 1);
		free(old_tz);
	} else {
		unsetenv("TZ");
	}

	return retval;
}

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

/* Display output from socket until next newline (may overread) */
static void read_reply(int socket)
{
	int done = 0;

	while (!done) {
		char buffer[256];
		int got;
		int i;

		got = recv(socket, buffer, sizeof(buffer) - 1, 0);
		if (got < 0)
			perror("recv");
		if (got <= 0)
			break;
		for (i = 0; i < got; i++)
			if (buffer[i] == '\0') {
				done = 1;
				buffer[i] = '\n';
			}
		write(1, buffer, got);
	}
}

static void usage(const char *prog, int status)
{
	printf("Usage:\n"
		"%s\t\t\t(refresh site containing current directory)\n"
		"%s site\t\t\t(refresh given site)\n"
		"%s site/path date\t(refresh 'site' if 'path' is\n"
		"\t\t\t\t missing, or older than 'date')\n"
		"\n"
		"Example: %s python.org/python2.2 2003-01-01\n\n"
		"This checks that " ZERO_MNT "/python.org/python2.2 \n"
		"exists and has a modification time after Jan 1st,\n"
		"2003, and forces a refresh if not.\n",
		prog, prog, prog, prog);

	exit(status);
}

static void refresh(const char *site, int force)
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
		exit(EXIT_FAILURE);
	}

	control = socket(AF_UNIX, SOCK_STREAM, AF_UNIX);
	if (control == -1) {
		perror("socket");
		exit(EXIT_FAILURE);
	}
	if (connect(control, (struct sockaddr *) &addr, sizeof(addr)) == -1) {
		perror("connect");
		fprintf(stderr,
			"Can't connect to Zero Install helper application.\n"
			"Is it running?\n");
		exit(EXIT_FAILURE);
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
		exit(EXIT_FAILURE);
	}

	send_command(control, force ? "REFRESH " : "REBUILD ");

	send_command(control, site);
	send_command(control, "\n");

	read_reply(control);
}

static int uptodate(const char *path, time_t mtime)
{
	struct stat info;

	if (stat(path, &info) != 0)
		return 0;

	return info.st_mtime >= mtime;
}

int main(int argc, char **argv)
{
	char path[MAX_PATH_LEN];

	if (argc == 2 && strcmp(argv[1], "--help") == 0)
		usage(argv[0], EXIT_SUCCESS);

	if (argc == 1) {
		/* 0refresh */
		char *site, *slash;

		if (!getcwd(path, sizeof(path))) {
			fprintf(stderr, "getcwd() failed\n");
			return EXIT_FAILURE;
		}
		if (strncmp(path, ZERO_MNT "/", sizeof(ZERO_MNT)) != 0) {
			fprintf(stderr, "'%s' not under " ZERO_MNT ".\n", path);
			return EXIT_FAILURE;
		}

		site = path + sizeof(ZERO_MNT);
		slash = strchr(site, '/');
		if (slash)
			*slash = '\0';
		refresh(site, 1);
	} else if (argc == 2) {
		/* 0refresh site */
		refresh(argv[1], 1);
	} else if (argc == 3 && strcmp(argv[1], "-l") == 0) {
		/* 0refresh -l site */
		refresh(argv[2], 0);
	} else if (argc == 3) {
		/* 0refresh site/path date */
		time_t mtime = 0;
		char *slash;
		
		mtime = parse_date(argv[2]);

		snprintf(path, sizeof(path), ZERO_MNT "/%s", argv[1]);

		if (uptodate(path, mtime))
			return EXIT_SUCCESS;

		slash = strchr(argv[1], '/');
		if (slash)
			*slash = '\0';
		refresh(argv[1], 1);
		if (slash)
			*slash = '/';

		if (uptodate(path, mtime))
			return EXIT_SUCCESS;

		fprintf(stderr,
			"0refresh: failed to update '%s' to date '%s' (GMT)\n",
			path, argv[2]);
		return EXIT_FAILURE;
	} else
		usage(argv[0], EXIT_FAILURE);

	return EXIT_SUCCESS;
}
