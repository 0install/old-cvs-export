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
#include "gpg.h"
#include "xml.h"
#include "mirrors.h"

#define TMP_PREFIX ".0inst-tmp-"

#define ZERO_INSTALL_INDEX ".0inst-index.xml"

static void build_ddd_from_index(Element *dir_node, char *dir);

/* Create directory 'path' from 'node' */
void fetch_create_directory(const char *path, Element *node)
{
	char cache_path[MAX_PATH_LEN];
	
	assert(node->name[0] == 'd');

	if (snprintf(cache_path, sizeof(cache_path), "%s%s", cache_dir,
	    path) > sizeof(cache_path) - 1) {
		error("Path too long");
		return;
	}

	build_ddd_from_index(node, cache_path);
	chdir("/");
}

static void recurse_ddd(Element *item, void *data)
{
	char *path = data;
	const char *name;
	int len = strlen(path);

	if (item->name[0] != 'd')
		return;

	name = xml_get_attr(item, "name");

	assert(strchr(name, '/') == NULL);

	if (len + strlen(name) + 2 >= MAX_PATH_LEN) {
		error("Path %s/%s too long", path, name);
		return;
	}

	path[len] = '/';
	strcpy(path + len + 1, name);

	/* TODO: only if path exists? */

	build_ddd_from_index(item, path);

	path[len] = '\0';
}

static void write_item(Element *item, void *data)
{
	FILE *ddd = data;
	const char *size, *mtime, *name;
	char t = item->name[0];

	assert(t == 'd' || t == 'e' || t == 'f' || t == 'l');
	
	size = xml_get_attr(item, "size");
	mtime = xml_get_attr(item, "mtime");
	name = xml_get_attr(item, "name");

	assert(size);
	assert(mtime);
	assert(name);

	fprintf(ddd, "%c %ld %ld %s%c",
		t == 'e' ? 'x' : t,
		atol(size),
		atol(mtime),
		name, 0);

	if (t == 'l') {
		const char *target;
		target = xml_get_attr(item, "target");
		fprintf(ddd, "%s%c", target, 0);
	}
}

/* Create <dir>/... from the children of dir_node, and recurse.
 * Changes dir (MAX_PATH_LEN).
 * 0 on success.
 */
static void build_ddd_from_index(Element *dir_node, char *dir)
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
	error("build_ddd_from_index: %m");
}

/* Called with cwd in directory where files have been extracted.
 * Moves each file in 'group' up if everything is correct.
 */
static void pull_up_files(Element *group)
{
	Element *item;
	struct stat info;
	const char *leaf = NULL;

	if (verbose)
		syslog(LOG_DEBUG, "(unpacked OK)");

	for (item = group->lastChild; item; item = item->previousSibling) {
		char up[MAX_PATH_LEN] = "../";
		const char *size_s, *mtime_s;
		long size, mtime;

		if (item->name[0] == 'a')
			continue;
		assert(item->name[0] == 'f' || item->name[0] == 'e');

		assert(!leaf);
		leaf = xml_get_attr(item, "name");

		size_s = xml_get_attr(item, "size");
		size = atol(size_s);

		mtime_s = xml_get_attr(item, "mtime");
		mtime = atol(mtime_s);

		if (lstat(leaf, &info)) {
			error("lstat: %m ('%s' missing from archive)", leaf);
			return;
		}

		if (!S_ISREG(info.st_mode)) {
			error("'%s' is not a regular file!", leaf);
			return;
		}

		if (info.st_size != size) {
			error("'%s' has wrong size!", leaf);
			return;
		}

		if (info.st_mtime != mtime) {
			error("'%s' has wrong mtime!", leaf);
			return;
		}

		if (strlen(leaf) > sizeof(up) - 4) {
			error("'%s' way too long", leaf);
			return;
		}
		strcpy(up + 3, leaf);
		if (rename(leaf, up)) {
			error("rename: %m");
			return;
		}
		leaf = NULL;
	}
}

/* Unpacks the archive. Uses the group to find out what other files should be
 * there and extract them too. Ensures types, sizes and MD5 sums match.
 * Changes cwd.
 */
