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
		if (!old_tz) {
			fprintf(stderr, "Out of memory\n");
			exit(EXIT_FAILURE);
		}
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
	DBusConnection *connection;
	DBusMessage *message, *reply;
	DBusError error;

	dbus_error_init(&error);
	connection = dbus_connection_open(DBUS_SERVER_SOCKET, &error);
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

	printf("OK\n");
	dbus_message_unref(reply);
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
