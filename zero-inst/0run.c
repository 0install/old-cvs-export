#define _XOPEN_SOURCE /* glibc2 needs this */
#define _BSD_SOURCE /* To control timezone info */
#include <time.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define ZERO_MNT "/uri/0install"

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

static void force_fetch(char *path)
{
	pid_t child;

	child = fork();
	if (child == -1) {
		perror("fork");
		exit(EXIT_FAILURE);
	}

	if (child == 0) {
		char *slash;

		slash = strchr(path, '/');
		if (slash)
			*slash = '\0';
		execlp("0refresh", "0refresh", path, NULL);
		perror("execlp(0refresh)");
		_exit(1);
	}

	if (waitpid(child, NULL, 0) != child) {
		perror("waitpid");
		exit(EXIT_FAILURE);
	}
}

int main(int argc, char **argv)
{
	struct stat info;
	time_t mtime;
	char *path;
	char *date;
	int len;

	if (argc < 2) {
		fprintf(stderr, "Usage: /bin/0run \"program time\" [args]\n\n"
			"Run 'program' (a pathname under " ZERO_MNT ").\n"
			"If the mtime is earlier than 'time' (GMT), force a\n"
			"refresh (and abort on failure). 'args' are passed\n"
			"to the program unmodifed.\n"
			"If 'program' is a directory, then program/AppRun is\n"
			"assumed.\n\n"
			"Example:\n"
			"#!/bin/0run python.org/python 2003-01-01\n"
			);
		return EXIT_FAILURE;
	}

	date = strrchr(argv[1], ' ');
	if (!date) {
		fprintf(stderr, "No date given (use a quoted space)\n");
		return EXIT_FAILURE;
	}
	*date = '\0';
	date++;

	len = sizeof(ZERO_MNT) + strlen(argv[1]) + 1;
	path = malloc(len);
	if (!path)
		goto oom;
	if (snprintf(path, len, "%s%s",
			argv[1][0] == '/' ? "" : ZERO_MNT "/",
			argv[1]) >= len)
		abort();

	if (strncmp(path, ZERO_MNT "/", sizeof(ZERO_MNT)) != 0) {
		fprintf(stderr, "Path '%s' is not under " ZERO_MNT "!\n",
				path);
		exit(EXIT_FAILURE);
	}

	mtime = parse_date(date);

	if (stat(path, &info) != 0 || info.st_mtime < mtime) {
		force_fetch(path + sizeof(ZERO_MNT));
		if (stat(path, &info) != 0 || info.st_mtime < mtime) {
			fprintf(stderr, "Failed to update '%s' to date '%s'\n",
					path, date);
			exit(EXIT_FAILURE);
		}
	}

	if (S_ISDIR(info.st_mode))
	{
		char *old = path;
		int alen;

		alen = strlen(old) + sizeof("/AppRun");
		path = malloc(alen);
		if (!path) {
			fprintf(stderr, "Out of memory\n");
			exit(EXIT_FAILURE);
		}

		if (snprintf(path, alen, "%s/AppRun", old) != alen - 1)
			abort();

		free(old);
	}

	argv[1] = path;
	execv(path, argv + 1);

	perror("execv");
	exit(127);
oom:
	fprintf(stderr, "Out of memory\n");
	return EXIT_FAILURE;
}