static void unpack_archive(const char *archive_path, const char *archive_dir,
				Element *group)
{
	int status = 0;
	struct stat info;
	pid_t child;
	const char *argv[] = {"tar", "-xzf", ".tgz", NULL};
	const char *size, *md5;
	
	if (verbose)
		syslog(LOG_DEBUG, "(unpacking %s)", archive_path);
	argv[2] = archive_path;

	if (chdir(archive_dir)) {
		error("chdir: %m");
		return;
	}

	if (lstat(archive_path, &info)) {
		error("lstat: %m");
		return;
	}

	size = xml_get_attr(group, "size");

	if (info.st_size != atol(size)) {
		error("Downloaded archive has wrong size!");
		return;
	}

	md5 = xml_get_attr(group, "MD5sum");
	if (!check_md5(archive_path, md5)) {
		error("Downloaded archive has wrong MD5 checksum!");
		return;
	}
	
	if (access(".0inst-tmp", F_OK) == 0) {
		/* error("Removing old .0inst-tmp directory"); */
		system("rm -r .0inst-tmp");
	}

	if (mkdir(".0inst-tmp", 0700)) {
		error("mkdir: %m");
		return;
	}
	
	if (chdir(".0inst-tmp")) {
		error("chdir: %m");
		return;
	}

	child = fork();
	if (child == -1) {
		error("fork: %m");
		return;
	}

	if (child == 0) {
		execvp(argv[0], (char **) argv);
		error("Trying to run tar: execvp: %m");
		_exit(1);
	}

	while (1) {
		if (waitpid(child, &status, 0) != -1 || errno != EINTR)
			break;
	}

	if (!WIFEXITED(status) || WEXITSTATUS(status)) {
		error("Error unpacking archive");
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

	syslog(LOG_INFO, "Fetching '%s'", uri);

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
		error("fork: %m");
		goto err;
	} else if (task->child_pid)
		return;

	execvp(argv[0], (char **) argv);

	error("Trying to run wget: execvp: %m");
	_exit(1);
err:
	task_set_string(task, NULL);
}

static void got_archive(Task *task, int success)
{
	if (success) {
		char *dir;

		dir = build_string("%d", task->str);
		if (dir) {
			unpack_archive(task->str, dir, task->data);
			free(dir);
		}
	} else {
		error("Failed to fetch archive (%s)", task->str);
	}

	unlink(task->str);

	task_destroy(task, success);
}

/* 1 on success */
int build_ddds_for_site(Index *index, const char *site)
{
	char path[MAX_PATH_LEN];
	char *dir;

	assert(site);
	assert(index);
	assert(strchr(site, '/') == NULL);

	dir = build_string("%s/%s", cache_dir, site);
	if (!dir)
		return 0;

	if (strlen(dir) + 1 >= MAX_PATH_LEN) {
		error("Path %s too long", dir);
		free(dir);
		return 0;
	}

	strcpy(path, dir);
	free(dir);

	build_ddd_from_index(index_get_root(index), path);

	return 1;
}

/* The index.tgz file is in site's meta directory.
 * Check signatures, validate, unpack and build all ... files.
 * Returns the new index on success, or NULL on failure.
 */
static Index *unpack_site_index(const char *site)
{
	Index *index = NULL;

	assert(strchr(site, '/') == NULL);

	/* Change to meta dir */
	{
		char *meta;

		meta = build_string("%s/%s/" META, cache_dir, site);
		if (!meta)
			goto out;

		if (chdir(meta)) {
			error("chdir: %m");
			free(meta);
			goto out;
		}
		
#if 0
		if (chmod(".", 0700))
			error("chmod: %m");	/* Quiet GPG */
#endif

		free(meta);
	}

	if (system("tar xzf index.tgz -O .0inst-index.xml >index.new")) {
		error("Failed to extract index file");
		goto out;
	}

	if (system("tar xzf index.tgz keyring.pub mirrors.xml index.xml.sig")) {
		error("Failed to extract GPG signature/keyring/mirrors!");
		goto out;
	} else if (gpg_trusted(site) != 1)
		goto out;

	index = parse_index("index.new", 1, site);
	if (!index) {
		if (unlink("index.new"))
			error("unlink: %m");
		goto out;
	}

	assert(index);

	if (rename("index.new", "index.xml")) {
		error("rename: %m");
		index_free(index);
		index = NULL;
		goto out;
	}

	if (!build_ddds_for_site(index, site)) {
		index_free(index);
		index = NULL;
	}

out:
	chdir("/");
	return index;
}

static void got_site_index(Task *task, int success)
{
	assert(task->type == TASK_INDEX);
	assert(task->child_pid == -1);

	if (!success)
		error("Failed to fetch archive");
	else {
		char *site = NULL;

		site = build_string("%h", task->str + cache_dir_len + 1);
		if (site) {
			task_steal_index(task, unpack_site_index(site));
			success = task->index != NULL;
			free(site);
		} else
			success = 0;
	}

	task_destroy(task, success);
}

