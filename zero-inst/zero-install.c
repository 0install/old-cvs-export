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

/* Static prototypes */
//static void request_next_step(Request *request);

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

#if 0
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
		if (request->users[i].fd != -1)
			close(request->users[i].fd);
		free(request->users[i].leaf);

		control_notify_user(request->users[i].uid);
	}

	if (request->index)
		index_free(request->index);
	if (request->current_download_path)
		free(request->current_download_path);
	free(request->users);
	free(request->path);
	free(request);
}

/* Create a new Request object. There is one of these for each directory
 * we are handling, possibly containing multiple requests within that
 * dir. The new Request will be in the READY state with no actual fetches,
 * and not in the global requests list yet.
 * NULL on error (out of memory, already reported).
 */
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
	request->index = NULL;

	request->current_download_archive = NULL;
	request->current_download_path = NULL;

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
#endif

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
/* Begins fetching 'uri', storing the file as 'path'.
 * Sets request->child_pid and request->current_download_path.
 * Clears request->current_download_archive.
 * On error, request->child_pid will still be -1.
 */
static void wget(Request *request, const char *uri, const char *path,
		 int use_cache)
{
	const char *argv[] = {"wget", "-q", "-O", NULL, uri,
			use_cache ? NULL : "--cache=off", NULL};
	char *slash;

	printf("\t(fetch '%s')\n", uri);

	assert(request->child_pid == -1);

	request->current_download_archive = NULL;

	if (request->current_download_path)
		free(request->current_download_path);
	request->current_download_path = my_strdup(path);
	if (!request->current_download_path)
		return;
	argv[3] = request->current_download_path;

	slash = strrchr(request->current_download_path, '/');
	assert(slash);

	*slash = '\0';
	if (!ensure_dir(request->current_download_path))
		goto err;
	*slash = '/';

	request->child_pid = fork();
	if (request->child_pid == -1) {
		perror("fork");
		goto err;
	} else if (request->child_pid)
		return;

	execvp(argv[0], (char **) argv);

	perror("Trying to run wget: execvp");
	_exit(1);
err:
	if (request->current_download_path) {
		free(request->current_download_path);
		request->current_download_path = NULL;
	}
}

static void request_done_head(Request *request)
{
	int i;

	assert(request->n_users > 0);

	printf("\t(finished '%s')\n", request->users[0].leaf);

	control_notify_user(request->users[0].uid);

	if (request->users[0].fd != -1)
		close(request->users[0].fd);
	free(request->users[0].leaf);

	request->n_users--;
	for (i = 0; i < request->n_users; i++)
		request->users[i] = request->users[i + 1];
}
#endif

#if 0
static void fetched_subindex(Request *request)
{
	Index *subindex;
	char *path = request->current_download_path;

	assert(request->state == FETCHING_SUBINDEX);
	assert(path);
	assert(strcmp(path + strlen(path) - sizeof(ZERO_INST_INDEX) + 1,
		      ZERO_INST_INDEX) == 0);

	subindex = parse_index(path);
	if (!subindex)
		return;
	utime(path, NULL);

	request->current_download_path = NULL;

	path[strlen(path) - sizeof(ZERO_INST_INDEX)] = '\0';
	build_ddd_from_index(subindex, path);
	index_free(subindex);

	free(path);
}

static void fetched_archive(Request *request)
{
	UserRequest *first_rq = request->users;
	char path[MAX_PATH_LEN];
	Group *group = NULL;
	Item *item = NULL;
	
	assert(request->state == FETCHING_ARCHIVE);
	assert(request->current_download_path);
	assert(request->current_download_archive);

	if (snprintf(path, sizeof(path), "%s%s/", cache_dir,
	    request->path) > sizeof(path) - 1) {
		fprintf(stderr, "Path too long\n");
		return;
	}
	if (chdir(path)) {
		perror("chdir");
		return;
	}

	index_lookup(request->index, first_rq->leaf, &group, &item);

	assert(item);
	assert(group);
	
	unpack_archive(request->current_download_path, group,
			request->current_download_archive);
	if (unlink(request->current_download_path))
		perror("fetched_archive: unlink");
	chdir("/");
}

/* Start a new fetch. Sets child->pid or request->index on success. */
static void begin_fetch_index(Request *request)
{
	char path[MAX_PATH_LEN];
	char uri[MAX_URI_LEN];
	time_t now;
	struct stat info;

	assert(request->child_pid == -1);

	if (snprintf(path, sizeof(path), "%s%s/" ZERO_INST_INDEX, cache_dir,
	    request->path) > sizeof(path) - 1) {
		fprintf(stderr, "Path too long\n");
		return;
	}

	time(&now);

	/* If the index file already exists and was recently fetched, don't
	 * bother fetching it again.
	 */
	if (lstat(path, &info) == 0 && info.st_ctime + INDEX_CHECK_TIME > now) {
		request->index = parse_index(path);
		if (request->index) {
			printf("\t(using cached index)\n");
			return;
		}
		printf("\t(cached index too old... refetching)\n");
	} else
		printf("\t(fetching index)\n");

	/* Fetch main directory index */
	if (!build_uri(uri, sizeof(uri), request->path, ZERO_INST_INDEX, NULL))
		return;

	request->state = FETCHING_INDEX;
	wget(request, uri, path, 0);
}
#endif

