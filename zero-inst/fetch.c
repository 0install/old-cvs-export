#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <expat.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

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

void build_ddd_from_index(const char *dir)
{
	Index *index = NULL;
	FILE *ddd = NULL;

	if (chdir(dir)) {
		perror("chdir");
		return;
	}
	
	index = parse_index(ZERO_INST_INDEX);
	if (!index)
		goto err;

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
	if (index)
		index_free(index);
	chdir("/");
}

/* Return the URI of the archive containing 'leaf' by reading the given
 * XML index file.
 * Returns EISDIR if it's a directory, 0 on success or EINVAL for other
 * errors.
 */
int get_item_info(const char *index_path, const char *leaf, char *uri, int len)
{
	Index *index;
	Item *item = NULL;
	Group *group = NULL;
	int a_len;

	/* printf("[ get '%s' ]\n", leaf); */
	index = parse_index(index_path);
	if (!index)
		return EINVAL;

	index_lookup(index, leaf, &group, &item);
	if (!item) {
		fprintf(stderr, "Item '%s' not in index\n", leaf);
		goto err;
	}

	if (item->type == 'd')
		return EISDIR;

	if (!group) {
		fprintf(stderr, "Item '%s' has no group!\n", leaf);
		goto err;
	}

	if (!group->archives) {
		fprintf(stderr, "No archives for '%s'\n", leaf);
		goto err;
	}

	assert(item->type == 'f' || item->type == 'x');

	a_len = strlen(group->archives->leafname) + 1;
	if (a_len > len)
		goto err;

	memcpy(uri, group->archives->leafname, a_len);

	index_free(index);
	return 0;
err:
	if (index)
		index_free(index);
	return EINVAL;
}

/* Unpack the archive, which should contain the file 'leaf'. Use the
 * index file to find out what other files should be there and extract them
 * too. Ensure types, sizes and MD5 sums match.
 */
void unpack_archive(const char *leaf)
{
	printf("TODO: unpacking '%s'; this is just a quick hack!\n", leaf);
	system("tar xzf archive.tgz");
}