/* Fetch the index file for 'path'.
 * This fetches the .tgz file, checks it, and then unpacks it.
 */
static Task *fetch_site_index(const char *path, int use_cache)
{
	Task *task = NULL;
	char *tgz = NULL, *uri = NULL;
	char *site_dir = NULL;

	assert(path[0] != '/');

	tgz = build_string("%s/%h/" META "/index.tgz", cache_dir, path);
	if (!tgz)
		goto out;

	for (task = all_tasks; task; task = task->next) {
		if (task->type == TASK_INDEX && strcmp(task->str, tgz) == 0) {
			syslog(LOG_INFO, "Merging with task %d", task->n);
			goto out;
		}
	}
	
	assert(!task);

	uri = build_string("http://%H/.0inst-index.tgz", path);
	if (!uri)
		goto out;

	site_dir = build_string("%s/%h", cache_dir, path);
	if (!site_dir || !ensure_dir(site_dir))
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
	if (site_dir)
		free(site_dir);
	return task;
}

/* Returns the parsed index for site containing 'path'.
 * If the index needs to be fetched (or force is set), returns NULL and returns
 * the task in 'task'. If task is NULL, never starts a task.
 * On error, both will be NULL.
 */
Index *get_index(const char *path, Task **task, int force)
{
	char *index_path;
	struct stat info;

	assert(!task || !*task);

	if (!task)
		force = 0;

	assert(path[0] == '/');
	path++;
	
	if (strcmp(path, "AppRun") == 0 || *path == '.')
		return NULL;	/* Don't waste time looking for these */

	index_path = build_string("%s/%h/" META "/index.xml",
			cache_dir, path);
	if (!index_path)
		return NULL;

#if 0
	if (verbose)
		printf("Index for '%s' is '%s'\n", path, index_path);
#endif

	/* TODO: compare times */
	if (force == 0 && stat(index_path, &info) == 0) {
		Index *index;
		char *site;

		site = build_string("%h", path);
		if (site) {
			index = parse_index(index_path, 0, site);
			free(site);
			if (index) {
				free(index_path);
				return index;
			}
		} else
			task = NULL;	/* OOM */
	}

	if (task)
		*task = fetch_site_index(path, !force);
	free(index_path);

	return NULL;
}

/* file is the cache-relative path of a file in the group.
 * Returns a full path for the new tmp file. The MD5sum is used
 * to make the name unique within the directory.
 * free() the result.
 */
static char *get_tmp_path_for_group(const char *file, Element *group)
{
	const char *md5 = NULL;
	char *tgz;

	md5 = xml_get_attr(group, "MD5sum");
	assert(strlen(md5) == 32);
	assert(strchr(md5, '/') == NULL);

	tgz = build_string("%s%d/" TMP_PREFIX "%s", cache_dir, file, md5);

	return tgz;
}

/* 'file' is the path of a file within the archive */
Task *fetch_archive(const char *file, Element *group, Index *index)
{
	Task *task = NULL;
	char *uri = NULL;
	char *tgz = NULL;

	assert(group->name[0] == 'g');

	uri = mirrors_get_best_url(index->site, xml_get_attr(group, "href"));
	if (!uri)
		goto out;

	tgz = get_tmp_path_for_group(file, group);
	if (!tgz)
		goto out;

	if (verbose)
		printf("Fetch archive as '%s'\n", tgz);
	
	/* Check that we're not already downloading it */
	for (task = all_tasks; task; task = task->next) {
		if (task->type == TASK_ARCHIVE &&
				strcmp(task->str, tgz) == 0) {
			syslog(LOG_INFO, "Merging with task %d", task->n);
			goto out;
		}
	}

	task = task_new(TASK_ARCHIVE);
	if (!task)
		goto out;
	task_set_index(task, index);

	task->step = got_archive;
	wget(task, uri, tgz, 1);
	task->data = group;

	/* Store the size, for progress indicators */
	{
		const char *size_s;
		size_s = xml_get_attr(group, "size");
		task->size = atol(size_s);
	}

	if (task->child_pid == -1) {
		task_destroy(task, 0);
		task = NULL;
	}

out:
	if (uri)
		free(uri);
	if (tgz)
		free(tgz);
	return task;
}
