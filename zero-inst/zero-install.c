/*
 * Zero Install -- user space helper
 *
 * Copyright (C) 2003  Thomas Leonard
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

/* When accessing files from the /uri filesystem, the lazyfs kernel module
 * can satisfy all requests for resources that have already been cached.
 * If a resource is requested which is not in the cache, this program is
 * used to fetch it.
 *
 * On startup, we open to the /uri/.lazyfs-helper pipe and read requests
 * from it. Each read gives a file handle which represents the request.
 * We read the name of the missing resource from the new file handle and
 * fetch the file. When we've finished handling a request (successful or
 * otherwise) we close the request handle.
 *
 * The requesting application will be awoken when this happens, and will
 * either get the file (if we cached it) or an error, if it still doesn't
 * exist.
 *
 * We may also be asked to cache something that's already in the cache, if
 * it doesn't match its directory entry (wrong type, size, etc), so we may
 * have to delete things.
 *
 * If several users try to get the same uncached file at once we will get
 * one request for each user. This is so that users can see and cancel their
 * own requests without affecting other users.
 */

#include <utime.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/param.h>
#include <sys/select.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <assert.h>

#include "global.h"
#include "support.h"
#include "control.h"
#include "fetch.h"
#include "index.h"
#include "zero-install.h"
#include "task.h"

/* When we need to use an index file, if it was created in the last hour
 * then don't bother to fetch it again. Otherwise, the overhead
 * of fetching the index again is pretty small compared to fetching the
 * archive.
 */
#define INDEX_CHECK_TIME (60 * 60)

char cache_dir[MAX_PATH_LEN];

static const char *prog; /* argv[0] */

static int finished = 0;

Request *open_requests = NULL;

static int to_wakeup_pipe = -1;	/* Write here to get noticed */

static int open_helper(void)
{
	int helper;

	helper = open(URI "/.lazyfs-helper", O_RDONLY);
	if (helper == -1) {
		int error = errno;

		perror("Error opening " URI "/.lazyfs-helper");

		if (error == EACCES)
			fprintf(stderr, "\nEnsure that %s is owned \n"
					"by the user that runs %s before "
					URI " is mounted.\n",
					cache_dir, prog);
		else
			fprintf(stderr, "\nEnsure that " URI " is mounted.\n");

		exit(EXIT_FAILURE);
	}

	return helper;
}

static void handle_root_request(int request_fd)
{
	FILE *ddd;
	long now;

	now = time(NULL);

	if (chdir(cache_dir))
		goto err;

	ddd = fopen("....", "w");
	if (!ddd)
		goto err;
	fprintf(ddd, "LazyFS\n"
		"d 0 %ld http%c"
		"d 0 %ld ftp%c"
		"d 0 %ld https%c",
		now, 0, now, 0, now, 0);
	if (fclose(ddd))
		goto err;

	if (rename("....", "..."))
		goto err;
	fprintf(stderr, "Write root ... file\n");
	goto out;
err:
	perror("handle_root_request");
	fprintf(stderr, "Unable to write root ... file\n");
out:
	close(request_fd);
	chdir("/");
}

/* Handle one of the top-level dirs (http, ftp, etc) by marking it as
 * dynamic.
 */
static void handle_toplevel_request(int request_fd, const char *dir)
{
	FILE *ddd;

	if (chdir(cache_dir))
		goto err;

	if (mkdir(dir, 0755) && errno != EEXIST)
		goto err;
	if (chdir(dir))
		goto err;

	ddd = fopen("....", "w");
	if (!ddd)
		goto err;
	fprintf(ddd, "LazyFS Dynamic\n");
	if (fclose(ddd))
		goto err;
	if (rename("....", "..."))
		goto err;
	goto out;
err:
	perror("handle_toplevel_request");
	fprintf(stderr, "Unable to write %s ... file\n", dir);
out:
	close(request_fd);
	chdir("/");

	sleep(1);
	fprintf(stderr, "Done\n");
}

