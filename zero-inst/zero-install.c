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
#include "xml.h"

int copy_stderr = 1;	/* False once closed... */

int verbose = 0; /* (debug) */

/* When we need to use an index file, if it was created in the last hour
 * then don't bother to fetch it again. Otherwise, the overhead
 * of fetching the index again is pretty small compared to fetching the
 * archive.
 */
#define INDEX_CHECK_TIME (60 * 60)

char cache_dir[MAX_PATH_LEN];
int cache_dir_len;	/* strlen(cache_dir) */

static const char *prog; /* argv[0] */

static int finished = 0;

static int to_wakeup_pipe = -1;	/* Write here to get noticed */

static int open_helper(void)
{
	int helper;

	helper = open(MNT_DIR "/.lazyfs-helper", O_RDONLY);
	if (helper == -1) {
		int error = errno;

		error("Error opening " MNT_DIR "/.lazyfs-helper: %m");

		if (error == EACCES)
			error("Ensure that %s is owned "
				"by the user that runs %s before "
				MNT_DIR " is mounted.",
					cache_dir, prog);
		else if (error == EBUSY)
			error("Ensure that zero-install is not "
					"already running.");
		else
			error("Ensure that " MNT_DIR " is mounted.");

		exit(EXIT_FAILURE);
	}

	return helper;
}

/* Handle the top-level dir (0install) by marking it as dynamic */
static void handle_root_request(int request_fd)
{
	FILE *ddd;

	if (chdir(cache_dir))
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
	error("handle_root_request: Unable to write ... file: %m");
out:
	close(request_fd);
	chdir("/");
}

static void kernel_got_archive(Task *task, const char *err)
{
	if (err)
		control_notify_error(task, err);
	close(task->fd);
 	task_destroy(task, err);
}

/* We have the index to find the item for this task. Start fetching the
 * item. Free the index and close the request when done.
 */
static void kernel_got_index(Task *task)
{
	Element *item;
	const char *slash;

	assert(task->index);

	slash = strchr(task->str + 1, '/');

	if (slash)
		item = index_lookup(task->index, slash);
	else
		item = index_get_root(task->index);
	if (!item) {
		/* TODO: rebuild index files? */
		error("%s not found in index!", task->str);
		close(task->fd);
		task_destroy(task, "Item not found in index!");
		return;
	}

	if (item->name[0] == 'd')
		fetch_create_directory(task->str, item);
	else if (item->name[0] == 'l')
		error("Warning: '%s' is a link!", task->str);
	else {
		Element *group;

		group = item->parentNode;
		assert(group->name[0] == 'g');

		task->child_task = fetch_archive(task->str,
						 group, task->index);
		if (task->child_task) {
			task->step = kernel_got_archive;
			control_notify_update(task);
			return;
		}
	}

	close(task->fd);
	task_destroy(task, NULL);
}

void kernel_cancel_task(Task *task)
{
	close(task->fd);
	task_destroy(task, NULL);
}

static void kernel_task_step(Task *task, const char *err)
{
	if (!err)
		task_steal_index(task, get_index(task->str, NULL, 0));

	if (task->index)
		kernel_got_index(task);
	else {
		control_notify_error(task, err ? err : "Failed to get index");
		close(task->fd);
		task_destroy(task, NULL);
	}
}

static void handle_request(int request_fd, uid_t uid, char *path)
{
	Task *task;

	if (strcmp(path, "/") == 0) {
		handle_root_request(request_fd);
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
		task_destroy(task, "Out of memory");
		return;
	}
	task->step = kernel_task_step;

	task->uid = uid;
	task->fd = request_fd;

	task_steal_index(task, get_index(path, &task->child_task, 0));
	if (task->child_task) {
		assert(!task->index);
		control_notify_update(task);
		return;		/* Download in progress */
	}

	if (!task->index) {
		/* Error -- give up */
		close(request_fd);
		task_destroy(task, "Failed to start fetching index");
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
		error("lazyfs closed connection!");
		exit(EXIT_FAILURE);
	}

	if (len < 0) {
		error("Error reading from request pipe: %m");
		exit(EXIT_FAILURE);
	}

	if (len < 2 || buffer[len - 1] != '\0' || buffer[0] == '\0') {
		error("Internal error: bad request FD");
		exit(EXIT_FAILURE);
	}

	request_fd = strtol(buffer, &end, 10);
	if (strncmp(end, " uid=", 5) != 0 || !end[5]) {
		error("Internal error: bad request FD '%s'", buffer);
		exit(EXIT_FAILURE);
	}
	uid = strtol(end + 5, &end, 10);
	if (*end != '\0') {
		error("Internal error: bad request FD '%s'", buffer);
		exit(EXIT_FAILURE);
	}

	close_on_exec(request_fd, 1);

	len = read(request_fd, buffer, sizeof(buffer));

	if (len == -1 && errno == ENOTDIR) {
		/* No longer exists... not an error 
		 * (example: someone doing 'pwd' in an unlinked directory)
		 */
		close(request_fd);
		return;
	}

	if (len < 0) {
		error("read from request FD: %m");
		exit(EXIT_FAILURE);
	}

	if (len < 2 || buffer[len - 1] != '\0' || buffer[0] != '/') {
		error("Internal error: bad request");
		exit(EXIT_FAILURE);
	}

	handle_request(request_fd, uid, buffer);
}

