#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdlib.h>

#include "global.h"
#include "index.h"
#include "support.h"
#include "zero-install.h"
#include "xml.h"

static int dir_valid(Element *dir);

static void get_names(Element *item, void *data)
{
	const char ***name = data;

	if (strcmp(item->name, "archive") == 0)
		return;

	**name = xml_get_attr(item, "name");
	assert(**name);	/* (checked in count) */
	(*name)++;
}

static void valid_subdirs(Element *item, void *data)
{
	int *ok = data;

	if (item->name[0] != 'd')
		return;
	if (!dir_valid(item))
		*ok = 0;
}

static int compar(const void *a, const void *b)
{
	const char *aa = * (const char **) a;
	const char *bb = * (const char **) b;

	return strcmp(aa, bb);
}

static int item_valid(Element *node)
{
	const char *name = xml_get_attr(node, "name");

	if (!name || !xml_get_attr(node, "mtime") ||
	    !xml_get_attr(node, "size")) {
		error("Missing attribute for item");
		return 0;
	}

	if (*name && strchr(name, '/') == NULL)
		return 1;

	error("Bad leafname");
	return 0;
}

static int count_group(Element *group)
{
	int n_items = 0;
	int n_archives = 0;
	Element *node;

	if (!xml_get_attr(group, "MD5sum") ||
	    !xml_get_attr(group, "size")) {
		error("Missing <group> attribute");
		return -1;
	}

	for (node = group->lastChild; node; node = node->previousSibling) {
		if (strcmp(node->name, "file") == 0 ||
		    strcmp(node->name, "exec") == 0) {
			if (!item_valid(node))
				return -1;
			n_items++;
		} else if (strcmp(node->name, "archive") == 0) {
			if (!xml_get_attr(node, "href")) {
				error("Missing href attribute");
				return -1;
			}
			n_archives++;
		} else {
			error("Unknown element in <group>");
			return -1;
		}
	}

	if (n_archives < 1 || n_items < 1) {
		error("Empty group");
		return -1;
	}

	return n_items;
}

/* Recursively validate a <dir> element */
static int dir_valid(Element *dir)
{
	int i, n = 0;
	int ok = 1;
	char **names, **name;
	Element *node;

	assert(strcmp(dir->name, "dir") == 0);

	if (xml_get_attr(dir, "size") == NULL ||
	    xml_get_attr(dir, "mtime") == NULL) {
		perror("Missing attribute\n");
		return 0;
	}

	/* Check the structure and count the number of items */
	for (node = dir->lastChild; node; node = node->previousSibling) {
		if (strcmp(node->name, "group") == 0) {
			int n_group;
			n_group = count_group(node);
			if (n_group < 1) {
				error("Invalid <group>");
				return 0;
			}
			n += n_group;
		} else if (strcmp(node->name, "dir") == 0) {
			if (!item_valid(node))
				return 0;
			if (!dir_valid(node))
				return 0;
			n++;
		} else if (strcmp(node->name, "link") == 0) {
			if (!item_valid(node) ||
			    !xml_get_attr(node, "target"))
				return 0;
			n++;
		} else {
			error("Unknown element in <dir>");
			return 0;
		}
	}

	if (n == 0)
		return 1;
	if (n < 0)
		return 0;

	name = names = my_malloc(sizeof(char *) * (n + 1));
	names[n - 1] = NULL;
	names[n] = NULL;
	index_foreach(dir, get_names, &name);
	/* Check we counted right */
	assert(names[n] == NULL && names[n - 1] != NULL);

	qsort(names, n, sizeof(char *), compar);

	for (i = 0; i < n; i++) {
		if (!names[i]) {
			ok = 0;
			break;
		}
		if (names[i][0] == '\0' ||
		    strchr(names[i], '/') ||
		    strncmp(names[i], ".0inst-", 7) == 0 ||
		    strcmp(names[i], ".") == 0 ||
		    strcmp(names[i], "..") == 0 ||
		    strcmp(names[i], "...") == 0) {
			error("Ilegal leafname '%s'", names[i]);
			ok = 0;
		}
		if (i < n - 1 && strcmp(names[i], names[i + 1]) == 0) {
			error("Duplicate leafname '%s'", names[i]);
			ok = 0;
		}
	}

	free(names);
	if (!ok)
		return 0;

	index_foreach(dir, valid_subdirs, &ok);

	return ok;
}

/* Check index is valid (doesn't do GPG checks; do that first).
 * 0 on error.
 */
static int index_valid(Index *index)
{
	Element *node;
	const char *path;

	if (!index || !index->doc) {
		error("Bad index (missing/bad XML?)");
		return 0;
	}
	
	node = index->doc;
	if (strcmp(node->name, "site-index") != 0) {
		error("Root should be <site-index>");
		return 0;
	}

	path = xml_get_attr(node, "path");
	if (!path) {
		error("No path attribute.");
		return 0;
	}
	if (strncmp(path, MNT_DIR "/", sizeof(MNT_DIR)) != 0) {
		error("Path attribute must start with '%s'", MNT_DIR);
		return 0;
	}

	node = node->lastChild;
	if (!node || node->previousSibling || strcmp(node->name, "dir") != 0) {
		error("<site-index> must contain only a <dir>");
		return 0;
	}

	if (!dir_valid(node))
		return 0;

	return 1;
}

