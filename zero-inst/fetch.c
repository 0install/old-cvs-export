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

#define ZERO_INSTALL_INDEX ".0inst-index.xml"

#define TMP_NAME ".0inst-archive.tgz"

static int build_ddd_from_index(xmlNode *dir_node, char *dir);

/* Create directory 'path' from 'node' */
void fetch_create_directory(const char *path, xmlNode *node)
{
	char cache_path[MAX_PATH_LEN];
	
	assert(node->name[0] == 'd');

	if (snprintf(cache_path, sizeof(cache_path), "%s%s/", cache_dir,
	    path) > sizeof(cache_path) - 1) {
		fprintf(stderr, "Path too long\n");
		return;
	}

	if (!ensure_dir(cache_path))
		return;

	build_ddd_from_index(node, cache_path);
}

#if 0
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
#endif

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
 * Changes dir (MAX_PATH_LEN).
 * 0 on success.
 */
static int build_ddd_from_index(xmlNode *dir_node, char *dir)
{
	int retval = -1;
	FILE *ddd = NULL;

	printf("Building %s/...\n", dir);

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
	//index_foreach(dir_node, recurse_ddd, &retval);

	return retval;
err:
	perror("build_ddd_from_index");
	return -1;
}

/* Called with cwd in directory where files have been extracted.
 * Moves each file in 'group' up if everything is correct.
 */
static void pull_up_files(xmlNode *group)
{
	xmlNode *item;
	struct stat info;
	xmlChar *leaf = NULL;

	printf("\t(unpacked OK)\n");

	for (item = group->children; item; item = item->next) {
		char up[MAX_PATH_LEN] = "../";
		xmlChar *size_s, *mtime_s;
		long size, mtime;

		if (item->type != XML_ELEMENT_NODE)
			continue;

		if (item->name[0] == 'a')
			continue;
		assert(item->name[0] == 'f' || item->name[0] == 'e');

		assert(!leaf);
		leaf = xmlGetNsProp(item, "name", NULL);
		assert(leaf);

		size_s = xmlGetNsProp(item, "size", NULL);
		size = atol(size_s);
		xmlFree(size_s);

		mtime_s = xmlGetNsProp(item, "mtime", NULL);
		mtime = atol(mtime_s);
		xmlFree(mtime_s);

		if (lstat(leaf, &info)) {
			perror("lstat");
			fprintf(stderr, "'%s' missing from archive\n", leaf);
			goto out;
		}

		if (!S_ISREG(info.st_mode)) {
			fprintf(stderr, "'%s' is not a regular file!\n", leaf);
			goto out;
		}

		if (info.st_size != size) {
			fprintf(stderr, "'%s' has wrong size!\n", leaf);
			goto out;
		}

		if (info.st_mtime != mtime) {
			fprintf(stderr, "'%s' has wrong mtime!\n", leaf);
			goto out;
		}

		if (strlen(leaf) > sizeof(up) - 4) {
			fprintf(stderr, "'%s' way too long\n", leaf);
			goto out;
		}
		strcpy(up + 3, leaf);
		if (rename(leaf, up)) {
			perror("rename");
			goto out;
		}
		/* printf("\t(extracted '%s')\n", leaf); */
		xmlFree(leaf);
		leaf = NULL;
	}
out:
	if (leaf)
		xmlFree(leaf);
}

/* Unpacks the archive. Uses the group to find out what other files should be
 * there and extract them too. Ensures types, sizes and MD5 sums match.
 * Changes cwd.
 */
static void unpack_archive(const char *archive_dir, xmlNode *archive)
{
	int status = 0;
	struct stat info;
	pid_t child;
	const char *argv[] = {"tar", "-xzf", "../" TMP_NAME, NULL};
	xmlChar *size;
	xmlNode *group = archive->parent;
	
	printf("\t(unpacking '%s/" TMP_NAME "')\n", archive_dir);

	if (chdir(archive_dir)) {
		perror("chdir");
		return;
	}

	if (lstat(TMP_NAME, &info)) {
		perror("lstat");
		return;
	}

	size = xmlGetNsProp(group, "size", NULL);

	if (info.st_size != atol(size)) {
		xmlFree(size);
		fprintf(stderr, "Downloaded archive has wrong size!\n");
		return;
	}
	xmlFree(size);

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
	system("rm -rf .0inst-tmp");
}

/* Begins fetching 'uri', storing the file as 'path'.
 * Sets task->child_pid and makes task->str a copy of 'path'.
 * On error, task->child_pid will still be -1.
 */
static void wget(Task *task, const char *uri, const char *path, int use_cache)
{
	const char *argv[] = {"wget", "-q", "-O", NULL, uri,
			use_cache ? NULL : "--cache=off", NULL};
	char *slash;

	printf("Fetch '%s'\n", uri);

	assert(task->child_pid == -1);

	task_set_string(task, path);
	if (!task->str)
		return;
	argv[3] = task->str;

	slash = strrchr(task->str, '/');
	assert(slash);

	*slash = '\0';
	if (!ensure_dir(task->str))
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
	task_set_string(task, NULL);
}

static void got_archive(Task *task, int success)
{
	if (success) {
		task->str[strlen(task->str) - sizeof(TMP_NAME)] = '\0';
		unpack_archive(task->str, task->data);
		task->str[strlen(task->str)] = '/';
	}

	unlink(task->str);

	task_destroy(task, success);
}