#if 0
/* Start a new fetch. Sets child->pid on success. */
static void begin_fetch_toplevel(Request *request)
{
	UserRequest *first_rq = request->users;
	char uri[MAX_URI_LEN];
	char path[MAX_PATH_LEN];

	/* This is just to save time on some common bogus lookups.
	 * Hostname mustn't start with '.'.
	 */
	if (first_rq->leaf[0] == '.' || strcmp(first_rq->leaf, "AppRun") == 0)
		return;

	if (snprintf(uri, sizeof(uri), "%s://%s/" ZERO_INST_INDEX,
	    request->path + 1, first_rq->leaf) > sizeof(uri) - 1) {
		fprintf(stderr, "URI too long\n");
		return;
	}
	if (snprintf(path, sizeof(path), "%s%s/%s/" ZERO_INST_INDEX,
	    cache_dir, request->path, first_rq->leaf) > sizeof(path) - 1) {
		fprintf(stderr, "Path too long\n");
		return;
	}

	request->state = FETCHING_SUBINDEX;
	wget(request, uri, path, 0);
}

/* Start a new fetch. Sets child->pid on success.
 * Modifies uri.
 */
static void begin_fetch_archive(Request *request, Item *archive)
{
	char uri[MAX_URI_LEN];
	char base[MAX_URI_LEN];
	char path[MAX_PATH_LEN];

	assert(request->child_pid == -1);
	assert(archive->type == 'a');

	printf("\t(fetch archive '%s')\n", archive->leafname);

	request->state = FETCHING_ARCHIVE;
	if (snprintf(path, sizeof(path), "%s%s/archive.tgz",
				cache_dir, request->path) > sizeof(path) - 1) {
		fprintf(stderr, "Path too long\n");
		return;
	}

	if (!build_uri(base, sizeof(base), request->path, NULL, NULL))
		return;

	if (!uri_ensure_absolute(archive->leafname, base, uri, sizeof(uri)))
		return;

	wget(request, uri, path, 0);
	request->current_download_archive = archive;
}

/* Start a new fetch. Sets child->pid on success. */
static void begin_fetch_subindex(Request *request)
{
	UserRequest *first_rq = request->users;
	char path[MAX_PATH_LEN];
	char uri[MAX_URI_LEN];

	assert(request->child_pid == -1);

	if (snprintf(path, sizeof(path), "%s%s/%s" , cache_dir,
			request->path, first_rq->leaf) > sizeof(path) - 1) {
		fprintf(stderr, "Path too long\n");
		return;
	}
	if (!ensure_dir(path))
		return;

	/* Fetch subdirectory index */
	if (!build_uri(uri, sizeof(uri), request->path, first_rq->leaf,
				ZERO_INST_INDEX))
		return;

	if (snprintf(path, sizeof(path),
		"%s%s/%s/" ZERO_INST_INDEX, cache_dir, request->path,
		first_rq->leaf) > sizeof(path) - 1) {
		fprintf(stderr, "Path too long\n");
		return;
	}

	request->state = FETCHING_SUBINDEX;
	wget(request, uri, path, 0);
}

/* We've got the main index, and dealt with any previous fetch already. Now we
 * need to use the index to find out what kind of thing the next leafname is
 * and start fetching that...
 * Sets child_pid on success.
 */
static void begin_fetch(Request *request)
{
	UserRequest *first_rq = request->users;
	Group *group = NULL;
	Item *item = NULL;

	assert(request->child_pid == -1);
	assert(request->n_users);

	printf("\t(%d files remaining in request)\n", request->n_users);

	if (!strchr(request->path + 1, '/')) {
		/* The root of a site (eg, /http/zero-install.sf.net) */
		begin_fetch_toplevel(request);
		return;
	}

	assert(request->index);

	/* It's not the root directory of a site, but a resource within it.
	 * Check the index to find out what kind of object it is and how to get
	 * it.
	 */

	index_lookup(request->index, first_rq->leaf, &group, &item);
	if (!item) {
		fprintf(stderr, "Item '%s' not in index\n", first_rq->leaf);
		return;
	}

	if (item->type == 'd') {
		begin_fetch_subindex(request);
		return;
	}

	if (!group) {
		fprintf(stderr, "Item '%s' has no group!\n", first_rq->leaf);
		return;
	}

	if (!group->archives) {
		fprintf(stderr, "No archives for '%s'\n", first_rq->leaf);
		return;
	}

	assert(item->type == 'f' || item->type == 'x');

	begin_fetch_archive(request, group->archives);
}

