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

#define DBUS_API_SUBJECT_TO_CHANGE
#include <dbus/dbus.h>

#define MAX_PATH_LEN 4096

#include "interface.h"

/* This is changed only when debugging */
static const char *mnt_dir = "/uri/0install";
static int mnt_dir_len = 0;

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
		if (!old_tz)
			goto oom;
		strcpy(old_tz, tmp);
	} else
		old_tz = NULL;

	if (setenv("TZ", "UTC", 1))
		goto oom;
	
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
		if (setenv("TZ", old_tz, 1))
			goto oom;
		free(old_tz);
	} else {
		unsetenv("TZ");
	}

	return retval;
oom:
	fprintf(stderr, "Out of memory\n");
	exit(EXIT_FAILURE);
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
		"This checks that %s/python.org/python2.2 \n"
		"exists and has a modification time after Jan 1st,\n"
		"2003, and forces a refresh if not.\n",
		prog, prog, prog, prog, mnt_dir);

	exit(status);
}

#define DBUS_SERVER_SOCKET_PRE "unix:path="
#define DBUS_SERVER_SOCKET_POST "/.lazyfs-cache/.control2"

static void refresh(const char *site, int force)
{
	DBusConnection *connection;
	DBusMessage *message, *reply;
	DBusError error;
	char *server_socket;
	int server_socket_len;

	server_socket_len = sizeof(DBUS_SERVER_SOCKET_PRE) - 1 +
			    mnt_dir_len +
			    sizeof(DBUS_SERVER_SOCKET_POST);
	server_socket = malloc(server_socket_len);
	if (server_socket == NULL) {
		fprintf(stderr, "Out of memory");
		exit(EXIT_FAILURE);
	}

	if (snprintf(server_socket, server_socket_len,
			DBUS_SERVER_SOCKET_PRE "%s" DBUS_SERVER_SOCKET_POST,
			mnt_dir) + 1 != server_socket_len) {
		abort();
	}

	dbus_error_init(&error);
	connection = dbus_connection_open(server_socket, &error);
	free(server_socket);
	if (dbus_error_is_set(&error)) {
		fprintf(stderr,
			"Can't connect to Zero Install helper application:\n"
			"%s\nIs zero-install running?\n", error.message);
		dbus_error_free(&error);
		exit(EXIT_FAILURE);
	}

	message = dbus_message_new_method_call(NULL, "/Main",
			DBUS_Z_NS, force ? "Refresh" : "Rebuild");
	if (!message) {
		fprintf(stderr, "Out of memory\n");
		exit(EXIT_FAILURE);
	}
	if (!dbus_message_append_args(message, DBUS_TYPE_STRING, site,
				 DBUS_TYPE_INVALID)) {
		fprintf(stderr, "Out of memory\n");
		exit(EXIT_FAILURE);
	}

	reply = dbus_connection_send_with_reply_and_block(connection, message,
				5 * 60 * 1000, &error);
	dbus_message_unref(message);
	if (reply && dbus_set_error_from_message(&error, reply)) {
		dbus_message_unref(reply);
		reply = NULL;
	}

	if (!reply) {
		fprintf(stderr, "%s: %s\n", error.name, error.message);
		dbus_error_free(&error);
		exit(EXIT_FAILURE);
	}

	dbus_message_unref(reply);
	dbus_connection_disconnect(connection);
	dbus_connection_unref(connection);
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

	/* For debugging, allow overriding /uri/0install */
	{
		char *uri_0install;

		uri_0install = getenv("DEBUG_URI_0INSTALL_DIR");
		if (uri_0install) {
			mnt_dir = uri_0install;
		}
		mnt_dir_len = strlen(mnt_dir);
	}

	if (argc == 2 && strcmp(argv[1], "--help") == 0)
		usage(argv[0], EXIT_SUCCESS);

	if (argc == 1) {
		/* 0refresh */
		char *site, *slash;

		if (!getcwd(path, sizeof(path))) {
			fprintf(stderr, "getcwd() failed\n");
			return EXIT_FAILURE;
		}
		if (strncmp(path, mnt_dir, mnt_dir_len) != 0 ||
		    path[mnt_dir_len] != '/') {
			fprintf(stderr, "'%s' not under %s.\n", path, mnt_dir);
			return EXIT_FAILURE;
		}

		site = path + mnt_dir_len + 1;
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

		if (snprintf(path, sizeof(path), ZERO_MNT "/%s", argv[1]) >= sizeof(path)) {
			fprintf(stderr, "Path too long\n");
			return EXIT_FAILURE;
		}

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

	printf("OK\n");
	return EXIT_SUCCESS;
}