static void got_site_index(Task *task, int success)
{
	char *slash;
	char path[MAX_PATH_LEN];
	int dir_len;

	assert(task->type == TASK_INDEX);
	assert(task->child_pid == -1);

	if (!success) {
		fprintf(stderr, "Failed to fetch archive\n");
		task_destroy(task, 0);
		return;
	}

	printf("[ got '%s' ]\n", task->str);
	task_set_index(task, parse_index(task->str));

	if (!task->index) {
		task_destroy(task, 0);
		return;
	}

	slash = strrchr(task->str, '/');
	*slash = '\0';

	dir_len = strlen(task->str) - sizeof(ZERO_INSTALL_INDEX);

	if (dir_len >= MAX_PATH_LEN) {
		task_destroy(task, 0);
		return;
	}

	memcpy(path, task->str, dir_len);
	path[dir_len] = '\0';

	build_ddd_from_index(index_get_root(task->index), path);

	task_destroy(task, 1);
}

/* Fetch the index file 'path' (in the cache).
 * path must be in the form <cache>/http/site/ZERO_INSTALL_INDEX.
 */
static Task *fetch_site_index(const char *path)
{
	Task *task;
	char buffer[MAX_URI_LEN];

	assert(strncmp(path, cache_dir, strlen(cache_dir)) == 0);
	
	if (!build_uri(buffer, sizeof(buffer),
			path + strlen(cache_dir), NULL, NULL))
		return NULL;

	task = task_new(TASK_INDEX);
	if (!task)
		return NULL;

	task->step = got_site_index;

	wget(task, buffer, path, 0);
	if (task->child_pid == -1) {
		task_destroy(task, 0);
		task = NULL;
	}

	return task;
}

/* Returns the parsed index for site containing 'path'.
 * If the index needs to be fetched (or force is set), returns NULL and returns
 * the task in 'task'. If task is NULL, never start a task.
 * On error, both will be NULL.
 */
Index *get_index(const char *path, Task **task, int force)
{
	char index_path[MAX_PATH_LEN];
	int stem_len;
	char *slash;
	int cache_len;
	int needed;
	struct stat info;

	assert(!task || !*task);

	if (!task)
		force = 0;

	assert(path[0] == '/');
	slash = strchr(path + 1, '/');
	assert(slash);
	
	if (strcmp(slash + 1, "AppRun") == 0 || slash[1] == '.')
		return NULL;	/* Don't waste time looking for these */

	slash = strchr(slash + 1, '/');

	if (slash)
		stem_len = slash - path;
	else
		stem_len = strlen(path);

	cache_len = strlen(cache_dir);

	needed = cache_len + stem_len + 1 + sizeof(ZERO_INSTALL_INDEX);
	if (needed >= sizeof(index_path))
		return NULL;

	memcpy(index_path, cache_dir, cache_len);
	memcpy(index_path + cache_len, path, stem_len);
	strcpy(index_path + cache_len + stem_len, "/" ZERO_INSTALL_INDEX);

	assert(strlen(index_path) + 1 == needed);

	printf("Index for '%s' is '%s'\n", path, index_path);

	/* TODO: compare times */
	if (force == 0 && stat(index_path, &info) == 0) {
		Index *index;
		
		index = parse_index(index_path);
		if (index) {
			/* Testing: */
			index_path[cache_len + stem_len] = '\0';
			build_ddd_from_index(index_get_root(index), index_path);
			return index;
		}
	}

	if (task)
		*task = fetch_site_index(index_path);

	return NULL;
}

/* 'file' is the path of a file within the archive */
Task *fetch_archive(const char *file, xmlNode *archive, Index *index)
{
	Task *task = NULL;
	char uri[MAX_URI_LEN];
	char path[MAX_PATH_LEN];
	char *slash;
	int cache_len;
	int stem_len;
	char *relative_uri;
	const char *abs_uri = NULL;

	cache_len = strlen(cache_dir);
	slash = strrchr(file, '/');
	stem_len = slash - file;
	
	if (cache_len + stem_len + sizeof(TMP_NAME) + 1 >= sizeof(path)) {
		fprintf(stderr, "Path %s too long\n", file);
		return NULL;
	}
	
	memcpy(path, cache_dir, cache_len);
	memcpy(path + cache_len, file, stem_len);
	path[cache_len + stem_len] = '\0';

	relative_uri = xmlGetNsProp(archive, "href", NULL);
	assert(relative_uri);

	if (!strstr(relative_uri, "://")) {
		/* Make URI absolute */
		slash = strchr(path + cache_len + 1, '/');
		slash = strchr(slash + 1, '/');
		*slash = '\0';
		
		if (!build_uri(uri, sizeof(uri),
				path + cache_len, relative_uri, NULL))
			goto out;
		abs_uri = uri;

		*slash = '/';
	} else
		abs_uri = relative_uri;

	strcpy(path + cache_len + stem_len, "/" TMP_NAME);

	printf("Fetch archive as '%s'\n", path);

	task = task_new(TASK_ARCHIVE);
	if (!task)
		return NULL;
	task_set_index(task, index);

	task->step = got_archive;
	wget(task, abs_uri, path, 1);
	task->data = archive;

	if (task->child_pid == -1) {
		task_destroy(task, 0);
		task = NULL;
	}

out:
	if (relative_uri)
		xmlFree(relative_uri);
	return task;
}
