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

#include "global.h"
#include "support.h"
#include "index.h"
#include "fetch.h"
#include "zero-install.h"

static void write_item(Item *item, void *data)
{
	FILE *ddd = data;

	fprintf(ddd, "%c %ld %ld %s%c",
		item->type, item->size, item->mtime, item->leafname, 0);

	if (item->target)
		fprintf(ddd, "%s%c", item->target, 0);
}

void build_ddd_from_index(Index *index, const char *dir)
{
	FILE *ddd = NULL;

	if (chdir(dir)) {
		perror("chdir");
		return;
	}
	
	ddd = fopen("....", "w");
	if (!ddd)
		goto err;
	fprintf(ddd, "LazyFS\n");

	index_foreach(index, write_item, ddd);
	
	if (fclose(ddd))
		goto err;
	if (rename("....", "..."))
		goto err;
	goto out;
err:
	perror("build_ddd_from_index");
out:
	chdir("/");
}

/* Called with cwd in directory where files have been extracted.
 * Moves each file in 'group' up if everything is correct.
 */
static void pull_up_files(Group *group)
{
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
}

/* Unpacks the archive, which should contain the file 'leaf'. Uses the
 * group to find out what other files should be there and extract them
 * too. Ensures types, sizes and MD5 sums match.
 * Changes cwd.
 */
void unpack_archive(const char *archive_path, Group *group, Item *archive)
{
	int status = 0;
	struct stat info;
	pid_t child;
	const char *argv[] = {"tar", "-xzf", archive_path, NULL};
	
	printf("\t(unpacking '%s')\n", archive_path);

	if (lstat(archive_path, &info)) {
		perror("lstat");
		return;
	}

	if (archive->size == -1)
		printf("[ note: no size in index; skipping check ]\n");
	else if (archive->size != info.st_size) {
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
}
