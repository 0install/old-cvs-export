#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
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

#define TMP_PREFIX ".0inst-tmp-"

#define ZERO_INSTALL_INDEX ".0inst-index.xml"

static void build_ddd_from_index(xmlNode *dir_node, char *dir);

/* Create directory 'path' from 'node' */
void fetch_create_directory(const char *path, xmlNode *node)
{
	char cache_path[MAX_PATH_LEN];
	
	assert(node->name[0] == 'd');

	if (snprintf(cache_path, sizeof(cache_path), "%s%s", cache_dir,
	    path) > sizeof(cache_path) - 1) {
		fprintf(stderr, "Path too long\n");
		return;
	}

	build_ddd_from_index(node, cache_path);
	chdir("/");
}

static void recurse_ddd(xmlNode *item, void *data)
{
	char *path = data;
	xmlChar *name;
	int len = strlen(path);

	if (item->name[0] != 'd')
		return;

	name = xmlGetNsProp(item, "name", NULL);

	assert(strchr(name, '/') == NULL);

	if (len + strlen(name) + 2 >= MAX_PATH_LEN) {
		fprintf(stderr, "Path %s/%s too long\n", path, name);
		return;
	}

	path[len] = '/';
	strcpy(path + len + 1, name);
	xmlFree(name);

	/* TODO: only if path exists? */

	build_ddd_from_index(item, path);

	path[len] = '\0';
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
 * Changes dir (MAX_PATH_LEN).
 * 0 on success.
 */
static void build_ddd_from_index(xmlNode *dir_node, char *dir)
{
	FILE *ddd = NULL;

	/* printf("Building %s/...\n", dir); */

	if (!ensure_dir(dir))
		goto err;

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

	/* TODO: delete old stuff */

	index_foreach(dir_node, recurse_ddd, dir);
	return;
err:
	perror("build_ddd_from_index");
}

/* Called with cwd in directory where files have been extracted.
 * Moves each file in 'group' up if everything is correct.
 */
static void pull_up_files(xmlNode *group)
{
	xmlNode *item;
	struct stat info;
	xmlChar *leaf = NULL;

	if (verbose)
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
static void unpack_archive(const char *archive_path, const char *archive_dir,
				xmlNode *archive)
{
	int status = 0;
	struct stat info;
	pid_t child;
	const char *argv[] = {"tar", "-xzf", ".tgz", NULL};
	xmlChar *size, *md5;
	xmlNode *group = archive->parent;
	
	if (verbose)
		printf("\t(unpacking %s)\n", archive_path);
	argv[2] = archive_path;

	if (chdir(archive_dir)) {
		perror("chdir");
		return;
	}

	if (lstat(archive_path, &info)) {
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

	md5 = xmlGetNsProp(group, "MD5sum", NULL);
	if (!check_md5(archive_path, md5)) {
		xmlFree(md5);
		fprintf(stderr, "Downloaded archive has wrong MD5 checksum!\n");
		return;
	}
	xmlFree(md5);
	
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
			"--tries=3",
			use_cache ? NULL : "--cache=off", NULL};
	char *slash;

	printf("Fetching '%s'\n", uri);

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
		char *dir;

		dir = build_filename("%d", task->str);
		if (dir) {
			unpack_archive(task->str, dir, task->data);
			free(dir);
		}
	} else {
		fprintf(stderr, "Failed to fetch archive\n");
	}

	unlink(task->str);

	task_destroy(task, success);
}

static void got_site_index(Task *task, int success)
{
	char *slash;
	char path[MAX_PATH_LEN];
	Index *index;

	assert(task->type == TASK_INDEX);
	assert(task->child_pid == -1);

	if (!success) {
		fprintf(stderr, "Failed to fetch archive\n");
		task_destroy(task, 0);
		return;
	}

	/* printf("[ got '%s' ]\n", task->str); */

	slash = strrchr(task->str, '/');
	*slash = '\0';

	if (chdir(task->str)) {
		perror("chdir");
		goto err;
	}

	if (system("tar xzf .0inst-index.tgz -O " ZERO_INSTALL_INDEX
				" >.0inst-index.new")) {
		fprintf(stderr, "Failed to unpack index\n");
		goto err;
	}

	fprintf(stderr, "TODO: skipping GPG signature check\n");

	assert(strncmp(task->str, cache_dir, strlen(cache_dir)) == 0);
	assert(!strchr(task->str + strlen(cache_dir) + 1, '/'));
		
	index = parse_index(".0inst-index.new");
	if (!index_valid(index, task->str + strlen(cache_dir) + 1)) {
		index_free(index);
		if (unlink(".0inst-index.new"))
			perror("unlink");
		goto err;
	}

	if (rename(".0inst-index.new", ZERO_INSTALL_INDEX)) {
		perror("rename");
		goto err;
	}

	assert(index);

	task_steal_index(task, index);

	if (strlen(task->str) >= MAX_PATH_LEN) {
		fprintf(stderr, "Path %s too long\n", task->str);
		goto err;
	}

	strcpy(path, task->str);

	build_ddd_from_index(index_get_root(task->index), path);
	chdir("/");

	task_destroy(task, 1);
	return;
err:
	chdir("/");
	task_destroy(task, 0);
}

/* Fetch the index file 'path' (in the cache).
 * path must be in the form <cache>/site/ZERO_INSTALL_INDEX.
 * (this actually fetches the .tgz file, checks it, and then unpacks it
 * to create ZERO_INSTALL_INDEX)
 */
static Task *fetch_site_index(const char *path, int use_cache)
{
	Task *task = NULL;
	char *tgz = NULL, *uri = NULL;

	assert(strncmp(path, cache_dir, strlen(cache_dir)) == 0);

	tgz = build_filename("%r.tgz", path);
	if (!tgz)
		goto out;
	printf("[ archive for '%s' is '%s' ]\n", path, tgz);

	for (task = all_tasks; task; task = task->next) {
		if (task->type == TASK_INDEX && strcmp(task->str, tgz) == 0) {
			fprintf(stderr, "Merging with task %d\n", task->n);
			goto out;
		}
	}
	
	assert(!task);

	uri = build_filename("http://%c", tgz);
	if (!uri)
		goto out;

	task = task_new(TASK_INDEX);
	if (!task)
		goto out;

	task->step = got_site_index;

	wget(task, uri, tgz, use_cache);
	if (task->child_pid == -1) {
		task_destroy(task, 0);
		task = NULL;
	}

out:
	if (tgz)
		free(tgz);
	if (uri)
		free(uri);
	return task;
}

/* Returns the parsed index for site containing 'path'.
 * If the index needs to be fetched (or force is set), returns NULL and returns
 * the task in 'task'. If task is NULL, never starts a task.
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
	
	if (strcmp(path + 1, "AppRun") == 0 || path[1] == '.')
		return NULL;	/* Don't waste time looking for these */

	slash = strchr(path + 1, '/');

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

	if (verbose)
		printf("Index for '%s' is '%s'\n", path, index_path);

	/* TODO: compare times */
	if (force == 0 && stat(index_path, &info) == 0) {
		Index *index;
		
		index = parse_index(index_path);
		if (index)
			return index;
	}

	if (task)
		*task = fetch_site_index(index_path, !force);

	return NULL;
}