static void kernel_got_archive(Task *task, int success)
{
	control_notify_user(task->uid);
	close(task->fd);
	task_destroy(task, 0);
}

/* We have the index to find the item for this task. Start fetching the
 * item. Free the index and close the request when done.
 */
static void kernel_got_index(Task *task)
{
	xmlNode *item;
	const char *slash;

	assert(task->index);

	control_notify_user(task->uid);

	slash = strchr(task->str + 1, '/');
	slash = strchr(slash + 1, '/');

	if (slash)
		item = index_lookup(task->index, slash);
	else
		item = index_get_root(task->index);
	if (!item) {
		/* TODO: rebuild index files? */
		printf("%s not found in index!\n", task->str);
		close(task->fd);
		task_destroy(task, 0);
		return;
	}

	if (item->name[0] == 'd')
		fetch_create_directory(task->str, item);
	else {
		xmlNode *archive;

		archive = index_find_archive(item);
		assert(archive);

		task->child_task = fetch_archive(task->str,
						 archive, task->index);
		if (task->child_task) {
			task->step = kernel_got_archive;
			return;
		}
	}

	close(task->fd);
	task_destroy(task, 1);
}

static void kernel_task_step(Task *task, int success)
{
	control_notify_user(task->uid);

	if (success)
		task_steal_index(task, get_index(task->str, NULL, 0));

	if (task->index)
		kernel_got_index(task);
	else {
		close(task->fd);
		task_destroy(task, 0);
		return;
	}
}

static void handle_request(int request_fd, uid_t uid, char *path)
{
	Task *task;
	char *slash;

	if (strcmp(path, "/") == 0) {
		handle_root_request(request_fd);
		return;
	}

	slash = strrchr(path, '/');
	if (slash == path) {
		handle_toplevel_request(request_fd, path + 1);
		return;
	}
	task = task_new(TASK_KERNEL);
	if (!task) {
		close(request_fd);
		return;
	}
	task_set_string(task, path);
	if (!task->str) {
		close(request_fd);
		task_destroy(task, 0);
		return;
	}
	task->step = kernel_task_step;

	task->uid = uid;
	task->fd = request_fd;

	task_steal_index(task, get_index(path, &task->child_task, 0));
	if (task->child_task) {
		assert(!task->index);
		control_notify_user(uid);
		return;		/* Download in progress */
	}

	if (!task->index) {
		/* Error -- give up */
		close(request_fd);
		task_destroy(task, 0);
		return;
	}

	kernel_got_index(task);
}

/* This is called as a signal handler; simply ensures that
 * child_died_callback() will get called later.
 */
static void child_died(int signum)
{
	write(to_wakeup_pipe, "\0", 1);	/* Wake up! */
}

static void sigint(int signum)
{
	finished = 1;
	write(to_wakeup_pipe, "\0", 1);	/* Wake up! */
}

static void read_from_helper(int helper)
{
	char buffer[MAXPATHLEN + 1];
	char *end;
	int len, request_fd;
	uid_t uid;

	len = read(helper, buffer, sizeof(buffer));
	if (len == 0) {
		fprintf(stderr, "lazyfs closed connection!");
		exit(EXIT_FAILURE);
	}

	if (len < 0) {
		perror("Error reading from request pipe");
		exit(EXIT_FAILURE);
	}

	if (len < 2 || buffer[len - 1] != '\0' || buffer[0] == '\0') {
		fprintf(stderr, "Internal error: bad request FD\n");
		exit(EXIT_FAILURE);
	}

	request_fd = strtol(buffer, &end, 10);
	if (strncmp(end, " uid=", 5) != 0 || !end[5]) {
		fprintf(stderr,
				"Internal error: bad request FD '%s'\n",
				buffer);
		exit(EXIT_FAILURE);
	}
	uid = strtol(end + 5, &end, 10);
	if (*end != '\0') {
		fprintf(stderr,
				"Internal error: bad request FD '%s'\n",
				buffer);
		exit(EXIT_FAILURE);
	}

	len = read(request_fd, buffer, sizeof(buffer));

	if (len < 2 || buffer[len - 1] != '\0' || buffer[0] != '/') {
		fprintf(stderr, "Internal error: bad request\n");
		exit(EXIT_FAILURE);
	}

	handle_request(request_fd, uid, buffer);
}

