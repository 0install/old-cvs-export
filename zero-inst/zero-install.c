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
 * it doesn't match its directory entries (wrong type, size, etc), so we may
 * have to delete things.
 *
 * If several users try to get the same uncached file at once we will get
 * one request for each user. This is so that users can see and cancel their
 * own requests without affecting other users.
 */

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

#include "support.h"
#include "control.h"
#include "fetch.h"
#include "zero-install.h"

#define MAX_URI_LEN 4096
#define MAX_PATH_LEN 4096

const char *cache_dir = "/var/cache/zero-inst";

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

static void finish_request(Request *request)
{
	int i;

	printf("Closing request in %s\n", request->path);

	if (request == open_requests) {
		open_requests = request->next;
	} else {
		Request *next;

		for (next = open_requests;; next = next->next) {
			if (!next) {
				fprintf(stderr,
					"finish_request: Internal error\n");
				exit(EXIT_FAILURE);
			}
				
			if (next->next == request) {
				next->next = request->next;
				break;
			}
		}
	}
	request->next = NULL;

	for (i = 0; i < request->n_users; i++) {
		printf("  Closing request %d for %s\n",
				request->users[i].fd,
				request->users[i].leaf);
		close(request->users[i].fd);
		free(request->users[i].leaf);

		control_notify_user(request->users[i].uid);
	}

	free(request->users);
	free(request->path);
	free(request);
}

static Request *request_new(const char *path)
{
	Request *request;

	request = my_malloc(sizeof(Request));
	if (!request)
		return NULL;

	request->n_users = 0;
	request->child_pid = -1;
	request->path = NULL;
	request->users = NULL;
	request->state = READY;

	request->path = my_strdup(path);
	if (!request->path)
		goto err;
	request->users = my_malloc(sizeof(UserRequest));
	if (!request->users)
		goto err;

	return request;
err:
	if (request->path)
		free(request->path);
	free(request);
	return NULL;
}

static void request_add_user(Request *request, int fd, uid_t uid,
				const char *leafname)
{
	UserRequest *new;
	char *leaf;

	new = my_realloc(request->users,
		      sizeof(UserRequest) * (request->n_users + 1));
	if (!new) {
		close(fd);
		return;
	}

	leaf = my_strdup(leafname);
	if (!leaf) {
		close(fd);
		return;
	}

	request->users = new;
	new[request->n_users].fd = fd;
	new[request->n_users].uid = uid;
	new[request->n_users].leaf = leaf;

	request->n_users++;

	control_notify_user(uid);
}

