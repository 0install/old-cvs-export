#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include <libxml/tree.h>
#include <libxml/relaxng.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "global.h"
#include "index.h"
#include "support.h"
#include "zero-install.h"

#define ZERO_NS "http://zero-install.sourceforge.net"

#define DTD "<?xml version='1.0'?>	\
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
</grammar>"

static xmlRelaxNGValidCtxtPtr schema = NULL;
static xmlRelaxNGParserCtxtPtr context = NULL;
static xmlRelaxNGPtr sc = NULL;

static int dir_valid(xmlNode *dir);

void index_init(void)
{
	assert(!schema);

	context = xmlRelaxNGNewMemParserCtxt(DTD, sizeof(DTD));
	assert(context);

	sc = xmlRelaxNGParse(context);
	assert(sc);

	schema = xmlRelaxNGNewValidCtxt(sc);
}

void index_shutdown(void)
{
	xmlRelaxNGFreeValidCtxt(schema);
	xmlRelaxNGFree(sc);
	xmlRelaxNGFreeParserCtxt(context);
	xmlRelaxNGCleanupTypes();
}

static void count(xmlNode *item, void *data)
{
	int *i = data;

	if (item->name[0] == 'a')
		return;
	assert(item->name[0] == 'f' || item->name[0] == 'e' ||
	       item->name[0] == 'd' || item->name[0] == 'l');

	(*i)++;
}

static void get_names(xmlNode *item, void *data)
{
	xmlChar ***name = data;

	if (item->name[0] == 'a')
		return;

	(*name)[0] = xmlGetNsProp(item, "name", NULL);
	(*name)++;
}

static void valid_subdirs(xmlNode *item, void *data)
{
	int *ok = data;

	if (item->name[0] != 'd')
		return;
	if (!dir_valid(item))
		*ok = 0;
}

int compar(const void *a, const void *b)
{
	const char *aa = * (const char **) a;
	const char *bb = * (const char **) b;

	return strcmp(aa, bb);
}

static int dir_valid(xmlNode *dir)
{
	int i, n = 0;
	int ok = 1;
	xmlChar **names, **name;

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
			fprintf(stderr, "Ilegal leafname '%s'\n", names[i]);
			ok = 0;
		}
		if (i < n - 1 && strcmp(names[i], names[i + 1]) == 0) {
			fprintf(stderr, "Duplicate leafname '%s'\n", names[i]);
			ok = 0;
		}
		xmlFree(names[i]);
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
	xmlNode *node;
	char *path;

	if (!index) {
		fprintf(stderr, "Bad index (missing/bad XML?)\n");
		return 0;
	}
	
	assert(schema);

	if (xmlRelaxNGValidateDoc(schema, index->doc)) {
		fprintf(stderr, "Index file does not validate -- aborting\n");
		return 0;
	}

	node = xmlDocGetRootElement(index->doc);
	path = xmlGetNsProp(node, "path", NULL);
	assert(path);
	if (strncmp(path, MNT_DIR "/", sizeof(MNT_DIR)) != 0) {
		fprintf(stderr,
			"WARNING: Path attribute must start with '%s'\n",
			MNT_DIR);
	}

	if (!dir_valid(index_get_root(index)))
		return 0;

	return 1;
}

static void index_link(Index *index, xmlNode *node)
{
	char *src = NULL, *target = NULL, *mtime = NULL, *size = NULL;
	xmlNode *old = NULL, *new;
	char *leaf;

	src = xmlGetNsProp(node, "src", NULL);
	mtime = xmlGetNsProp(node, "mtime", NULL);
	target = xmlGetNsProp(node, "target", NULL);
	size = xmlGetNsProp(node, "size", NULL);

	if (!src || !target || !mtime || !size) {
		fprintf(stderr, "Missing attribute for <link>\n");
		goto out;
	}

	leaf = strrchr(src, '/');
	if (!leaf)
		goto out;
	leaf++;

	old = index_lookup(index, src);
	if (old) {
		xmlUnlinkNode(old);
		xmlFreeNode(old);
	}

	leaf[-1] = '\0';
	old = index_lookup(index, src);
	if (!old) {
		fprintf(stderr, "Can't override '%s'; doesn't exist!\n", src);
		goto out;
	}

	new = xmlNewNode(old->ns, "link");
	if (!new)
		goto out;

	xmlSetNsProp(new, NULL, "mtime", mtime);
	xmlSetNsProp(new, NULL, "size", size);
	xmlSetNsProp(new, NULL, "target", target);
	xmlSetNsProp(new, NULL, "name", leaf);

	xmlAddChild(old, new);
out:
	if (src)
		xmlFree(src);
	if (mtime)
		xmlFree(mtime);
	if (size)
		xmlFree(size);
	if (target)
		xmlFree(target);
}