/* Decide the URI where the archive is to be downloaded from.
 * file is the cache-relative path of a file in the group.
 * free() the result.
 */
static char *get_uri_for_archive(const char *file, xmlNode *archive)
{
	xmlChar *href;
	char *uri;

	href = xmlGetNsProp(archive, "href", NULL);
	assert(href);

	/* Make URI absolute if needed */
	if (!strstr(href, "://")) {
		uri = build_filename("http://%h/%s", file + 1, href);
	} else
		uri = my_strdup(href);
	xmlFree(href);

	return uri;
}
/* file is the cache-relative path of a file in the group.
 * Returns a full path for the new tmp file. The MD5sum is used
 * to make the name unique within the directory.
 * free() the result.
 */
static char *get_tmp_path_for_group(const char *file, xmlNode *group)
{
	xmlChar *md5 = NULL;
	char *tgz;

	md5 = xmlGetNsProp(group, "MD5sum", NULL);
	assert(strlen(md5) == 32);
	assert(strchr(md5, '/') == NULL);

	tgz = build_filename("%s%d/" TMP_PREFIX "%s", cache_dir, file, md5);
	xmlFree(md5);

	return tgz;
}

/* 'file' is the path of a file within the archive */
Task *fetch_archive(const char *file, xmlNode *archive, Index *index)
{
	Task *task = NULL;
	char *uri = NULL;
	char *tgz = NULL;

	uri = get_uri_for_archive(file, archive);
	tgz = get_tmp_path_for_group(file, archive->parent);

	if (!tgz || !uri)
		goto out;

	if (verbose)
		printf("Fetch archive as '%s'\n", tgz);
	
	/* Check that we're not already downloading it */
	for (task = all_tasks; task; task = task->next) {
		if (task->type == TASK_ARCHIVE &&
				strcmp(task->str, tgz) == 0) {
			fprintf(stderr, "Merging with task %d\n", task->n);
			goto out;
		}
	}

	task = task_new(TASK_ARCHIVE);
	if (!task)
		goto out;
	task_set_index(task, index);

	task->step = got_archive;
	wget(task, uri, tgz, 1);
	task->data = archive;

	/* Store the size, for progress indicators */
	{
		xmlChar *size_s;
		size_s = xmlGetNsProp(archive->parent, "size", NULL);
		task->size = atol(size_s);
		xmlFree(size_s);
	}

	if (task->child_pid == -1) {
		task_destroy(task, 0);
		task = NULL;
	}

out:
	if (uri)
		xmlFree(uri);
	if (tgz)
		xmlFree(tgz);
	return task;
}
