#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <expat.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>

#include "global.h"
#include "support.h"
#include "index.h"
#include "fetch.h"
#include "task.h"
#include "zero-install.h"

#define ZERO_INSTALL_INDEX "0install-index.xml"

static int build_ddd_from_index(xmlNode *dir_node, const char *dir);

static void recurse_ddd(xmlNode *item, void *data)
{
	int *retval = data;
	xmlChar *name;

	if (item->name[0] != 'd')
		return;

	if (*retval)
		return;
	
	name = xmlGetNsProp(item, "name", NULL);

	*retval = build_ddd_from_index(item, name);

	xmlFree(name);
}

static void write_item(xmlNode *item, void *data)
{
	FILE *ddd = data;
	xmlChar *size, *mtime, *name;
	char t = item->name[0];

	assert(t == 'd' || t == 'e' || t == 'f' || t == 'l');
	
	size = xmlGetNsProp(item, "size", NULL);
	mtime = xmlGetNsProp(item, "mtime", NULL);
	name = xmlGetNsProp(item, "name", NULL);

	assert(size);
	assert(mtime);
	assert(name);

	fprintf(ddd, "%c %ld %ld %s%c",
		t == 'e' ? 'x' : t,
		atol(size),
		atol(mtime),
		name, 0);

	xmlFree(size);
	xmlFree(mtime);
	xmlFree(name);

	if (t == 'l') {
		xmlChar *target;
		target = xmlGetNsProp(item, "target", NULL);
		fprintf(ddd, "%s%c", target, 0);
		xmlFree(target);
	}
}

/* Create <dir>/... from the children of dir_node, and recurse.
 * Changes cwd.
 * 0 on success.
 */
static int build_ddd_from_index(xmlNode *dir_node, const char *dir)
{
	int retval = -1;
	FILE *ddd = NULL;

	printf("Building ... in %s\n", dir);

	if (chdir(dir))
		goto err;
	
	ddd = fopen("....", "w");
	if (!ddd)
		goto err;
	fprintf(ddd, "LazyFS\n");

	index_foreach(dir_node, write_item, ddd);
	
	if (fclose(ddd))
		goto err;
	if (rename("....", "..."))
		goto err;

	retval = 0;
	index_foreach(dir_node, recurse_ddd, &retval);

	return retval;
err:
	perror("build_ddd_from_index");
	return -1;
}

/* Called with cwd in directory where files have been extracted.
 * Moves each file in 'group' up if everything is correct.
 */
static void pull_up_files(Group *group)
{
#if 0
	Item *item;
	struct stat info;

	printf("\t(unpacked OK)\n");

	for (item = group->items; item; item = item->next) {
		char up[MAX_PATH_LEN] = "../";

		if (lstat(item->leafname, &info)) {
			perror("lstat");
			fprintf(stderr, "'%s' missing from archive\n",
					item->leafname);
			return;
		}

		if (!S_ISREG(info.st_mode)) {
			fprintf(stderr, "'%s' is not a regular file!\n",
					item->leafname);
			return;
		}

		if (info.st_size != item->size) {
			fprintf(stderr, "'%s' has wrong size!\n",
					item->leafname);
			return;
		}

		if (info.st_mtime != item->mtime) {
			fprintf(stderr, "'%s' has wrong mtime!\n",
					item->leafname);
			return;
		}

		if (strlen(item->leafname) > sizeof(up) - 4) {
			fprintf(stderr, "'%s' way too long\n",
					item->leafname);
			return;
		}
		strcpy(up + 3, item->leafname);
		if (rename(item->leafname, up)) {
			perror("rename");
			return;
		}
		/* printf("\t(extracted '%s')\n", item->leafname); */
	}
#endif
}

/* Unpacks the archive, which should contain the file 'leaf'. Uses the
 * group to find out what other files should be there and extract them
 * too. Ensures types, sizes and MD5 sums match.
 * Changes cwd.
 */
