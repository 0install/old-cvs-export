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

/* Load 'pathname' as an XML index file. Returns NULL if document is invalid
 * in any way. Ref-count on return is 1.
 */
Index *parse_index(const char *pathname)
{
	xmlDoc *doc;
	Index *index;

	assert(schema);

	doc = xmlParseFile(pathname);
	if (!doc)
		return NULL;

	if (xmlRelaxNGValidateDoc(schema, doc)) {
		fprintf(stderr, "Index file does not validate -- aborting\n");
		xmlFreeDoc(doc);
		return NULL;
	}

	index = my_malloc(sizeof(Index));
	if (!index) {
		xmlFreeDoc(doc);
		return NULL;
	}

	index->doc = doc;
	index->ref = 1;

	return index;
}

/* Decrement ref count */
void index_free(Index *index)
{
	assert(index->ref > 0);

	index->ref--;
	
	if (!index->ref)
		xmlFreeDoc(index->doc);
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
