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

struct _Index {
	Item *root;
};

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
 * in any way.
 */
Index *parse_index(const char *pathname)
{
	xmlDoc *doc;

	assert(schema);

	doc = xmlParseFile(pathname);
	if (!doc)
		return NULL;

	if (xmlRelaxNGValidateDoc(schema, doc)) {
		fprintf(stderr, "Index file does not validate -- aborting\n");
		xmlFreeDoc(doc);
		return NULL;
	}

	return doc;
}

void index_free(Index *index)
{
	xmlFreeDoc(index);
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

	node = xmlDocGetRootElement(index);

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

		printf("Looking for %s\n", path);

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


#if 0
typedef enum {ERROR, DOC, SITE, DIRECTORY, GROUP,
	      ITEM, GROUP_ITEM, ARCHIVE} ParseState;

struct _SiteIndex {
	Item		*root;

	ParseState	state;	/* Used during parsing */
	int		skip;	/* >0 => wait for end tag */
	Item		*current;
};

static const XML_Char *get_attr(const XML_Char **atts, const char *name)
{
	while (*atts) {
		if (strcmp(name, atts[0]) == 0)
			return atts[1];
		atts += 2;
	}

	return NULL;
}

static long get_long_attr(const XML_Char **atts, const char *name)
{
	const XML_Char *value;

	value = get_attr(atts, name);
	if (!value)
		return -1;
	return atol(value);
}

static void item_free(Item *item)
{
	if (item->target)
		free(item->target);
	if (item->leafname)
		free(item->leafname);
	free(item);
}

static void start_item(SiteIndex *site, const XML_Char *type,
			const XML_Char **atts)
{
	Item *new;
	long size = -1;
	long mtime = -1;
	const XML_Char *name = NULL, *href = NULL;
	Item *index = site->current;

	if (type[0] != 'a') {
		size = get_long_attr(atts, "size");
		mtime = get_long_attr(atts, "mtime");
		name = get_attr(atts, "name");
		if (size < 0 || mtime < 0 || !name) {
			site->state = ERROR;
			return;
		}
	} else {
		href = get_attr(atts, "href");
		if (!href)
			return;
	}

	new = my_malloc(sizeof(Item));
	if (!new) {
		site->state = ERROR;
		return;
	}
	
	new->leafname = name ? my_strdup(name) : NULL;
	new->target = NULL;
	new->mtime = mtime;
	new->size = size;
	new->parent = site->current;

	if (type[0] == 'd') {
		site->state = DIRECTORY;
		site->current = new;
	} else if (type[0] == 'l') {
		new->type = type[0];
		new->next = index->no_data;
		index->no_data = new;
		site->state = ITEM;
		if (new->type == 'l') {
			const XML_Char *target;
			target = get_attr(atts, "target");
			if (target)
				new->target = my_strdup(target);
			/* (will check for not found / OOM later) */
		}
	} else if (type[0] == 'f' || type[0] == 'e') {
		new->type = type[0] == 'f' ? 'f' : 'x';
		new->next = index->groups->items;
		index->groups->items = new;
		site->state = GROUP_ITEM;
	} else if (type[0] == 'a') {
		new->type = 'a';
		new->next = index->groups->archives;
		new->target = my_strdup(href);	/* Check OOM later */
		index->groups->archives = new;
		site->state = ARCHIVE;
	} else {
		site->state = ERROR;
		item_free(new);
	}
}

static void start_element(void *userData, const XML_Char *name,
			  const XML_Char **atts)
{
	SiteIndex *site = userData;
	Index *index = site->current;

	if (site->state == ERROR)
		return;

	if (site->skip > 0) {
		printf("Skipping '%s'\n", name);
		site->skip++;
		return;
	}

	/* printf("> %s\n", name); */

	if (site->state == DOC) {
		if (strcmp(name, ZERO_NS " site-index") != 0) {
			fprintf(stderr, "Bad root element (%s)\n", name);
			site->state = ERROR;
			return;
		}
		site->state = SITE;
		return;
	}

	if (strncmp(name, ZERO_NS " ", sizeof(ZERO_NS)) != 0) {
		fprintf(stderr, "Skipping unknown element '%s'\n", name);
		site->skip = 1;
		return;
	}

	name += sizeof(ZERO_NS);

	if (site->state == SITE) {
		if (strcmp(name, "dir") == 0) {
			assert(site->root == NULL);
			site->root = index_new();
			if (!site->root) {
				site->state = ERROR;
				return;
			}
			site->current = site->root;
			site->state = DIRECTORY;
			return;
		}
	}

	if (site->state == DIRECTORY) {
		/* Expecting a group, dir or link */
		if (strcmp(name, "dir") == 0 ||
		    strcmp(name, "link") == 0) {
			start_item(site, name, atts);
			return;
		}
		if (strcmp(name, "group") == 0) {
			Group *new;
			char *md5;
			off_t size;

			md5 = (char *) get_attr(atts, "MD5sum");
			size = get_long_attr(atts, "size");
			if (md5)
				md5 = my_strdup(md5);
			if (size <= 0 || !md5) {
				site->state = ERROR;
				return;
			}
			new = my_malloc(sizeof(Group));
			if (!new) {
				site->state = ERROR;
				free(md5);
				return;
			}
			new->items = NULL;
			new->archives = NULL;
			new->next = index->groups;
			new->md5 = md5;
			index->groups = new;

			new->size = size;

			site->state = GROUP;
			return;
		}
	} else if (site->state == GROUP) {
		if (strcmp(name, "file") == 0 ||
		    strcmp(name, "exec") == 0 ||
		    strcmp(name, "archive") == 0) {
			start_item(site, name, atts);
			return;
		}
	}

	fprintf(stderr, "Unknown element '%s'\n", name);
	site->skip = 1;
}

static void end_element(void *userData, const XML_Char *name)
{
	SiteIndex *site = userData;

	if (site->state == ERROR) {
		/* printf("Skipping </%s> due to earlier errors\n", name); */
		return;
	}

	if (site->skip > 0) {
		site->skip--;
		return;
	}

	/* printf("Exit %s in state %d\n", name, site->state); */

	if (site->state == ITEM)
		site->state = DIRECTORY;
	else if (site->state == GROUP)
		site->state = DIRECTORY;
	else if (site->state == DIRECTORY) {
		site->current = site->current->parent;
		if (site->current)
			site->state = DIRECTORY;
		else
			site->state = SITE;
	} else if (site->state == SITE)
		site->state = DOC;
	else if (site->state == GROUP_ITEM || site->state == ARCHIVE)
		site->state = GROUP;
	else
		printf("Unknown exit from '%s'\n", name);
}

static void free_items(Item *item)
{
	while (item) {
		Item *old = item;
		
		item = item->next;

		item_free(old);
	}
}

void index_free(Index *index)
{
	Group *group = index->groups;
	Index *subdir = index->subdirs;

	free_items(index->no_data);

	while (group) {
		Group *old = group;
		
		group = group->next;

		if (old->md5)
			free(old->md5);

		free_items(old->archives);
		free_items(old->items);
		free(old);
	}

	while (subdir) {
		Index *old = subdir;
		
		subdir = subdir->next_sibling;

		index_free(old);
	}

	if (index->dirname)
		free(index->dirname);

	free(index);
}

static int compar(const void *a, const void *b)
{
	Item *aa = *(Item **) a;
	Item *bb = *(Item **) b;
	int retval;

	retval = strcmp(aa->leafname, bb->leafname);
	
	if (retval)
		return retval;

	assert(aa != bb);

	return aa > bb ? 1 : -1;	/* Stable sort */
}

static int index_valid(SiteIndex *site)
{
	int i = 0;
	int n = 0;
	Item *item;
	Group *group;
	Item **array;
	Index *index = site->root; 	/* XXX */

	if (site->state == ERROR || !index)
		return 0;

	for (item = index->no_data; item; item = item->next) {
		if (!item->leafname)
			return 0;
		if (item->type != 'd' && item->type != 'l')
			return 0;
		if (item->type == 'l' && !item->target)
			return 0;
		if (strchr(item->leafname, '/'))
			return 0;
		n++;
	}

	for (group = index->groups; group; group = group->next) {
		for (item = group->items; item; item = item->next) {
			if (!item->leafname)
				return 0;
			if (item->type != 'f' && item->type != 'x')
				return 0;
			if (strchr(item->leafname, '/'))
				return 0;
			n++;
		}
		for (item = group->archives; item; item = item->next) {
			if (!item->target)
				return 0;
		}
	}

	array = my_malloc(sizeof(Item *) * n);
	if (!array)
		return 0;

	for (item = index->no_data; item; item = item->next) {
		array[i] = item;
		i++;
	}
	for (group = index->groups; group; group = group->next) {
		for (item = group->items; item; item = item->next) {
			array[i] = item;
			i++;
		}
	}

	assert(i == n);

	qsort(array, n, sizeof(Item *), compar);

	for (i = 0; i < n - 1; i++) {
		if (strcmp(array[i]->leafname, array[i + 1]->leafname) == 0) {
			fprintf(stderr, "Duplicate: %s\n",
					array[i]->leafname);
			free(array);
			return 0;
		}
	}

	free(array);

	return 1;
}

void site_index_free(SiteIndex *site)
{
	if (site->root)
		index_free(site->root);
	free(site);
}

/* Load and parse the XML index file 'path'. NULL on error. */
SiteIndex *parse_index(const char *path)
{
	XML_Parser parser = NULL;
	SiteIndex *site = NULL;
	int src = -1;

	site = my_malloc(sizeof(SiteIndex));
	if (!site)
		goto err;

	parser = XML_ParserCreateNS(NULL, ' ');
	if (!parser)
		goto err;
	XML_SetUserData(parser, site);

	site->root = NULL;
	site->current = NULL;
	site->state = DOC;
	site->skip = 0;

	XML_SetStartElementHandler(parser, start_element);
	XML_SetEndElementHandler(parser, end_element);

	src = open(path, O_RDONLY);
	if (src == -1) {
		perror("open");
		goto err;
	}

	for (;;) {
		char buffer[256];
		int got;
		
		got = read(src, buffer, sizeof(buffer));
		if (got < 0) {
			perror("read");
			goto err;
		}
		if (!XML_Parse(parser, buffer, got, got == 0)) {
			fprintf(stderr, "Invalid XML\n");
			goto err;
		}
		if (site->state == ERROR)
			goto err;
		if (got == 0)
			break;
	}

	if (index_valid(site))
		goto out;
	fprintf(stderr, "Validation failed\n");

err:
	fprintf(stderr, "parse_index: failed\n");
	if (site)
		site_index_free(site);
	site = NULL;
out:
	if (parser)
		XML_ParserFree(parser);
	if (src != -1)
		close(src);
	return site;
}

static void in(int indent)
{
	while (indent--)
		fputc('\t', stderr);
}

static void index_dump(Index *index, int indent)
{
	Index *subdir;
	Group *group;
	Item *item;

	if (!index) {
		fprintf(stderr, "index_dump: no index!\n");
		return;
	}

	in(indent);
	fprintf(stderr, "Index: %s\n", index->dirname ? index->dirname : "/");

	for (group = index->groups; group; group = group->next) {
		fprintf(stderr, "\tGroup: %ld (%s)\n", group->size, group->md5);
		for (item = group->items; item; item = item->next) {
			in(indent);
			fprintf(stderr, "\t\t%c : %s : %ld : %s",
				item->type,
				item->leafname ? item->leafname : "(null)",
				item->size,
				ctime(&item->mtime));
		}
		for (item = group->archives; item; item = item->next) {
			in(indent);
			fprintf(stderr, "\t\t%c : %s\n",
				item->type,
				item->target ? item->target : "(null)");
		}
	}

	for (item = index->no_data; item; item = item->next) {
		in(indent);
		fprintf(stderr, "\t%c : %s : %ld : %s",
				item->type,
				item->leafname ? item->leafname : "(null)",
				item->size,
				ctime(&item->mtime));
	}

	for (subdir = index->subdirs; subdir; subdir = subdir->next_sibling) {
		index_dump(subdir, indent + 1);
	}
}

void site_index_dump(SiteIndex *site)
{
	index_dump(site->root, 0);
}

void index_foreach(Index *index,
		   void (*fn)(Item *item, void *data),
		   void *data)
{
	Item *item;
	Group *group;

	for (group = index->groups; group; group = group->next) {
		for (item = group->items; item; item = item->next) {
			fn(item, data);
		}
	}

	for (item = index->no_data; item; item = item->next) {
		fn(item, data);
	}
}

void index_lookup(Index *index, const char *leaf,
		  Group **group_ret, Item **item_ret)
{
	Item *item;
	Group *group;

	for (group = index->groups; group; group = group->next) {
		for (item = group->items; item; item = item->next) {
			if (strcmp(item->leafname, leaf) == 0) {
				*group_ret = group;
				*item_ret = item;
				return;
			}
		}
	}

	*group_ret = NULL;

	for (item = index->no_data; item; item = item->next) {
		if (strcmp(item->leafname, leaf) == 0) {
			*item_ret = item;
			return;
		}
	}

	*item_ret = NULL;
}
#endif