/* Called whenever there is no child process running for this request.
 *
 * If we've just finished doing something (eg, fetching a tarball or index)
 * then we deal with that first.
 *
 * Then, if there's nothing more to do, we finish_request().
 * Otherwise, we spawn a new process to perform the next step.
 */
static void request_next_step(Request *request)
{
	printf("Request %s moving forward...\n", request->path);

	if (request->child_pid != -1) {
		fprintf(stderr, "request_next_step: In progress!\n");
		exit(EXIT_FAILURE);
	}

	if (!request->n_users) {
		finish_request(request);
		return;
	}

	/* (don't need an index for top-levels) */
	if (!request->index && strchr(request->path + 1, '/')) {
		/* We don't have a parsed index yet. Either we need to fetch
		 * it, or we've just finished fetching it.
		 */
		if (request->state == READY) {
			/* Must get here before adding more users */
			assert(request->n_users == 1);

			begin_fetch_index(request);
			if (request->index) {
				/* Quite recent... no need to fetch */
			} else if (request->child_pid == -1) {
				/* Tried to fetch and failed */
				finish_request(request);
				return;
			} else {
				/* Fetching from remote server */
				request->state = FETCHING_INDEX;
				control_notify_user(request->users->uid);
				return;
			}
		} else if (request->state == FETCHING_INDEX) {
			char *path = request->current_download_path;
			printf("\t(processing index)\n");

			request->index = parse_index(path);
			if (!request->index) {
				finish_request(request);
				return;
			}
			utime(path, NULL);
			path[strlen(path) - sizeof(ZERO_INST_INDEX)] = '\0';
			/* TODO: might not need to recreate ... file */
			build_ddd_from_index(request->index, path);
			free(path);
			request->current_download_path = NULL;
		}
	}
	
	assert(request->index || !strchr(request->path + 1, '/'));
	
	if (request->state == FETCHING_SUBINDEX) {
		printf("\t(fetched subindex)\n");
		fetched_subindex(request);
		request_done_head(request);
	} else if (request->state == FETCHING_ARCHIVE) {
		printf("\t(fetched archive)\n");
		fetched_archive(request);
		request_done_head(request);
	}

	while (request->n_users) {
		uid_t uid = request->users->uid;

		begin_fetch(request);
		if (request->child_pid != -1) {
			control_notify_user(uid);
			break;
		}
		/* Couldn't start request... skip to next */
		printf("\t(skipping)\n");
		request_done_head(request);
	}

	/* Either we have begun a new request, or there is nothing left to do */

	if (!request->n_users)
		finish_request(request);

	return;
}
#endif

/* We have the index to find the item for this task. Start fetching the
 * item. Free the index and close the request when done.
 */
static void kernel_got_index(Task *task, Index *index)
{
	xmlNode *item;
	const char *slash;

	assert(index);

	slash = strchr(task->str + 1, '/');
	slash = strchr(slash + 1, '/');

	item = index_lookup(index, slash);
	if (!item) {
		/* TODO: rebuild index files? */
		printf("%s not found in index!\n", task->str);
		index_free(index);
		close(task->fd);
		task_destroy(task, 0);
		return;
	}

	if (item->name[0] == 'd')
		fetch_create_directory(task->str, item);
	else
		printf("Todo: fetch archive for %s '%s'\n",
				item->name, task->str);

	index_free(index);
	close(task->fd);
	task_destroy(task, 1);
}

static void kernel_task_step(Task *task, int success)
{
	Index *index = NULL;

	if (success)
		index = get_index(task->str, NULL, 0);

	if (index)
		kernel_got_index(task, index);
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
	Index *index;

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

	index = get_index(path, &task->child_task, 0);
	if (task->child_task)
		return;		/* Download in progress */

	if (!index) {
		/* Error -- give up */
		close(request_fd);
		task_destroy(task, 0);
		return;
	}

	kernel_got_index(task, index);
}

#if 0
/* Fetch 'leaf' in 'path' for user 'uid'. fd is the lazyfs request FD,
 * or -1 for client requests (no notification of cancellations needed).
 * 'path' must be relative to /uri, and already checked for safety.
 * 0 on success. On error, fd is closed.
 * Not for top-levels (path=/ or /http).
 */
int queue_request(const char *path, const char *leaf, uid_t uid, int fd)
{
	Request *request;

	printf("Request %d: Fetch '%s'/'%s' for user %ld\n",
			fd, path, leaf, (long) uid);

	if (path[0] != '/' || strchr(leaf, '/'))
		goto err;

	request = find_request(path);
	if (!request) {
		request = request_new(path);
		if (!request) {
			fprintf(stderr, "%s: Out of memory!\n", prog);
			goto err;
		}
		request->next = open_requests;
		open_requests = request;
	}

	request_add_user(request, fd, uid, leaf);

	if (request->n_users == 1)
		request_next_step(request);
	else
		control_notify_user(uid);

	return 0;
err:
	if (fd != -1)
		close(fd);
	return -1;
}
#endif

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
