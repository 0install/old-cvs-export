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
#include <time.h>

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

/* The number of seconds after the user rejects a request during which
 * we will auto-reject identical requests.
 */
#define AUTO_REJECT_PERIOD 5


static char *last_reject_request = NULL;
static uid_t last_reject_user = 0;
static time_t last_reject_time = 0;

static char *wget_log = NULL;

static void build_ddd_from_index(Element *dir_node, char *dir);

/* 0 on success (cwd is changed). */
static int chdir_meta(const char *site)
{
	char *meta;

	assert(strchr(site, '/') == NULL);

	meta = build_string("%s/%s/" META, cache_dir, site);
	if (!meta)
		return 1;

	if (chdir(meta)) {
		error("chdir: %m");
		free(meta);
		return 1;
	}
	
	free(meta);

	return 0;
}

/* Return the index for site. If index does not exist, or signature does
 * not match (index out-of-date), returns NULL.
 */
static IndexP load_index(const char *site)
{
	Index *index = NULL;
	struct stat info;
	char *index_path = NULL;

	assert(strchr(site, '/') == NULL);

	index_path = build_string("%s/%h/" META "/index.xml", cache_dir, site);
	if (!index_path)
		goto out;	/* OOM */

	if (stat(index_path, &info) != 0)
		goto out;	/* Index file doesn't exist */
	
	if (chdir_meta(site))
		goto out;

	if (gpg_trusted(site, "index.xml") != NULL)
		goto out;
		
	index = parse_index(index_path, 0, site);

out:
	if (index_path)
		free(index_path);

	chdir("/");

	return index;
}


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

static void may_rotate_log(void) {
	struct stat log_info;
	char *backup = NULL;

	if (stat(wget_log, &log_info) != 0)
		return;	/* Doesn't exist yet? OK. */

	if (log_info.st_size < 10000)
		return;	/* Nice and small. Keep it. */

	backup = build_string("%s.old", wget_log);
	syslog(LOG_INFO, "Wget log is too big. Backing up as '%s'", backup);
	if (rename(wget_log, backup))
		error("rename:%m");
}

/* Begins fetching 'uri', storing the file as 'path'.
 * Sets task->child_pid and makes task->str a copy of 'path'.
 * On error, task->child_pid will still be -1.
 */