static void read_from_wakeup(int wakeup)
{
	char buffer[40];
	int status;

	if (read(wakeup, buffer, sizeof(buffer)) < 0) {
		error("read_from_wakeup: %m");
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

static void create_pid_file(pid_t child)
{
	char *pid_file = NULL;
	FILE *file;

	pid_file = build_string("%s/.0inst-pid", cache_dir);
	if (!pid_file)
		goto err;

	file = fopen(pid_file, "w");
	if (!file)
		goto err;

	fprintf(file, "%ld\n", (long) child);
	if (fclose(file))
		goto err;

	free(pid_file);
	return;
err:
	if (pid_file)
		free(pid_file);
	error("Creating PID file: %m "
		"(unable to create zero-install.pid file!)");
	exit(EXIT_FAILURE);
}

#define CACHE_LINK MNT_DIR "/.lazyfs-cache"

int main(int argc, char **argv)
{
	int wakeup_pipe[2];
	struct sigaction act;
	int helper;
	int max_fd;
	char *pid_file;
	int background = 1;
	
	openlog("zero-install", 0, LOG_DAEMON);

	if (0) {
		Index *index = parse_index("/var/cache/zero-inst/localhost/.0inst-meta/index.new", 1, "foo");
		printf("%p\n", index);
		index_free(index);
		exit(0);
	}

	if (argv[1]) {
		if (strcmp(argv[1], "--debug") == 0) {
			verbose = 1;
			background = 0;
		} else if (strcmp(argv[1], "--nodaemon") == 0)
			background = 0;
	}

	umask(0022);
	
	prog = argv[0];

	cache_dir_len = readlink(CACHE_LINK, cache_dir, sizeof(cache_dir));
	if (cache_dir_len == -1) {
		error("readlink(" CACHE_LINK "): %m");
		error("Can't find location of cache directory."
			"Make sure " MNT_DIR " is mounted and that you are "
			"running the latest version of the lazyfs kernel "
			"module.");
		return EXIT_FAILURE;
	}
	assert(cache_dir_len >= 1 && cache_dir_len < sizeof(cache_dir));
	cache_dir[cache_dir_len] = '\0';

	syslog(LOG_INFO, "Started: using cache directory '%s'", cache_dir);

	if (0) {
		printf("Literal: %s\n", build_string("Hello world"));
		printf("Combine: %s\n", build_string("%s/%s", "one", "two"));
		printf("Dir    : %s\n", build_string("%d/%s", "one/two",
								"three"));
		printf("Percent: %s\n", build_string("%d/%%s", "one/two"));
		printf("Dot    : %s\n", build_string("%d/%r.tgz", "one/two",
							"index.xml"));
		printf("Cache  : %s\n", build_string("http://%c/foo",
						"/var/cache/zero-inst/bob"));
		printf("Host   : %s\n", build_string("http://%h/foo",
						"localhost.org/fred/bob"));
		printf("Host2  : %s\n", build_string("http://%h/foo",
						"localhost.org#~foo"));
		printf("Host3  : %s\n", build_string("http://%H/foo",
						"localhost.org#~foo"));

		//printf("Error  : %s\n", build_string("%d", "hello"));
		//printf("Error  : %s\n", build_string("%f", "hello"));
	}

	/* Ensure root is uptodate */
	handle_root_request(-1);

	helper = open_helper();

	/* When we get a signal, we can't do much right then. Instead,
	 * we send a char down this pipe, which causes the main loop to
	 * deal with the event next time we're idle.
	 */
	if (pipe(wakeup_pipe)) {
		error("pipe: %m");
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

	create_control_socket();

	if (wakeup_pipe[0] > helper)
		max_fd = wakeup_pipe[0];
	else
		max_fd = helper;

	if (background) {
		/* Daemon mode... */
		pid_t child;
		int null;

		child = fork();
		if (child == -1) {
			error("fork: %m");
			exit(EXIT_FAILURE);
		} else if (child) {
			waitpid(child, NULL, 0);
			_exit(0);
		}
		if (setsid() == -1)
			error("setsid: %m");
		child = fork();
		if (child == -1) {
			error("fork: %m");
			exit(EXIT_FAILURE);
		} else if (child) {
			create_pid_file(child);
			_exit(0);
		}
		if (chdir("/"))
			error("chdir: %m");
		null = open("/dev/null", O_RDWR);
		dup2(null, 0);
		dup2(null, 1);
		dup2(null, 2);
		close(null);
		copy_stderr = 0;
	} else
		create_pid_file(getpid());

	while (!finished) {
		fd_set rfds, wfds;
		int n = max_fd + 1;

		FD_ZERO(&rfds);
		FD_ZERO(&wfds);

		FD_SET(helper, &rfds);
		FD_SET(wakeup_pipe[0], &rfds);

		n = control_add_select(n, &rfds, &wfds);

		if (select(n, &rfds, &wfds, NULL, NULL) == -1) {
			if (errno == EINTR)
				continue;
			error("select: %m");
			exit(EXIT_FAILURE);
		}

		if (FD_ISSET(helper, &rfds))
			read_from_helper(helper);
		
		if (FD_ISSET(wakeup_pipe[0], &rfds))
			read_from_wakeup(wakeup_pipe[0]);
		
		control_check_select(&rfds, &wfds);
	}

	/* Doing a clean shutdown is mainly for valgrind's benefit */
	syslog(LOG_WARNING, "Got SIGINT. Terminating.");

	control_drop_clients();

	pid_file = build_string("%s/.0inst-pid", cache_dir);
	if (unlink(pid_file))
		error("unlink pid file: %m");
	free(pid_file);

	close(helper);

	closelog();

	return EXIT_SUCCESS;
}