void unpack_archive(const char *archive_path, Group *group, Item *archive)
{
#if 0
	int status = 0;
	struct stat info;
	pid_t child;
	const char *argv[] = {"tar", "-xzf", archive_path, NULL};
	
	printf("\t(unpacking '%s')\n", archive_path);

	if (lstat(archive_path, &info)) {
		perror("lstat");
		return;
	}

	if (archive->size != info.st_size) {
		fprintf(stderr, "Downloaded archive has wrong size!\n");
		return;
	}

	printf("\t(TODO: skipping MD5 check)\n");
	
	if (access(".0inst-tmp", F_OK) == 0) {
		fprintf(stderr, "Removing old .0inst-tmp directory\n");
		system("rm -r .0inst-tmp");
	}

	if (mkdir(".0inst-tmp", 0700)) {
		perror("mkdir");
		return;
	}
	
	if (chdir(".0inst-tmp")) {
		perror("chdir");
		return;
	}

	child = fork();
	if (child == -1) {
		perror("fork");
		return;
	}

	if (child == 0) {
		execvp(argv[0], (char **) argv);
		perror("Trying to run tar: execvp");
		_exit(1);
	}

	while (1) {
		if (waitpid(child, &status, 0) != -1 || errno != EINTR)
			break;
	}

	if (!WIFEXITED(status) || WEXITSTATUS(status)) {
		printf("\t(error unpacking archive)\n");
	} else {
		pull_up_files(group);
	}

	chdir("..");
	system("rm -r .0inst-tmp");
#endif
}

/* Begins fetching 'uri', storing the file as 'path'.
 * Sets task->child_pid and task->data to a copy of 'path'.
 * On error, task->child_pid will still be -1.
 */
static void wget(Task *task, const char *uri, const char *path, int use_cache)
{
	const char *argv[] = {"wget", "-q", "-O", NULL, uri,
			use_cache ? NULL : "--cache=off", NULL};
	char *slash;
	char *path_copy;

	printf("Fetch '%s'\n", uri);

	assert(task->child_pid == -1);
	assert(task->data == NULL);

	path_copy = task->data = my_strdup(path);
	if (!path_copy)
		return;
	argv[3] = path_copy;

	slash = strrchr(path_copy, '/');
	assert(slash);

	*slash = '\0';
	if (!ensure_dir(path_copy))
		goto err;
	*slash = '/';

	task->child_pid = fork();
	if (task->child_pid == -1) {
		perror("fork");
		goto err;
	} else if (task->child_pid)
		return;

	execvp(argv[0], (char **) argv);

	perror("Trying to run wget: execvp");
	_exit(1);
err:
	free(path_copy);
	task->data = NULL;
}

void got_site_index(Task *task, int success)
{
	Index *index;
	char *slash;

	assert(task->type == TASK_INDEX);
	assert(task->child_pid == -1);

	if (!success) {
		fprintf(stderr, "Failed to fetch archive\n");
		task_destroy(task, 0);
		return;
	}

	printf("[ got '%s' ]\n", (char *) task->data);
	index = parse_index((char *) task->data);

	if (!index) {
		task_destroy(task, 0);
		return;
	}

	slash = strrchr(task->data, '/');
	*slash = '\0';

	build_ddd_from_index(index_get_root(index), task->data);
	chdir("/");

	task_destroy(task, 1);
}

/* Fetch the index file for the site 'path'.
 * path must be in the form /http/site[/path].
 */
Task *fetch_site_index(const char *path)
{
	Task *task;
	char buffer[MAX_URI_LEN];
	char out[MAX_PATH_LEN];
	const char *slash;
	int cache_len;
	int stem_len;
	int needed;

	assert(path[0] == '/');
	slash = strchr(path + 1, '/');
	assert(slash);
	slash = strchr(slash + 1, '/');

	if (slash)
		stem_len = slash - path;
	else
		stem_len = strlen(path);

	cache_len = strlen(cache_dir);
	/* <cache><stem>/ZERO_INSTALL_INDEX */
	needed = cache_len + stem_len + 1 + sizeof(ZERO_INSTALL_INDEX);
	if (needed > sizeof(out))
		return NULL;

	strcpy(out, cache_dir);
	out[cache_len] = '/';

	memcpy(out + cache_len, path, stem_len);
	out[cache_len + stem_len] = '\0';

	if (!build_uri(buffer, sizeof(buffer),
			out + cache_len, ZERO_INSTALL_INDEX, NULL))
		return NULL;

	strcpy(out + cache_len + stem_len, "/" ZERO_INSTALL_INDEX);

	assert(strlen(out) + 1 == needed);

	task = task_new(TASK_INDEX);
	if (!task)
		return NULL;

	task->step = got_site_index;

	wget(task, buffer, out, 0);
	if (task->child_pid == -1) {
		task_destroy(task, 0);
		task = NULL;
	}

	return task;
}