static void wget(Task *task, const char *uri, const char *path, int use_cache)
{
	const char *argv[] = {"wget", "-O", NULL, uri,
			"--tries=3", "-a", wget_log,
			use_cache ? NULL : "--cache=off", NULL};
	char *slash;

	syslog(LOG_INFO, "Fetching '%s'", uri);

	assert(task->child_pid == -1);

	task_set_string(task, path);
	if (!task->str)
		return;
	argv[2] = task->str;

	slash = strrchr(task->str, '/');
	assert(slash);

	*slash = '\0';
	if (!ensure_dir(task->str))
		goto err;
	*slash = '/';

	may_rotate_log();

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

static void got_archive(Task *task, const char *err)
{
	if (!err) {
		char *dir;

		dir = build_string("%d", task->str);
		if (dir) {
			unpack_archive(task->str, dir, task->data);
			free(dir);
		}
	} else {
		/* XXX: maybe the index is too old? force a refresh... */
		error("Failed to fetch archive (%s)", task->str);
	}

	unlink(task->str);

	task_destroy(task, err);
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

/* The index.tar.bz2 file is in site's meta directory.
 * Unpack it. NULL on success, or pointer to error message.
 */
static const char *unpack_site_archive(const char *site)
{
	const char *err = "Error";

	assert(strchr(site, '/') == NULL);

	if (chdir_meta(site))
		return "chdir failed";

	if (system("tar --bzip2 -xf index.tar.bz2 keyring.pub mirrors.xml "
		   "index.xml.sig") == 0)
		err = NULL;
	else
		err = "Failed to extract GPG signature/keyring/mirrors!";

	chdir("/");
	return err;
}

/* The index.xml.bz2 file is in site's meta directory.
 * Check signatures, validates and build all ... files.
 * Returns the new index on success, or NULL on failure (error is set).
 */
static Index *unpack_site_index(const char *site, const char **err)
{
	Index *index = NULL;

	assert(strchr(site, '/') == NULL);
	assert(*err == NULL);

	if (chdir_meta(site)) {
		return NULL;
	}

	if (system("bunzip2 -c index.xml.bz2 >index.new")) {
		*err = "Failed to extract index file";
		goto out;
	} else {
		if (unlink("index.xml.bz2"))
			error("unlink bz2");
	}

	*err = gpg_trusted(site, "index.new");
	if (*err) {
		if (unlink("index.new"))
			error("unlink: %m");
		goto out;
	}

	if (rename("index.new", "index.xml")) {
		error("rename: %m");
		goto out;
	}

	index = load_index(site);
	if (!index) {
		*err = "Index is not valid";
		goto out;
	}

	if (!build_ddds_for_site(index, site)) {
		*err = "Failed to create index files";
		index_free(index);
		index = NULL;
	}

out:
	chdir("/");
	if (!index && !*err)
		*err = "Internal error (check logs)";
	return index;
}

static void got_site_index(Task *task, const char *err)
{
	assert(task->type == TASK_INDEX);
	assert(task->child_pid == -1);

	if (!err) {
		char *site = NULL;

		site = build_string("%h", task->str + cache_dir_len + 1);
		if (site) {
			task_steal_index(task, unpack_site_index(site, &err));
			if (!err && !task->index)
				err = "Failed to load index";
			free(site);
		} else
			err = "Out of memory";
	}

	if (err)
		error("got_site_index: %s", err);

	task_destroy(task, err);
}

/* We've downloaded the index archive, but don't have an up-to-date
 * index.xml. Start fetching that...
 * 1 on success (fetch in progress).
 */
static int fetch_index_file(Task *task, const char *site)
{
	char *uri, *bz;

	uri = mirrors_get_best_url(site, NULL);
	if (!uri)
		return 0;

	bz = build_string("%s/%h/" META "/index.xml.bz2", cache_dir, site);
	if (!bz) {
		free(uri);
		return 0;
	}

	task->step = got_site_index;
	wget(task, uri, bz, 1);
	free(bz);
	free(uri);

	if (task->child_pid == -1)
		return 0;

	return 1;
}

static void got_site_index_archive(Task *task, const char *err)
{
	assert(task->type == TASK_INDEX);
	assert(task->child_pid == -1);

	if (err)
		err = "Failed to fetch index archive";

	if (!err) {
		char *site = NULL;

		site = build_string("%h", task->str + cache_dir_len + 1);
		if (site) {
			err = unpack_site_archive(site);
			if (!err) {
				task_steal_index(task, load_index(site));
				if (!task->index) {
					if (fetch_index_file(task, site))
						return;
					err = "Can't fetch index";
				}
			}
			free(site);
		} else {
			err = "Out of memory";
		}
	}

	if (err)
		error("got_site_index_archive: %s", err);

	task_destroy(task, err);
}

/* Fetch the index archive for 'path'.
 * This fetches the .tar.bz2 file, checks it, and then unpacks it.
 */
static Task *fetch_site_index(const char *path, int use_cache)
{
	Task *task = NULL;
	char *tbz = NULL, *uri = NULL;
	char *site_dir = NULL;

	assert(path[0] != '/');

	tbz = build_string("%s/%h/" META "/index.tar.bz2", cache_dir, path);
	if (!tbz)
		goto out;

	for (task = all_tasks; task; task = task->next) {
		if (task->type == TASK_INDEX && strcmp(task->str, tbz) == 0) {
			syslog(LOG_INFO, "Merging with task %d", task->n);
			goto out;
		}
	}
	
	assert(!task);

	uri = build_string("http://%H/.0inst-index.tar.bz2", path);
	if (!uri)
		goto out;

	site_dir = build_string("%s/%h", cache_dir, path);
	if (!site_dir || !ensure_dir(site_dir))
		goto out;

	task = task_new(TASK_INDEX);
	if (!task)
		goto out;

	task->step = got_site_index_archive;

	wget(task, uri, tbz, use_cache);
	if (task->child_pid == -1) {
		task_destroy(task, "Failed to fork child process");
		task = NULL;
	}

out:
	if (tbz)
		free(tbz);
	if (uri)
		free(uri);
	if (site_dir)
		free(site_dir);
	return task;
}

/* Check that 'site' is a valid site name within /uri.
 * DNS isn't case sensitive, but Unix is. Therefore, we require all
 * lower-case names in addition to the usual restrictions.
 * However, anything after '#' is OK, since it isn't part of the domain.
 *
 * This is mainly for efficiency -- don't bother running wget on invalid
 * names.
 *
 * See: RFC 1034
 *
 * "The labels must follow the rules for ARPANET host names. They must
 * start with a letter, end with a letter or digit, and have as interior
 * characters only letters, digits, and hyphen.  There are also some
 * restrictions on the length. Labels must be 63 characters or less."
 *
 * 'site' is terminated by \0 or '/'.
 */
static int valid_site_name(const char *site)
{
	const char *end;
	int domain_len;
	int i;
	int label_start = 0;

	assert(site);

	/* Must start with [a-z] */
	if (site[0] < 'a' || site[0] > 'z')
		return 0;

	end = strpbrk(site, "#/");
	if (end)
		domain_len = end - site;
	else
		domain_len = strlen(site);

	/* Domain names are limited to 255 characters */
	if (domain_len > 255)
		return 0;

	for (i = 1; i < domain_len; i++) {
		const char c = site[i];

		if (c == '.') {
			if (i == label_start)
				return 0;	/* Zero-length label */
			label_start = i + 1;
			continue;
		}

		if (i - label_start >= 63)
			return 0;	/* Label part too long */

		if (c >= 'a' && c <= 'z')
			continue;	/* Lowercase ASCII OK */

		if (c >= '0' && c <= '9')
			continue;	/* Digits are OK */

		if (c == '-' && i < domain_len - 1)
			continue;	/* '-' OK except at end */

		/* Unknown character. Reject. */
		return 0;
	}

	return 1;	/* OK! */
}

/* Returns the parsed index for site containing 'path'.
 * If the index needs to be fetched (or force is set), returns NULL and returns
 * the task in 'task'. If task is NULL, never starts a task.
 * On error, both will be NULL.
 */
IndexP get_index(const char *path, Task **task, int force)
{
	assert(!task || !*task);

	if (!task)
		force = 0;

	assert(path[0] == '/');
	path++;
	
	if (!valid_site_name(path))
		return NULL;	/* Don't waste time looking for these */

	/* TODO: compare times? */
	if (!force) {
		Index *index;
		char *site;

		site = build_string("%h", path);
		if (!site)
			return NULL;	/* OOM */

		index = load_index(site);
		free(site);

		if (index)
			return index;
	}

	if (task)
		*task = fetch_site_index(path, !force);

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
		task_destroy(task, "Failed to fork child process");
		task = NULL;
	}

out:
	if (uri)
		free(uri);
	if (tgz)
		free(tgz);
	return task;
}

/* User has cancelled fetching 'request'. Reject duplicate requests in
 * future for a few seconds.
 */
void fetch_set_auto_reject(const char *request, uid_t uid)
{
	error("Should reject '%s' for '%d'", request, uid);
	if (last_reject_request)
		free(last_reject_request);
	last_reject_request = my_strdup(request);
	last_reject_user = uid;
	last_reject_time = time(NULL);
}

/* Test whether we should reject this request before even trying to
 * download (eg, because the user already cancelled the same request
 * a few seconds ago.
 */
int fetch_check_auto_reject(const char *request, uid_t uid)
{
	if (last_reject_request && last_reject_user == uid &&
	    strcmp(last_reject_request, request) == 0 &&
	    time(NULL) - last_reject_time < AUTO_REJECT_PERIOD) {
		error("Auto re-rejecting request for '%s'", request);
		return 1;
	}
	return 0;
}

static void test_valid_site_name(void)
{
	assert(valid_site_name("") == 0);
	assert(valid_site_name("a") == 1);
	assert(valid_site_name("z") == 1);
	assert(valid_site_name("0") == 0);
	assert(valid_site_name("`") == 0);
	assert(valid_site_name("{") == 0);

	assert(valid_site_name("AppRun") == 0);
	assert(valid_site_name(".DirIcon") == 0);

	assert(valid_site_name("a.b") == 1);
	assert(valid_site_name("a..b") == 0);
	assert(valid_site_name("a--b") == 1);
	assert(valid_site_name("a--b-") == 0);

	assert(valid_site_name("zero-install.sourceforge.net") == 1);
	assert(valid_site_name("rox.sourceforge.net") == 1);
	assert(valid_site_name("rox.sourceforge.net#~foo.£?") == 1);
	assert(valid_site_name("rox.sourceforge.net~foo.£?") == 0);
	assert(valid_site_name("some name.com") == 0);

	assert(valid_site_name("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
			       "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 1);
	assert(valid_site_name("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
			       "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 0);

	assert(valid_site_name("b.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
			       "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 1);
	assert(valid_site_name("b.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
			       "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 0);

	assert(valid_site_name("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa."
			       "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa."
			       "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa."
			       "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 1);
	assert(valid_site_name("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa."
			       "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa."
			       "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa."
			       "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 1);
}

void fetch_run_tests(void)
{
	test_valid_site_name();
}

void fetch_init(void)
{
	wget_log = build_string("%s/.0inst-wget.log", cache_dir);
	if (!wget_log)
		exit(EXIT_FAILURE);
	syslog(LOG_INFO, "Started: using cache directory '%s'", cache_dir);
	syslog(LOG_INFO, "Network errors are logged to '%s'", wget_log);
}