static void read_from_wakeup(int wakeup)
{
	char buffer[40];
	int status;

	if (read(wakeup, buffer, sizeof(buffer)) < 0) {
		perror("read_from_wakeup");
		exit(EXIT_FAILURE);
	}

	while (1)
	{
		pid_t child;

		child = waitpid(-1, &status, WNOHANG);

		if (child == 0 || child == -1)
			return;

		task_process_done(child,
			WIFEXITED(status) && WEXITSTATUS(status) == 0);
	}
}

#define CACHE_LINK "/uri/.lazyfs-cache"

int main(int argc, char **argv)
{
	int wakeup_pipe[2];
	struct sigaction act;
	int helper;
	int control_socket;
	int max_fd;
	int len;

	index_init();

	if (0)
	{
		Index *index = parse_index("/var/www/0install-index.xml");
		if (index) {
			index_dump(index);
			index_free(index);
		}
		index_shutdown();
		return 0;
	}

	umask(0022);
	
	prog = argv[0];

	len = readlink(CACHE_LINK, cache_dir, sizeof(cache_dir));
	if (len == -1) {
		perror("readlink(" CACHE_LINK ")");
		fprintf(stderr, "\nCan't find location of cache directory.\n"
			"Make sure /uri is mounted and that you are running\n"
			"the latest version of the lazyfs kernel module.\n");
		return EXIT_FAILURE;
	}
	assert(len >= 1 && len < sizeof(cache_dir));
	cache_dir[len] = '\0';
	printf("Zero Install started: using cache directory '%s'\n", cache_dir);

	helper = open_helper();

	/* When we get a signal, we can't do much right then. Instead,
	 * we send a char down this pipe, which causes the main loop to
	 * deal with the event next time we're idle.
	 */
	if (pipe(wakeup_pipe)) {
		perror("pipe");
		return EXIT_FAILURE;
	}
	to_wakeup_pipe = wakeup_pipe[1];

	/* If the pipe is full then we're going to get woken up anyway... */
	set_blocking(to_wakeup_pipe, 0);

	/* Let child processes die */
	act.sa_handler = child_died;
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_NOCLDSTOP;
	sigaction(SIGCHLD, &act, NULL);

	/* Catch SIGINT and exit nicely */
	act.sa_handler = sigint;
	sigemptyset(&act.sa_mask);
	act.sa_flags = SA_ONESHOT;
	sigaction(SIGINT, &act, NULL);

	control_socket = create_control_socket();

	if (wakeup_pipe[0] > helper)
		max_fd = wakeup_pipe[0];
	else
		max_fd = helper;
	if (control_socket > max_fd)
		max_fd = control_socket;

	while (!finished) {
		fd_set rfds, wfds;
		int n = max_fd + 1;

		control_push_updates();

		FD_ZERO(&rfds);
		FD_ZERO(&wfds);

		FD_SET(helper, &rfds);
		FD_SET(wakeup_pipe[0], &rfds);
		FD_SET(control_socket, &rfds);

		n = control_add_select(n, &rfds, &wfds);

		if (select(n, &rfds, &wfds, NULL, NULL) == -1) {
			if (errno == EINTR)
				continue;
			perror("select");
			exit(EXIT_FAILURE);
		}

		if (FD_ISSET(helper, &rfds))
			read_from_helper(helper);
		
		if (FD_ISSET(wakeup_pipe[0], &rfds))
			read_from_wakeup(wakeup_pipe[0]);
		
		if (FD_ISSET(control_socket, &rfds))
			read_from_control(control_socket);

		control_check_select(&rfds, &wfds);
	}

	/* Doing a clean shutdown is mainly for valgrind's benefit */
	printf("%s: Got SIGINT... terminating...\n", prog);

	control_drop_clients();

	close(helper);

	return EXIT_SUCCESS;
}