static Request *find_request(const char *path)
{
	Request *next = open_requests;

	while (next) {
		if (strcmp(next->path, path) == 0)
			return next;
		next = next->next;
	}

	return NULL;
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

#if 0
static void close_all_fds(void)
{
	Request *next;

	for (next = open_requests; next; next = next->next) {
		int i;

		for (i = 0; i < next->n_users; i++) {
			close(next->users[i].fd);
		}
	}
}
#endif

static int ensure_dir(const char *path)
{
	struct stat info;

	assert(strncmp(path, cache_dir, strlen(cache_dir)) == 0);
	
	if (lstat(path, &info) == 0) {
		if (S_ISDIR(info.st_mode))
			return 1;	/* Already exists */
		fprintf(stderr, "%s should be a directory... unlinking!\n",
				path);
		unlink(path);
	}

	if (mkdir(path, 0755)) {
		perror("mkdir");
		fprintf(stderr, "(while creating %s)\n", path);
		return 0;
	}

	return 1;
}

static void wget(Request *request, const char *uri, char *path,
		 int use_cache)
{
	const char *argv[] = {"wget", "-q", "-O", path, uri, NULL};
	char *slash;

	printf("Fetch '%s'\n", uri);

	if (request->child_pid != -1) {
		fprintf(stderr, "calling wget: Internal error\n");
		exit(EXIT_FAILURE);
	}

	slash = strrchr(path, '/');
	if (!slash) {
		fprintf(stderr, "calling wget: Internal error II\n");
		exit(EXIT_FAILURE);
	}

	*slash = '\0';
	if (!ensure_dir(path))
		return;
	*slash = '/';

	request->child_pid = fork();
	if (request->child_pid == -1) {
		perror("fork");
		return;
	} else if (request->child_pid)
		return;

	execvp(argv[0], (char **) argv);

	perror("Trying to run wget: execvp");
	_exit(1);
}

/* /http/www.foo.org/some/path, one, two to
 * http://www.foo.org/some/path/one/two
 *
 * The two 'leaf' components are appended, if non-NULL.
 * 0 on error, which will have been already reported.
 */
static int build_uri(char *buffer, int len, const char *path,
		     const char *leaf1, const char *leaf2)
{
	int n;
	char *slash;

	assert(path[0] == '/');
	path++;

	slash = strchr(path, '/');
	assert(slash != NULL && slash != path);

	n = slash - path;	/* Length of protocol (http=4) */
	if (n > len)
		goto too_big;
	memcpy(buffer, path, n);
	path += n;
	buffer += n;
	len -= n;

	if (snprintf(buffer, len, ":/%s%s%s%s%s", path,
			leaf1 ? "/" : "", leaf1 ? leaf1 : "",
			leaf2 ? "/" : "", leaf2 ? leaf2 : "") > len - 1)
		goto too_big;

	return 1;
too_big:
	fprintf(stderr, "Path '%s' too long to convert to URI\n", path);
	return 0;
}

/* Either do finish_request or start a child process and advance state */
static void request_ensure_running(Request *request)
{
	UserRequest *first_rq = request->users;
	char uri[MAX_URI_LEN];
	char path[MAX_PATH_LEN];
	int err;
	
	if (request->child_pid != -1)
		goto err;

	if (request->n_users == 0) {
		fprintf(stderr, "request_ensure_running: Internal error\n");
		exit(EXIT_FAILURE);
	}

	if (request->state == FETCHING_INDEX) {
		printf("[ process index ]\n");
		if (snprintf(path, sizeof(path),
			"%s%s/%s/", cache_dir, request->path,
			first_rq->leaf) > sizeof(path) - 1) {
			fprintf(stderr, "Path too long\n");
			goto err;
		}
		build_ddd_from_index(path);
		goto err;
	}

	if (request->state == FETCHING_ARCHIVE) {
		fprintf(stderr, "Got archive!\n");
		exit(EXIT_SUCCESS);
	}

	if (!strchr(request->path + 1, '/')) {
		/* The root of a site (eg, /http/zero-install.sf.net) */

		if (strcmp(first_rq->leaf, "AppRun") == 0 ||
		    strcmp(first_rq->leaf, ".DirIcon") == 0)
			goto err;
		
		if (snprintf(uri, sizeof(uri),
			"%s://%s/" ZERO_INST_INDEX, request->path + 1,
			first_rq->leaf) > sizeof(uri) - 1) {
			fprintf(stderr, "URI too long\n");
			goto err;
		}
		if (snprintf(path, sizeof(path),
			"%s%s/%s/" ZERO_INST_INDEX, cache_dir, request->path,
			first_rq->leaf) > sizeof(path) - 1) {
			fprintf(stderr, "Path too long\n");
			goto err;
		}

		request->state = FETCHING_INDEX;
		wget(request, uri, path, 0);
		if (!request->child_pid)
			goto err;
		return;
	}

	/* It's not the root directory of a site, but a resource within it.
	 * Read the index file to find out what kind of object it is and
	 * how to get it.
	 */
	/* TODO: check the index is up-to-date */

	if (snprintf(path, sizeof(path), "%s%s/" ZERO_INST_INDEX, cache_dir,
			request->path) > sizeof(path) - 1) {
		fprintf(stderr, "Path too long\n");
		goto err;
	}
	printf("[ check index '%s' ]\n", path);
	/* Find out what kind of thing it is */

	err = get_item_info(path, first_rq->leaf, uri, sizeof(uri));
	if (!err) {
		request->state = FETCHING_ARCHIVE;
		if (snprintf(path, sizeof(path),
			"%s%s/%s/archive.tgz" , cache_dir, request->path, 
			first_rq->leaf) > sizeof(path) - 1) {
			fprintf(stderr, "Path too long\n");
			goto err;
		}

		wget(request, uri, path, 0);
		if (!request->child_pid)
			goto err;
		return;
	} else if (err != EISDIR)
		goto err;

	/* It's a directory */
	if (snprintf(path, sizeof(path), "%s%s/%s" , cache_dir,
			request->path, first_rq->leaf) > sizeof(path) - 1) {
		fprintf(stderr, "Path too long\n");
		goto err;
	}
	if (!ensure_dir(path))
		goto err;

	/* Fetch subdirectory index */
	if (!build_uri(uri, sizeof(uri), request->path, first_rq->leaf,
				ZERO_INST_INDEX))
		goto err;
	if (snprintf(path, sizeof(path),
		"%s%s/%s/" ZERO_INST_INDEX, cache_dir, request->path,
		first_rq->leaf) > sizeof(path) - 1) {
		fprintf(stderr, "Path too long\n");
		goto err;
	}

	request->state = FETCHING_INDEX;
	wget(request, uri, path, 0);
	if (!request->child_pid)
		goto err;
	return;
err:
	finish_request(request);
}

static void handle_request(int request_fd, uid_t uid, char *path)
{
	Request *request;
	char *slash;

	printf("Request %d: Fetch '%s' for user %ld\n", request_fd,
			path, (long) uid);

	if (strcmp(path, "/") == 0) {
		handle_root_request(request_fd);
		return;
	}

	slash = strrchr(path, '/');
	if (slash == path) {
		handle_toplevel_request(request_fd, path + 1);
		return;
	}
	*slash = '\0';

	request = find_request(path);
	if (!request) {
		request = request_new(path);
		if (!request) {
			fprintf(stderr, "%s: Out of memory!\n", prog);
			close(request_fd);
			return;
		}
		request->next = open_requests;
		open_requests = request;
	}

	request_add_user(request, request_fd, uid, slash + 1);
	if (request->n_users == 0) {
		finish_request(request);
		return;
	}

	request_ensure_running(request);
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

static void request_child_finished(Request *request)
{
	request->child_pid = -1;

	printf("[ process completed - continue with request ]\n");
	request_ensure_running(request);
}

static void read_from_wakeup(int wakeup)
{
	char buffer[40];

	if (read(wakeup, buffer, sizeof(buffer)) < 0) {
		perror("read_from_wakeup");
		exit(EXIT_FAILURE);
	}

	while (1)
	{
		Request *next;
		pid_t child;

		child = waitpid(-1, NULL, WNOHANG);

		if (child == 0 || child == -1)
			return;

		for (next = open_requests;; next = next->next) {
			if (!next) {
				fprintf(stderr,
					"Unknown child %ld!\n", (long) child);
				break;
			}
			if (next->child_pid == child) {
				request_child_finished(next);
				break;
			}
		}
	}
}

int main(int argc, char **argv)
{
	int wakeup_pipe[2];
	struct sigaction act;
	int helper;
	int control_socket;
	int max_fd;

	umask(0022);
	
	prog = argv[0];

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

	close(helper);

	return EXIT_SUCCESS;
}