static void index_link(Index *index, Element *node)
{
	const char *src;
	Element *old = NULL, *new;
	const char *leaf;
	char *dir;
	const char *attrs[] = {
		"mtime", NULL,
		"size", NULL,
		"target", NULL,
		"name", NULL,
		NULL
	};

	src = xml_get_attr(node, "src");

	attrs[1] = xml_get_attr(node, "mtime");
	attrs[3] = xml_get_attr(node, "size");
	attrs[5] = xml_get_attr(node, "target");

	if (!src || !attrs[1] || !attrs[3] || !attrs[5]) {
		error("Missing attribute for <link>");
		return;
	}

	leaf = strrchr(src, '/');
	if (!leaf)
		return;
	leaf++;

	old = index_lookup(index, src);
	if (old)
		xml_destroy_node(old);

	dir = build_string("%d", src);
	old = index_lookup(index, dir);
	if (!old) {
		error("Can't override '%s'; doesn't exist!", dir);
		free(dir);
		return;
	}
	free(dir);

	attrs[7] = leaf;
	new = xml_new_with_attrs("link", attrs);
	if (!new)
		return;

	xml_add_child(old, new);
}

static int index_merge_overrides(Index *index)
{
	char *links;
	Element *doc;
	Element *node;

	links = build_string("%s/%h/.0inst-meta/override.xml", cache_dir,
					index->site);
	if (!links)
		return 0;

	if (access(links, F_OK) != 0) {
		free(links);
		return 1;	/* no links file; OK */
	}

	doc = xml_new(NULL, links);
	free(links);
	if (!doc) {
		error("Failed to parse override.xml for '%s'", index->site);
		return 0;	/* Corrupt */
	}

	for (node = doc->lastChild; node; node = node->previousSibling) {
		if (strcmp(node->name, "link") != 0)
			continue;

		index_link(index, node);
	}

	xml_destroy(doc);

	return 1;
}

/* Load 'pathname' as an XML index file. Returns NULL if document is invalid
 * in any way. Ref-count on return is 1.
 */
Index *parse_index(const char *pathname, int validate, const char *site)
{
	Element *doc;
	Index *index;

	assert(site);
	assert(strchr(site, '/') == NULL);

	doc = xml_new(ZERO_NS, pathname);
	if (!doc)
		return NULL;

	index = my_malloc(sizeof(Index));
	if (!index) {
		xml_destroy(doc);
		return NULL;
	}
	index->doc = doc;
	index->ref = 1;
	index->site = my_strdup(site);

	if (!index->site) {
		index_free(index);
		return NULL;
	}

	if (validate && !index_valid(index)) {
		error("Index for '%s' does not validate!", site);
		index_free(index);
		return NULL;
	}
	
	if (!index_merge_overrides(index)) {
		index_free(index);
		return NULL;
	}

	return index;
}

/* Decrement ref count */
void index_free(Index *index)
{
	assert(index);
	assert(index->ref > 0);

	index->ref--;
	
	if (!index->ref) {
		xml_destroy(index->doc);
		if (index->site)
			free(index->site);
		free(index);
	}
}

void index_foreach(Element *dir,
		   void (*fn)(Element *item, void *data),
		   void *data)
{
	Element *item;

	for (item = dir->lastChild; item; item = item->previousSibling) {
		if (strcmp(item->name, "dir") == 0 ||
		    strcmp(item->name, "link") == 0)
			fn(item, data);
		else {
			Element *file;

			assert(strcmp(item->name, "group") == 0);
			for (file = item->lastChild; file;
					file = file->previousSibling) {
				if (strcmp(file->name, "file") == 0 ||
				    strcmp(file->name, "exec") == 0)
					fn(file, data);
				else
					assert(!strcmp(file->name, "archive"));
			}
		}
	}
}

/* Return /site-index/dir */
Element *index_get_root(Index *index)
{
	Element *node;

	for (node = index->doc->lastChild; node; node = node->previousSibling) {
		if (strcmp(node->name, "dir") == 0)
			return node;
	}

	assert(0);
	return NULL;
}

typedef struct {
	const char *name;
	int name_len;
	Element *node;
} Info;

static void find_child(Element *child, void *data)
{
	Info *info = data;
	const char *name;

	if (info->node)
		return;

	name = xml_get_attr(child, "name");
	assert(name);

	//printf("[ checking '%s' with '%s':%d ]\n",
	//		name, info->name, info->name_len);

	if (strncmp(info->name, name, info->name_len) == 0 &&
					name[info->name_len] == '\0')
		info->node = child;
}

Element *index_lookup(Index *index, const char *path)
{
	Element *dir;

	dir = index_get_root(index);

	while (*path) {
		char *slash;
		Info info;

		/* printf("Looking for %s\n", path); */

		assert(path[0] == '/');
		path++;

		slash = strchr(path, '/');

		info.name = path;
		info.name_len = slash ? slash - path : strlen(path);
		info.node = NULL;
		index_foreach(dir, find_child, &info);

		if (!info.node)
			return NULL;	/* Not found */

		dir = info.node;
		path += info.name_len;
	}

	return dir;
}

/* Find an archive for this file */
Element *index_find_archive(Element *file)
{
	Element *node;
	
	assert(file->name[0] == 'f' || file->name[0] == 'e');

	for (node = file->parentNode->lastChild; node;
			node = node->previousSibling) {
		if (strcmp(node->name, "archive") == 0)
			return node;
	}

	assert(0);
	return NULL;
}