static int index_merge_overrides(Index *index, const char *site)
{
	char *links;
	xmlDoc *doc;
	xmlNode *node;

	links = build_string("%s/%h/.0inst-meta/override.xml", cache_dir, site);
	if (!links)
		return 0;

	if (access(links, F_OK) != 0) {
		free(links);
		return 1;	/* no links file; OK */
	}

	doc = xmlParseFile(links);
	free(links);
	if (!doc) {
		fprintf(stderr, "Failed to parse override.xml for '%s'\n",
				site);
		return 0;	/* Corrupt */
	}

	node = xmlDocGetRootElement(doc);

	for (node = node->children; node; node = node->next) {
		if (node->type != XML_ELEMENT_NODE)
			continue;
		if (strcmp(node->name, "link") != 0)
			continue;

		index_link(index, node);
	}

	xmlFreeDoc(doc);

	return 1;
}

/* Load 'pathname' as an XML index file. Returns NULL if document is invalid
 * in any way. Ref-count on return is 1.
 */
Index *parse_index(const char *pathname, int validate, const char *site)
{
	xmlDoc *doc;
	Index *index;

	assert(site);
	assert(strchr(site, '/') == NULL);

	doc = xmlParseFile(pathname);
	if (!doc)
		return NULL;

	index = my_malloc(sizeof(Index));
	if (!index) {
		xmlFreeDoc(doc);
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
		xmlFreeDoc(index->doc);
		free(index);
	}
}

void index_dump(Index *index)
{
	if (!index) {
		printf("No index!\n");
		return;
	}

	printf("Index:\n");
}

void index_foreach(xmlNode *dir,
		   void (*fn)(xmlNode *item, void *data),
		   void *data)
{
	xmlNode *item;

	for (item = dir->children; item; item = item->next) {
		if (item->type != XML_ELEMENT_NODE)
			continue;
		if (strcmp(item->name, "dir") == 0 ||
		    strcmp(item->name, "link") == 0)
			fn(item, data);
		else {
			xmlNode *file;

			assert(strcmp(item->name, "group") == 0);
			for (file = item->children; file; file = file->next) {
				if (file->type != XML_ELEMENT_NODE)
					continue;
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
xmlNode *index_get_root(Index *index)
{
	xmlNode *node;

	node = xmlDocGetRootElement(index->doc);

	for (node = node->children; node; node = node->next) {
		if (node->type == XML_ELEMENT_NODE) {
			assert(strcmp(node->name, "dir") == 0);
			return node;
		}
	}

	assert(0);
	return NULL;
}

typedef struct {
	const char *name;
	int name_len;
	xmlNode *node;
} Info;

void find_child(xmlNode *child, void *data)
{
	Info *info = data;
	xmlChar *name;

	if (info->node)
		return;

	name = xmlGetNsProp(child, "name", NULL);
	assert(name);

	//printf("[ checking '%s' with '%s':%d ]\n",
	//		name, info->name, info->name_len);

	if (strncmp(info->name, name, info->name_len) == 0 &&
					name[info->name_len] == '\0')
		info->node = child;

	xmlFree(name);
}

xmlNode *index_lookup(Index *index, const char *path)
{
	xmlNode *dir;

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
xmlNode *index_find_archive(xmlNode *file)
{
	xmlNode *node;
	
	assert(file->name[0] == 'f' || file->name[0] == 'e');

	for (node = file->parent->children; node; node = node->next) {
		if (strcmp(node->name, "archive") == 0)
			return node;
	}

	assert(0);
	return NULL;
}
