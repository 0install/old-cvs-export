#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include <expat.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "global.h"
#include "index.h"
#include "support.h"
#include "zero-install.h"

#define ZERO_NS "http://zero-install.sf.net"

typedef enum {ERROR, DOC, DIRECTORY, GROUP,
	      ITEM, GROUP_ITEM, ARCHIVE} ParseState;

struct _Index {
	Group *groups;	/* Linked list of groups */
	Item *no_data;	/* Non-grouped items (links and directories) */

	ParseState	state;	/* Used during parsing */
	int		skip;	/* >0 => wait for end tag */
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

static void char_data(void *userData, const XML_Char *s, int len)
{
	Index *index = userData;
	Item *current;
	int old_len = 0;
	char *new = NULL;

	if (index->skip)
		return;

	if (index->state == ITEM)
		current = index->no_data;
	else if (index->state == GROUP_ITEM)
		current = index->groups->items;
	else if (index->state == ARCHIVE)
		current = index->groups->archives;
	else
		return;

	assert(current);

	if (current->leafname) {
		old_len = strlen(current->leafname);
		new = my_realloc(current->leafname, old_len + len + 1);
	} else
		new = my_malloc(len + 1);

	if (!new) {
		index->state = ERROR;
		return;
	}

	current->leafname = new;
	memcpy(current->leafname + old_len, s, len);
	current->leafname[old_len + len] = '\0';
}

static void item_free(Item *item)
{
	if (item->md5)
		free(item->md5);
	if (item->target)
		free(item->target);
	if (item->leafname)
		free(item->leafname);
	free(item);
}

static void start_item(Index *index, const XML_Char *type,
			const XML_Char **atts)
{
	Item *new;
	long size = -1;
	long mtime = -1;

	size = get_long_attr(atts, "size");
	if (type[0] != 'a') {
		mtime = get_long_attr(atts, "mtime");
		if (size < 0 || mtime < 0) {
			index->state = ERROR;
			return;
		}
	} else if (size == 0) {
		index->state = ERROR;
		return;		/* -1 is OK, though, for compatibility */
	}

	new = my_malloc(sizeof(Item));
	if (!new) {
		index->state = ERROR;
		return;
	}
	new->leafname = NULL;
	new->target = NULL;
	new->md5 = NULL;
	new->mtime = mtime;
	new->size = size;

	if (type[0] == 'd' || type[0] == 'l') {
		new->type = type[0];
		new->next = index->no_data;
		index->no_data = new;
		index->state = ITEM;
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
		index->state = GROUP_ITEM;
	} else if (type[0] == 'a') {
		const XML_Char *md5;

		new->type = 'a';
		md5 = get_attr(atts, "MD5sum");
		if (md5)
			new->md5 = my_strdup(md5);
		new->next = index->groups->archives;
		index->groups->archives = new;
		index->state = ARCHIVE;
	} else {
		index->state = ERROR;
		item_free(new);
	}
}

static void start_element(void *userData, const XML_Char *name,
			  const XML_Char **atts)
{
	Index *index = userData;

	if (index->state == ERROR)
		return;

	if (index->skip > 0) {
		printf("Skipping '%s'\n", name);
		index->skip++;
		return;
	}

	if (index->state == DOC) {
		if (strcmp(name, ZERO_NS " directory") != 0) {
			fprintf(stderr, "Bad root element (%s)\n", name);
			index->state = ERROR;
			return;
		}
		index->state = DIRECTORY;
		return;
	}

	if (strncmp(name, ZERO_NS " ", sizeof(ZERO_NS)) != 0) {
		fprintf(stderr, "Skipping unknown element '%s'\n", name);
		index->skip = 1;
		return;
	}

	name += sizeof(ZERO_NS);

	if (index->state == DIRECTORY) {
		/* Expecting a group, dir or link */
		if (strcmp(name, "dir") == 0 ||
		    strcmp(name, "link") == 0) {
			start_item(index, name, atts);
			return;
		}
		if (strcmp(name, "group") == 0) {
			Group *new;
			new = my_malloc(sizeof(Group));
			if (!new) {
				index->state = ERROR;
				return;
			}
			new->items = NULL;
			new->archives = NULL;
			new->next = index->groups;
			index->groups = new;

			index->state = GROUP;
			return;
		}
	} else if (index->state == GROUP) {
		if (strcmp(name, "file") == 0 ||
		    strcmp(name, "exec") == 0 ||
		    strcmp(name, "archive") == 0) {
			start_item(index, name, atts);
			return;
		}
	}

	fprintf(stderr, "Unknown element '%s'\n", name);
	index->skip = 1;
}

static void end_element(void *userData, const XML_Char *name)
{
	Index *index = userData;

	if (index->state == ERROR)
		return;

	if (index->skip > 0) {
		index->skip--;
		return;
	}

	if (index->state == ITEM)
		index->state = DIRECTORY;
	else if (index->state == GROUP)
		index->state = DIRECTORY;
	else if (index->state == DIRECTORY)
		index->state = DOC;
	else if (index->state == GROUP_ITEM || index->state == ARCHIVE)
		index->state = GROUP;
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

	free_items(index->no_data);

	while (group) {
		Group *old = group;
		
		group = group->next;

		free_items(old->archives);
		free_items(old->items);
		free(old);
	}

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

static int index_valid(Index *index)
{
	int i = 0;
	int n = 0;
	Item *item;
	Group *group;
	Item **array;
	
	if (index->state == ERROR)
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

/* Load and parse the XML index file 'path'. NULL on error. */
Index *parse_index(const char *path)
{
	XML_Parser parser = NULL;
	Index *index = NULL;
	int src = -1;

	index = my_malloc(sizeof(Index));
	if (!index)
		goto err;

	parser = XML_ParserCreateNS(NULL, ' ');
	if (!parser)
		goto err;
	XML_SetUserData(parser, index);

	index->groups = NULL;
	index->no_data = NULL;
	index->state = DOC;
	index->skip = 0;

	XML_SetStartElementHandler(parser, start_element);
	XML_SetEndElementHandler(parser, end_element);
	XML_SetCharacterDataHandler(parser, char_data);

	src = open(path, O_RDONLY);
	if (src == -1) {
		perror("open");
		goto err;
	}

	for (;;) {
		char buffer[256];
		int got;
		
		got = read(src, buffer, sizeof(buffer));
		if (got < 0)
			goto err;
		if (!XML_Parse(parser, buffer, got, got == 0)) {
			fprintf(stderr, "Invalid XML\n");
			goto err;
		}
		if (index->state == ERROR)
			goto err;
		if (got == 0)
			break;
	}

	if (index_valid(index))
		goto out;

err:
	fprintf(stderr, "parse_index: failed\n");
	if (index)
		index_free(index);
	index = NULL;
out:
	if (parser)
		XML_ParserFree(parser);
	if (src != -1)
		close(src);
	return index;
}

void index_dump(Index *index)
{
	Group *group;
	Item *item;

	if (!index) {
		fprintf(stderr, "index_dump: no index!\n");
		return;
	}

	fprintf(stderr, "Index:\n");

	for (group = index->groups; group; group = group->next) {
		fprintf(stderr, "\tGroup:\n");
		for (item = group->items; item; item = item->next) {
			fprintf(stderr, "\t\t%c : %s : %ld : %s",
				item->type,
				item->leafname ? item->leafname : "(null)",
				item->size,
				ctime(&item->mtime));
		}
		for (item = group->archives; item; item = item->next) {
			fprintf(stderr, "\t\t%c : %s (%s) [ %ld ]\n",
				item->type,
				item->leafname ? item->leafname : "(null)",
				item->md5 ? item->md5 : "(no MD5)",
				item->size);
		}
	}

	for (item = index->no_data; item; item = item->next) {
		fprintf(stderr, "\t%c : %s : %ld : %s",
				item->type,
				item->leafname ? item->leafname : "(null)",
				item->size,
				ctime(&item->mtime));
	}
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
