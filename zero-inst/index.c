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

static const char DTD[] = "<?xml version='1.0'?>	\
<grammar xmlns:z='" ZERO_NS "' xmlns='http://relaxng.org/ns/structure/1.0'> \
 <start>								\
  <element name='z:site-index'>				\
   <attribute name='path'/>				\
   <element name='z:dir'>				\
    <ref name='dir'/>				\
   </element>					\
  </element>					\
 </start>					\
 						\
 <define name='dir'>				\
  <attribute name='size'/>				\
  <attribute name='mtime'/>				\
  <zeroOrMore> \
   <choice> \
    <element name='z:dir'>				\
     <ref name='dir'/>					\
     <attribute name='name'/>				\
    </element>						\
    <element name='z:link'>				\
     <ref name='item'/>					\
     <attribute name='target'/>				\
    </element>						\
    <ref name='group'/>					\
   </choice>						\
   </zeroOrMore> 					\
 </define>						\
 <define name='item'>					\
   <attribute name='size'/>				\
   <attribute name='mtime'/>				\
   <attribute name='name'/>				\
 </define>				\
 <define name='group'>			\
  <element name='z:group'>		\
   <attribute name='MD5sum'/>		\
   <attribute name='size'/>		\
   <zeroOrMore>				\
    <choice>				\
     <element name='z:file'><ref name='item'/></element> \
     <element name='z:exec'><ref name='item'/></element> \
    </choice>				\
   </zeroOrMore>		\
   <oneOrMore>			\
    <element name='z:archive'>		\
     <attribute name='href'/>		\
    </element>				\
   </oneOrMore>		\
  </element>		\
 </define>		\
</grammar>";

static int dir_valid(Element *dir);

static void count(Element *item, void *data)
{
	int *i = data;

	if (item->name[0] == 'a')
		return;
	assert(item->name[0] == 'f' || item->name[0] == 'e' ||
	       item->name[0] == 'd' || item->name[0] == 'l');

	(*i)++;
}

static void get_names(Element *item, void *data)
{
	const char ***name = data;

	if (item->name[0] == 'a')
		return;

	(*name)[0] = xml_get_attr(item, "name");
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

static int dir_valid(Element *dir)
{
	int i, n = 0;
	int ok = 1;
	const char **names, **name;

	assert(dir->name[0] == 'd');

	index_foreach(dir, count, &n);

	if (n == 0)
		return 1;

	name = names = my_malloc(sizeof(char *) * n);
	index_foreach(dir, get_names, &name);

	qsort(names, n, sizeof(char *), compar);

	for (i = 0; i < n; i++) {
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
 * 0 on error (already reported).
 */
static int index_valid(Index *index)
{
	//Element *node;
	//char *path;

	return 1;	// XXX

	if (!index) {
		error("Bad index (missing/bad XML?)");
		return 0;
	}
	
#if 0
	assert(schema);

	if (xmlRelaxNGValidateDoc(schema, index->doc)) {
		error("Index file does not validate -- aborting");
		return 0;
	}

	node = xmlDocGetRootElement(index->doc);
	path = xmlGetNsProp(node, "path", NULL);
	assert(path);
	if (strncmp(path, MNT_DIR "/", sizeof(MNT_DIR)) != 0) {
		error("WARNING: Path attribute must start with '%s'",
			MNT_DIR);
	}

	if (!dir_valid(index_get_root(index)))
		return 0;

	return 1;
#endif
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

static int index_merge_overrides(Index *index, const char *site)
{
	char *links;
	Element *doc;
	Element *node;

	links = build_string("%s/%h/.0inst-meta/override.xml", cache_dir, site);
	if (!links)
		return 0;

	if (access(links, F_OK) != 0) {
		free(links);
		return 1;	/* no links file; OK */
	}

	doc = xml_new(NULL, links);
	free(links);
	if (!doc) {
		error("Failed to parse override.xml for '%s'", site);
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

	if (validate && !index_valid(index)) {
		index_free(index);
		return NULL;
	}
	
	if (!index_merge_overrides(index, site)) {
		index_free(index);
		return NULL;
	}

	return index;
}

/* Decrement ref count */
void index_free(Index *index)
{
	assert(index->ref > 0);

	index->ref--;
	
	if (!index->ref) {
		xml_destroy(index->doc);
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
